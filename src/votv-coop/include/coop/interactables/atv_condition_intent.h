// coop/interactables/atv_condition_intent.h -- the non-syncer's condition, reported. The ATV lane
// streams condition from the SYNCER only (atv_authority::AdmitBlock refuses every other peer's
// block), and for an idle rig the syncer is the host -- so a CLIENT's own refuel, repair or tire
// swap ran on its mirror, changed nothing the host could see, and the host's 2 s keepalive then
// wrote the old condition back over it. The canister drained, the state did not move: that is the
// bug this lane closes, for the SECOND ATV and for the first alike, and it is also the "act-as-host
// tire-eject intent lane" the presence rule in atv_condition_sync deferred a client's presence to.
//
// Shape: the client polls its indexed ATVs each tick; where its local condition differs from what
// the host last told it (the per-entry belief below) and it neither syncs the rig nor sits in it,
// it sends the FULL AtvStatePayload shape as AtvConditionIntent -- one fill site (atv_sync's
// ReadPayload) keeps the intent's condition block identical to the stream's. The host judges the
// sender's reach of that rig, clamps the domains, and applies the condition half through
// atv_condition_sync::ApplyPayload's intent arm: accumulators always, presence consumed BECAUSE the
// reach test passed (a validated client standing at the rig replacing a wheel is the game's own
// flow), verbs where the expressed baseline moved. The host's own stream then carries the new
// condition to every peer, the sender's belief re-seeds from that apply, and no echo loops.
//
// Game thread throughout (the net-pump's tick and receive drain).

#pragma once

#include "ue_wrap/devices/atv_condition.h"  // Snapshot (the belief's type)

#include <cstdint>

namespace coop::net { struct AtvStatePayload; }

namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_condition_intent {

// Per-tick (net_pump, game thread). CLIENT: walk the lane's indexed ATVs and report local condition
// the stream does not carry. HOST: nothing (its own copy is the authority). No-op when not connected.
void Tick();

// Wire ingest, HOST only (the dispatch gates the role and the sender slot). Applies the condition
// half of `p` to the ATV `key` names, for the sender `slot`, behind the reach test.
void OnIntent(const coop::net::AtvStatePayload& p, uint8_t slot);

// The host's (or a client's own apply of) a condition block landed on `e`: re-seed the sender-side
// belief from it, so a block the host authored does not read back as a local change to report.
// Called from atv_condition_sync::ApplyPayload for every applied block, both roles. Game thread.
void NoteBlockApplied(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p);

// Session teardown: drop the said-once marks; the beliefs live in the entries, which the ATV lane
// clears with its map.
void OnDisconnect();

}  // namespace coop::atv_condition_intent
