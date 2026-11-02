// coop/world/balance_sync.cpp -- see coop/world/balance_sync.h.

#include "coop/world/balance_sync.h"

#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady: an earn report waits for our own edge
#include "ue_wrap/world/economy.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::balance_sync {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::economy;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host broadcast dedup -- game-thread only (Tick host branch). Sentinel via a separate
// flag so any int32 Points value (including 0 / negative) is a legitimate balance.
bool    g_haveSent = false;
int32_t g_lastSent = 0;

// Client mirror: the latest host balance to apply, RETRIED each tick until the saveSlot
// resolves -- a connect-edge BalanceSync can arrive before the client's gamemode/saveSlot
// is loaded (the WritePoints would otherwise silently fail + the mirror never lands).
// Atomic because ApplyFromHost runs off the event_feed dispatch while Tick applies on the
// game thread. Latest-wins (a newer BalanceSync overwrites a not-yet-applied pending).
std::atomic<int32_t> g_pendingTotal{0};
std::atomic<bool>    g_havePending{false};

// The earn lane's client state: the last balance the two peers agreed on, against which a local
// rise is measured. Seeded from the LOCAL value at the first tick -- never from zero, which
// would report the whole balance as an earning at every join -- and re-seeded by every applied
// host total. An un-sent rise survives a refused send (the basis only moves when the send took).
int32_t g_earnBasis = 0;
bool    g_haveEarnBasis = false;

// The host's per-sender pace and refusals: the same shape the seat and condition intents use.
constexpr coop::net::IntentBudget kEarnBudget{4.f, 1.f};
coop::net::IntentBucket g_earnBuckets[coop::net::kMaxPeers];   // 0 is the host's own, never a sender
coop::net::RefusalTally g_earnRefusals{"balance_sync"};
unsigned long long g_earnApplied = 0;
unsigned long long g_earnRefusedCount = 0;

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        // HOST: poll the canonical balance; broadcast on change (dedup).
        int32_t points = 0;
        if (!E::ReadPoints(&points)) return;  // not resolved yet (booting / at menu)
        if (g_haveSent && points == g_lastSent) return;  // unchanged -> no broadcast
        coop::net::BalancePayload p{points};
        // Keep retrying if no peer accepted the broadcast into its stream or backlog.
        if (!s->SendReliable(coop::net::ReliableKind::BalanceSync, &p, sizeof(p))) return;
        g_haveSent = true;
        g_lastSent = points;
        UE_LOGI("balance_sync: host Points -> %d, broadcast BalanceSync to all", points);
    } else {
        // CLIENT: report the local rise no intent lane forwarded (a sale, a token payout, a task
        // reward), then retry the latest pending mirror until WritePoints succeeds (the saveSlot
        // may not have resolved when the connect-edge BalanceSync arrived).
        int32_t local = 0;
        const bool worldReady = coop::net_pump::HasAnnouncedWorldReady();
        if (E::ReadPoints(&local)) {
            if (!g_haveEarnBasis) { g_earnBasis = local; g_haveEarnBasis = true; }
            if (worldReady) {
                // The rise, measured against the agreed balance. Seeding from the LOCAL value
                // (never from zero) is what keeps a join from reporting its whole balance.
                const int64_t rise = static_cast<int64_t>(local) - static_cast<int64_t>(g_earnBasis);
                if (rise > 0) {
                    const int32_t delta = rise > coop::net::kBalanceEarnMax
                                              ? coop::net::kBalanceEarnMax
                                              : static_cast<int32_t>(rise);
                    coop::net::BalanceEarnPayload p{delta};
                    if (s->SendReliable(coop::net::ReliableKind::BalanceEarn, &p, sizeof(p))) {
                        // Optimistic: the reliable channel delivers, so the host will hold what we
                        // sent -- until its own broadcast says otherwise, and that apply re-bases.
                        g_earnBasis = local;
                        static bool s_said = false;
                        if (!s_said) { s_said = true;
                            UE_LOGI("balance_sync: reported the local rise +%d (Points=%d) -- the host "
                                    "merges it and its broadcast converges every peer", delta, local); }
                    }
                    // else: the send did not take this tick; the rise is still unreported (the
                    // basis did not move) and the next tick asks again.
                } else if (rise < 0) {
                    // A fall without a host block in front of it: a spend the host already charged
                    // (the order lane debits the client before the host's broadcast lands). The
                    // host's value wins; re-basing keeps the next rise measured from the real floor.
                    g_earnBasis = local;
                }
            }
            // Not world-ready yet: no report (the host would refuse it and the basis would eat
            // the earning). The basis still seeds above, so the first report is the first rise.
        }
        if (!g_havePending.load(std::memory_order_acquire)) return;
        const int32_t total = g_pendingTotal.load(std::memory_order_acquire);
        if (E::WritePoints(total)) {
            // The applied total IS the agreed balance: the earn basis re-seeds from it, so the
            // merge we reported (or the host's own earning) never reads back as a fresh rise.
            g_earnBasis = total;
            // WritePoints is side-effect-free, so the on-screen credit number would stay frozen:
            // text_points is push-updated only by the blueprint's addPoints -> SetText. Re-run that
            // repaint with a zero add so the mirrored value actually shows.
            E::RefreshPointsHud();
            g_havePending.store(false, std::memory_order_release);
            UE_LOGI("balance_sync: mirrored host balance -> Points=%d (HUD repainted)", total);
        }
        // else: saveSlot still resolving -- silently retry next tick.
    }
}

void SendCurrentToSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    int32_t points = 0;
    if (!E::ReadPoints(&points)) {
        UE_LOGW("balance_sync: direct send to slot %d -- Points not resolved yet (no replay)", slot);
        return;
    }
    coop::net::BalancePayload p{points};
    s->SendReliableToSlot(slot, coop::net::ReliableKind::BalanceSync, &p, sizeof(p));
    UE_LOGI("balance_sync: sent current Points=%d directly to slot %d", points, slot);
}

void ApplyFromHost(int32_t total) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    // Stash the latest; Tick (game thread) writes it + RETRIES until the saveSlot
    // resolves (a connect-edge BalanceSync can arrive before the gamemode is ready).
    g_pendingTotal.store(total, std::memory_order_release);
    g_havePending.store(true, std::memory_order_release);
}

void CreditLocal(int32_t amount) {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool host      = s && s->role() == coop::net::Role::Host;
    const bool connected = s && s->connected();
    if (host || !connected) {
        // Host OR solo (no live session): apply locally. If hosting, the next TickHost
        // poll sees the new Points and broadcasts it to all clients.
        GT::Post([amount] { E::AddPoints(amount); });
        UE_LOGI("balance_sync: local credit %+d (%s)", amount,
                host ? "host -> will broadcast" : "solo");
        return;
    }
    // A connected CLIENT cannot AUTHOR the shared balance, and that half is deliberate: the lane
    // that let it -- a delta the host applied through AddPoints with no value bound -- was retired
    // whole rather than clamped, because the balance is host-authoritative everywhere. Client-side
    // earning still reaches it, but only where an intent lane forwards the action and the host
    // re-runs the game's own verb on the authoritative instance, as coingun_collect does for a coin
    // pickup. A direct credit from here is not that, so it is refused: defence in depth, since
    // coop::dev_gate already refuses the only caller on a client.
    UE_LOGW("balance_sync: local credit %+d REFUSED -- a client cannot author the shared "
            "balance; earning reaches it through an intent lane the host re-runs", amount);
}

void OnEarnIntent(int32_t delta, uint8_t slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    const auto now = std::chrono::steady_clock::now();
    if (!s->IsSlotWorldReady(slot)) {
        g_earnRefusals.Note(slot, "not_world_ready", now);
        return;
    }
    if (delta <= 0 || delta > coop::net::kBalanceEarnMax) {
        ++g_earnRefusedCount;
        g_earnRefusals.Note(slot, "a delta out of bounds", now);
        return;
    }
    if (slot >= coop::net::kMaxPeers || !g_earnBuckets[slot].Take(kEarnBudget, coop::net::NowMs())) {
        ++g_earnRefusedCount;
        g_earnRefusals.Note(slot, "paced", now);
        return;
    }
    // The game's own credit-writer, on the game thread: the UI, the stats and the emails fire
    // exactly as they do for the host's own earning. The change-poll in Tick sees the new total
    // and broadcasts it to every peer -- the earner included, whose basis re-seeds from it.
    GT::Post([delta] { E::AddPoints(delta); });
    ++g_earnApplied;
    static bool s_said = false;
    if (!s_said) { s_said = true;
        UE_LOGI("balance_sync: slot %u's earning +%d taken into the shared balance (the poll "
                "broadcasts the merge)", slot, delta); }
}

void OnDisconnect() {
    g_haveSent = false;
    g_lastSent = 0;
    g_havePending.store(false, std::memory_order_release);
    g_haveEarnBasis = false;
    g_earnBasis = 0;
    for (auto& b : g_earnBuckets) b.Reset();
    g_earnRefusals.Clear();
    if (g_earnApplied || g_earnRefusedCount)
        UE_LOGI("balance_sync: session end -- earn intents applied %llu, refused %llu",
                g_earnApplied, g_earnRefusedCount);
    g_earnApplied = 0;
    g_earnRefusedCount = 0;
}

}  // namespace coop::balance_sync
