// coop/interactables/atv_condition_intent.cpp -- see coop/interactables/atv_condition_intent.h.
// The client half walks the ATV lane's index and reports what its mirror changed; the host half
// judges and applies. This file is the policy; ue_wrap/devices/atv_condition owns the layout and
// the dispatch, and atv_condition_sync owns the apply (the intent re-enters it).

#include "coop/interactables/atv_condition_intent.h"

#include "coop/interactables/atv_authority.h"   // AtvRequest: a pending sit/carry covers the rig
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_index_internal.h"  // detail::g_atvs: the lane's key -> record map
#include "coop/interactables/atv_sync_internal.h"
#include "coop/element/intent_authority.h"      // the reach test, the seat lanes' own judge
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"              // HasAnnouncedWorldReady: no report before our own edge

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_condition.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace coop::atv_condition_intent {

namespace {

namespace AC = ue_wrap::atv_condition;
using coop::atv_sync::AtvEntry;
using coop::atv_sync::detail::g_atvs;
using coop::net::StringFromWireKey;
using coop::net::WireKeyFromString;

// The same reach the seat and takeover requests are judged with (atv_authority's kReachUU): a
// refuel or a repair is a hands-on act at the rig, so its reporter stands within arm's length
// plus the rig's bounds allowance.
constexpr float kReachUU = 400.f;
// One report per ATV per second: a canister pour and a repair both take seconds, so the pace
// costs nothing honest and turns a forged flood into a trickle.
constexpr uint64_t kIntentPaceMs = 1000;
// A per-tick send ceiling, so a world holding many ATVs cannot spend one tick on many intents.
constexpr int kMaxSendsPerTick = 4;
// Difference quanta. Values get a small epsilon -- the clamp of a client's block is the game's
// own domain, so a legitimate local value and its clamped echo differ by nothing honest, and a
// twentieth of a point of fuel or durability is invisible. Dirt keeps the stream's own quantum
// (kDirtEps in atv_condition_sync): a 1% step does not render. Presence (mask, hasSpare) is NOT
// measured at all: the host does not consume a client's presence (the wheel-prop birth cannot
// travel), so reporting it would only spend wire on a skipped half.
constexpr float kValueEps = 0.05f;
constexpr float kDirtEps  = 0.01f;

std::atomic<unsigned long long> g_received{0}, g_applied{0}, g_refusedReach{0},
    g_refusedPace{0}, g_invalid{0}, g_sent{0};
coop::net::RefusalTally g_refusals{"atv_condition_intent"};

// The sender's pace: the seat lanes' own shape (burst, per second), per slot; slots recycle
// lowest-free, so a session's end empties them.
constexpr coop::net::IntentBudget kIntentBudget{4.f, 1.f};
coop::net::IntentBucket g_buckets[coop::players::kMaxPeers];

bool Differs(const AC::Snapshot& a, const AC::Snapshot& b) {
    if (std::memcmp(a.fixes, b.fixes, sizeof(a.fixes)) != 0) return true;
    if (std::memcmp(a.types, b.types, sizeof(a.types)) != 0) return true;
    if (a.spareFixes != b.spareFixes) return true;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a.dur[i] - b.dur[i]) > kValueEps) return true;
        if (std::fabs(a.dirt[i] - b.dirt[i]) > kDirtEps) return true;
    }
    if (std::fabs(a.bodyDirt - b.bodyDirt) > kDirtEps) return true;
    if (std::fabs(a.spareDur - b.spareDur) > kValueEps) return true;
    if (std::fabs(a.spareDirt - b.spareDirt) > kDirtEps) return true;
    if (std::fabs(a.fuel - b.fuel) > kValueEps) return true;
    if (std::fabs(a.health - b.health) > kValueEps) return true;
    return false;
}

// The local snapshot -> the wire block, the fill that atv_sync's ReadPayload does for the stream.
// The pose and stamp halves stay zero: the host reads none of them, and zeros are finite.
void FillIntentBlock(const AC::Snapshot& s, coop::net::AtvStatePayload& p) {
    for (int i = 0; i < 4; ++i) {
        p.tiresDurability[i] = s.dur[i];
        p.tiresDirt[i]  = s.dirt[i];
        p.tiresFixes[i] = s.fixes[i];
        p.tiresTypes[i] = s.types[i];
    }
    p.bodyDirt = s.bodyDirt;
    p.spareDurability = s.spareDur;
    p.spareDirt = s.spareDirt;
    p.fuel = s.fuel;
    p.health = s.health;
    p.tiresMask = s.mask;
    p.hasSpare = s.hasSpare ? 1 : 0;
    p.spareFixes = s.spareFixes;
    p.tiresValid = 1;
}

}  // namespace

void Tick() {
    auto* s = coop::atv_sync::LaneSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;  // a report before our own edge can ride the seed back
    if (!coop::atv_sync::IndexCurrent()) return;            // a stale index holds another world's ATVs
    if (!AC::Resolved() && !AC::Resolve()) return;

    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = coop::atv_sync::NowMs();
    int sends = 0;

    for (auto& kv : g_atvs) {
        if (sends >= kMaxSendsPerTick) break;
        AtvEntry& e = kv.second;
        if (!ue_wrap::reflection::IsLiveByIndex(e.actor, e.idx)) continue;
        // The syncer's condition crosses by the stream, a pending sit or carry will too once
        // granted, and the seat's holder is the syncer by the grant; only a bystander reports.
        const bool syncsOrPending = e.syncerSlot == localSlot ||
                                    (e.pendingSeq != 0 &&
                                     e.pendingAction != coop::atv_authority::AtvRequest::Leave);
        if (syncsOrPending || e.occupantSlot == localSlot) continue;

        AC::Snapshot local{};
        if (!AC::Read(e.actor, local)) continue;   // an unreadable rig has no honest report

        // First sight (or the entry repointed to a new actor): the LOCAL state is the baseline,
        // so a join reports nothing and the world's own state does not ride back to the host.
        if (!e.condIntentBeliefSeeded || e.condIntentBeliefActor != e.actor) {
            e.condIntentBelief = local;
            e.condIntentBeliefSeeded = true;
            e.condIntentBeliefActor = e.actor;
            continue;
        }
        if (!Differs(local, e.condIntentBelief)) continue;
        if (nowMs < e.condIntentNextMs) continue;

        coop::net::AtvStatePayload p{};
        WireKeyFromString(kv.first, p.key);
        FillIntentBlock(local, p);
        if (!s->SendReliable(coop::net::ReliableKind::AtvConditionIntent, &p, sizeof(p))) continue;
        ++sends;
        ++g_sent;
        e.condIntentNextMs = nowMs + kIntentPaceMs;
        // Optimistic: the reliable channel delivers, so what we sent is what the host will hold
        // -- until its own block says otherwise, and that apply re-seeds the belief (below).
        e.condIntentBelief = local;
        static bool s_said = false;
        if (!s_said) { s_said = true;
            UE_LOGI("atv_condition_intent: reported a rig's local condition the stream does not "
                    "carry (key='%ls', fuel=%.1f health=%.1f) -- the host judges and merges it",
                    kv.first.c_str(), local.fuel, local.health); }
    }
}

void OnIntent(const coop::net::AtvStatePayload& p, uint8_t slot) {
    ++g_received;
    auto* s = coop::atv_sync::LaneSession();
    if (!s) return;
    if (!s->IsSlotWorldReady(slot)) {
        g_refusals.Note(slot, "not_world_ready", std::chrono::steady_clock::now());
        return;
    }
    const std::wstring key = StringFromWireKey(p.key);
    if (key.empty()) {
        g_refusals.Note(slot, "an empty key", std::chrono::steady_clock::now());
        return;
    }
    if (!p.tiresValid) { ++g_invalid; return; }   // the producer could not read; nothing to merge
    if (!coop::atv_sync::IndexCurrent()) return;  // a stale index holds another world's ATVs
    auto it = g_atvs.find(key);
    if (it == g_atvs.end() ||
        !ue_wrap::reflection::IsLiveByIndex(it->second.actor, it->second.idx)) {
        g_refusals.Note(slot, "unknown_atv", std::chrono::steady_clock::now());
        return;
    }
    AtvEntry& e = it->second;
    // The condition block is thirteen floats; one non-finite poisons the host's persisted fields
    // and disables this lane's own edges with it. Refuse the block as the stream's apply does.
    const float* fs[] = { &p.tiresDurability[0], &p.tiresDurability[1], &p.tiresDurability[2],
                          &p.tiresDurability[3], &p.tiresDirt[0], &p.tiresDirt[1], &p.tiresDirt[2],
                          &p.tiresDirt[3], &p.bodyDirt, &p.spareDurability, &p.spareDirt,
                          &p.fuel, &p.health };
    for (const float* f : fs) {
        if (!std::isfinite(*f)) {
            ++g_invalid;
            g_refusals.Note(slot, "a non-finite condition", std::chrono::steady_clock::now());
            return;
        }
    }
    if (slot >= coop::players::kMaxPeers || !g_buckets[slot].Take(kIntentBudget, coop::atv_sync::NowMs())) {
        ++g_refusedPace;
        g_refusals.Note(slot, "paced", std::chrono::steady_clock::now());
        return;
    }
    // The reach test, the seat lanes' own judge: the reporter stands at the rig it reports on.
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(*s, slot, kReachUU).Authorize(e.actor);
    if (!subject) {
        ++g_refusedReach;
        g_refusals.Note(slot, coop::element::OutcomeName(subject.outcome),
                        std::chrono::steady_clock::now());
        return;
    }
    // The apply: clamped, presence skipped, verbs on the change edges, belief re-seeded inside,
    // and `stampDue` up -- the merged state reaches every ready client within the tick.
    coop::atv_condition_sync::ApplyPayload(e, p, /*fromHost*/ false, /*fromIntent*/ true);
    ++g_applied;
    static bool s_said = false;
    if (!s_said) { s_said = true;
        UE_LOGI("atv_condition_intent: merged slot %u's condition into the host's rig key='%ls' "
                "(fuel=%.1f health=%.1f) -- the stamp carries it to every peer", slot, key.c_str(),
                p.fuel, p.health); }
}

void NoteBlockApplied(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p) {
    if (!p.tiresValid) return;   // an invalid block wrote nothing; the belief is not its business
    coop::atv_condition_sync::PayloadToSnapshot(p, e.condIntentBelief);
    e.condIntentBeliefSeeded = true;
    e.condIntentBeliefActor  = e.actor;
}

void OnDisconnect() {
    if (g_sent || g_received || g_refusedReach || g_refusedPace || g_invalid)
        UE_LOGI("atv_condition_intent: session end -- sent %llu, received %llu, applied %llu; "
                "refused: %llu out of reach, %llu paced, %llu invalid",
                g_sent.load(), g_received.load(), g_applied.load(), g_refusedReach.load(),
                g_refusedPace.load(), g_invalid.load());
    g_received = g_applied = g_refusedReach = g_refusedPace = g_invalid = g_sent = 0;
    g_refusals.Clear();
    for (auto& b : g_buckets) b.Reset();
}

}  // namespace coop::atv_condition_intent
