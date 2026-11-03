// coop/props/prop_record_poll.h -- the in-place state of a save-keeping prop, refreshed by POLL.
//
// The record lane (prop_save_data) carries a prop's save record at its birth and at a join, and
// prop_record_refresh republishes it at the verb that changes it for the classes whose mutating
// verb this build knows: the drive box, the reel case, the mining frame (its upd) and the dug
// hole (its use, dig-up and buried-item verbs). Everything ELSE that keeps state in its record --
// the plant that grew from the seed, the rest of the garden family -- changes in place with no
// verb this build knows by name, so nothing republishes and the peers diverge: the plant
// vanished, the bed reset, on every peer but the one that touched it.
//
// This lane closes that class of divergence without knowing any class or verb name. Each second
// it takes a budgeted slice of the props whose class matches a configurable name pattern
// (`sync.poll_patterns`, default the plant and garden families; the mining frame and the holes
// were the poll's first tenants and have since moved to their verbs), captures each prop's save
// record, and hashes its VALUES (classes, keys, transforms and signal rows are left out on
// purpose: pose is the prop lanes', and a photo must not cost a walk). A hash that moved
// republishes the record through prop_save_data::Publish -- the same send the birth paths and the
// refresh lane use, so the host broadcasts and a client sends an intent the host validates,
// applies and re-publishes canonically. The first sight of a key is baseline only, so a join
// reports nothing.
//
// The same pass says, once per world, WHICH save-keeping keyed classes the patterns did not name
// -- the census that turns "this feature does not cover my object" into one line a user can paste
// and one pattern added to the row.
//
// Game thread throughout (the net-pump's tick; the capture dispatches the record's own getData).

#pragma once

namespace coop::net { class Session; }

namespace coop::prop_record_poll {

// The session this lane publishes through (the net-pump's install, every session start).
void SetSession(coop::net::Session* session);

// Per-tick (net_pump, game thread): the paced pass over the matched classes' instances, and the
// one-time census. No-op when the row is off or no session is connected.
void Tick();

// Session teardown: the hashes and the said-once marks go; the class list is world-scoped and
// goes with it at the next world.
void OnDisconnect();

}  // namespace coop::prop_record_poll
