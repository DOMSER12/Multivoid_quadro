// coop/interactables/elevator_sync.cpp -- see coop/interactables/elevator_sync.h. The cargo
// lift's press lane. The lift is host-authoritative (its Blueprint animates its own move, so a
// symmetric poll would fight the swing the way a door's would): a client's press is refused at
// the body and sent as an intent the host validates and runs on its own copy, the camera lane's
// shape; the host's run lands the lift where the game decides, and the lift channel's POST verb
// watch broadcasts the end the run chose. coop::interactable_sync holds the state channel.

#include "coop/interactables/elevator_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the lift lane's key for a lift, and its resolve
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"  // the sender's puppet, the press's player
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/elevator.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace coop::elevator_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace el = ue_wrap::elevator;

// The one body a client refuses: the lift's own press verb, Declared on the lift's class, so
// another class's function of the name is never asked. Every button of the lift (its
// buttonsElev family and the cargoButton are the lift's own components) dispatches this verb on
// the lift actor, so one watch covers the whole family.
UE_FACT_FN(kRowPress, "elevator_sync", L"cargoLift_C", L"actionOptionIndex", Declared);

constexpr int kTagPress = 0x454C5652;  // 'ELVR'

// The reach a sender's press is judged against. A button press is an aimed E-press, the same
// trace length the camera lane judges: a player's use reaches as far as the look-at trace,
// whose length is mainPlayer.armLength (200 by default); twice the default is the reach, and the
// authority adds the lift's own bounds radius (a lift is wide, and a player rides it) and the
// 600 uu pose-staleness allowance, so the bound refuses a sender naming a lift it cannot be
// near, never a press the game itself allows.
constexpr float kReachUU = 400.0f;

// A sender's presses run from a bounded queue at a bounded rate: a press is a click, so the
// bound is only felt by a stalled or flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;
constexpr uint64_t kClientSayMs  = 60000;  // a key's refusal line, once in this long

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

uint64_t g_sent = 0, g_ran = 0, g_converged = 0, g_denied = 0;
uint64_t g_unindexed = 0, g_loadNative = 0;
bool     g_unknownSaid = false;  // HOST: a request for a key no lift answers to, said once a session
uint64_t g_nextUnindexedSayMs = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- host side -------------------------------------------------------------------------------
// One sender: its queued presses with their bucket, and the latches that clear with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::ElevatorIntentPayload> queue;
    uint64_t nextSayMs = 0;      // the full queue's refusal, said once in kRefusalSayMs
    bool     waitSaid  = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// The lift's state for the requester alone: a request that changed nothing still answers, so a
// client whose copy diverged converges and its button is never dead. Host to that client only;
// the change case goes to everyone through the state channel's own verb watch.
void SendStateTo(coop::net::Session& s, uint8_t slot, const coop::net::WireKey& key, bool up) {
    coop::net::KeyedTogglePayload kp{};
    kp.key = key;
    kp.action = up ? 1 : 0;
    s.SendReliableToSlot(slot, coop::net::ReliableKind::ElevatorState, &kp, sizeof(kp));
}

void Deny(uint8_t slot, const std::wstring& key, const char* why) {
    ++g_denied;
    if (g_denied <= 3 || g_denied % 20 == 0)
        UE_LOGI("[LIFT-INTENT] DENY slot=%u key='%ls' -- %s (#%llu)", static_cast<unsigned>(slot),
                key.c_str(), why, static_cast<unsigned long long>(g_denied));
}

// False keeps the request at its sender's queue head: the host has no body for the sender yet
// (its first pose has not arrived), so its reach cannot be measured, and the request waits for
// the body rather than being lost. Everything else is run or refused, and consumed.
bool Execute(coop::net::Session& s, const coop::net::ElevatorIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* lift = key.empty() ? nullptr : coop::interactable_sync::ResolveElevator(key);
    if (!lift) {
        // An unknown key is counted, and said once a session: a client can name any key it likes.
        if (!g_unknownSaid) {
            g_unknownSaid = true;
            Deny(slot, key, "the lift lane indexes no lift by that key (said once a session)");
        } else {
            ++g_denied;
        }
        return true;
    }
    const coop::element::IntentTarget target = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU);
    const coop::element::IntentSubject subject = target.Authorize(lift);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[LIFT-INTENT] slot %u's lift requests wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        char why[96];
        std::snprintf(why, sizeof why, "%s (%.0f uu of %.0f)", coop::element::OutcomeName(subject.outcome),
                      subject.distUU, subject.reachUU);
        Deny(slot, key, why);
        return true;
    }
    // The convergence rule protocol.h's ElevatorIntent comment carries: the intent carries the
    // `dir` the SENDER's copy read when its press was refused -- the state its press would have
    // flipped. The host's copy at that same state lands the same end from the same press; at any
    // other state the sender's copy is behind, and the sender is answered with the host's state
    // instead of a run, so a press that races a state edge cannot drive the host's lift the
    // wrong way.
    bool hostDir = false, senderDir = false;
    if (!el::TryReadUp(lift, hostDir)) {
        Deny(slot, key, "the lift's `dir` did not read");
        return true;
    }
    senderDir = p.dir != 0;
    if (hostDir != senderDir) {
        ++g_converged;
        SendStateTo(s, slot, p.key, hostDir);
        UE_LOGI("[LIFT-INTENT] slot %u's press on key='%ls' changed nothing here: the host's end "
                "already differs from the sender's (%d vs %d); its state is sent to that slot alone",
                static_cast<unsigned>(slot), key.c_str(), hostDir ? 1 : 0, senderDir ? 1 : 0);
        return true;
    }
    // The body Authorize just measured is the sender's puppet, passed as the player of the press,
    // and the intent carries the action the sender's button dispatched, which the replay passes
    // back through the lift's own verb: the game's own body decides the move, its guards included,
    // from the same arguments it would have read where the press was made.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    const bool dispatched = el::CallActionOptionIndex(lift, body, p.action);
    if (dispatched) ++g_ran; else ++g_denied;
    UE_LOGI("[LIFT-INTENT] host ran slot %u's press on key='%ls': dispatched=%d, dir %d -> the "
            "lift's own body decides",
            static_cast<unsigned>(slot), key.c_str(), dispatched ? 1 : 0, hostDir ? 1 : 0);
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
// A key's refusal is said once in kClientSayMs, with the number that followed it; the rest are counted.
struct Said { uint64_t nextMs = 0; uint64_t since = 0; };
std::unordered_map<std::wstring, Said> g_said;

void SayRefusal(const std::wstring& key) {
    Said& sd = g_said[key];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return; }
    if (sd.since)
        UE_LOGI("elevator: refused a client's own press of key=%ls, sent as an intent "
                "(%llu more since the last line)", key.c_str(),
                static_cast<unsigned long long>(sd.since));
    else
        UE_LOGI("elevator: refused a client's own press of key=%ls, sent as an intent", key.c_str());
    sd.nextMs = now + kClientSayMs;
    sd.since = 0;
}

// Refuses a client's own press of the lift and sends the `dir` its copy reads -- the state its
// press would have flipped -- with the action its dispatch carried, to the host. A lift the lane
// does not index has no name the host could resolve: the press is refused and nothing is sent,
// said once a minute, since to the player it is a dead button on one lift.
sg::Verdict Refuse(coop::net::Session& s, void* lift, uint8_t action) {
    bool cur = false;
    if (!el::TryReadUp(lift, cur)) return sg::Verdict::Run;  // the class did not resolve: native
    const std::wstring key = coop::interactable_sync::ElevatorKey(lift);
    if (key.empty()) {
        ++g_unindexed;
        const uint64_t now = NowMs();
        if (now >= g_nextUnindexedSayMs) {
            g_nextUnindexedSayMs = now + kClientSayMs;
            UE_LOGW("elevator: refused a client's own press on lift %p, which the lift lane does "
                    "not index (%s) -- nothing sent", lift,
                    el::IsLift(lift) ? "its key is not in the index yet" : "it has no key of its own");
        }
        return sg::Verdict::Cancel;
    }
    coop::net::ElevatorIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.dir = cur ? 1 : 0;
    p.action = action;
    if (!s.SendReliable(coop::net::ReliableKind::ElevatorIntent, &p, sizeof(p))) {
        UE_LOGW("elevator: the client's press on key=%ls was not sent (the session refused it); "
                "the lift stays as the host has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    SayRefusal(key);
    return sg::Verdict::Cancel;
}

sg::Verdict OnPre(const sg::Call& call) {
    // Our own replay of the verb (the state channel's apply runs it to animate the copy) is not a
    // player's press: refusing it would cancel the apply's animation and send the host an intent
    // for the very state it just sent.
    if (call.fromOurCode) return sg::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    if (call.tag != kTagPress) return sg::Verdict::Run;
    // The pressed action rides the call's locals, the camera gate's read: the replay passes the
    // same value back through the lift's own verb on the host's copy.
    static R::ParamOffset s_action{L"action"};
    const int32_t off = s_action.Of(call.function);
    const uint8_t action = off < 0 ? 0 : *reinterpret_cast<const uint8_t*>(call.locals + off);
    // A call on an object of a world this client has not announced ready is that world's load and
    // runs natively: the save's lift takes its own state there, and the connect snapshot re-aims
    // it after the world is announced.
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) { ++g_loadNative; return sg::Verdict::Run; }
    return Refuse(*s, call.object, action);
}

void RegisterWatches() {
    if (g_reg != Reg::Pending) return;
    if (sg::WatchName(kRowPress, kTagPress, &OnPre, nullptr)) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("[LIFT-INTENT] the script-body gate refused the watch on the cargo lift's "
            "actionOptionIndex -- for the rest of this process a client's lift press runs only "
            "where it is made");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowPress, kTagPress)) {
            g_settled = true;
            UE_LOGI("[LIFT-INTENT] the cargo lift press gate is live");
        } else if (g_reg == Reg::Refused && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[LIFT-INTENT] the cargo lift press gate is dead (refused, or resolved into a "
                    "full table) -- a client's lift press runs only where it is made");
        } else if (session.running() && g_reg == Reg::Registered) {
            // A class the build spells otherwise holds the watch pending forever with no line, so
            // once in 45 s the wait is said; the lift wrapper's own census (EnsureResolved's miss)
            // names the classes the build does carry instead.
            static std::chrono::steady_clock::time_point s_next{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_next) {
                s_next = now + std::chrono::seconds(45);
                UE_LOGW("[LIFT-INTENT] the press watch is still pending on cargoLift_C::"
                        "actionOptionIndex -- the class may not be loaded yet, or the name spells "
                        "otherwise (the elevator wrapper's census names what this build carries)");
            }
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const coop::net::ElevatorIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed request ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnLiftVerb(void* /*lift*/) {
    // The state edge is interactable_sync::OnElevatorVerb's (the HostAuth channel's); the gate
    // keeps no POST watch of its own, so a client's cancelled press produces nothing here.
}

void OnElevatorIntent(coop::net::Session& session, const coop::net::ElevatorIntentPayload& payload,
                      uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto payloadCopy = payload;
    if (!sl.queue.Push(std::move(payloadCopy), kMaxPending)) {
        const uint64_t now = NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[LIFT-INTENT] slot %u queue full (%zu) -- refusing its lift requests until it "
                    "drains", static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

void OnDisconnect() {
    if (g_sent || g_ran || g_converged || g_denied || g_unindexed || g_loadNative)
        UE_LOGI("[LIFT-INTENT] session end -- sent=%llu ran=%llu changed-nothing=%llu denied=%llu "
                "unindexed refused=%llu own load run natively=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_converged),
                static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_unindexed),
                static_cast<unsigned long long>(g_loadNative));
    g_sent = g_ran = g_converged = g_denied = g_unindexed = g_loadNative = 0;
    g_unknownSaid = false;
    g_nextUnindexedSayMs = 0;
    g_said.clear();
}

}  // namespace coop::elevator_sync
