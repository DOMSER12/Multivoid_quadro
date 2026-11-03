// coop/props/hole_sync.h -- the dug hole's birth lane (the shovel's dirthole family).
//
// The hole's in-place changes (burying an item, planting a seed, digging it up) ride the record
// refresh lane's verb watches (coop/props/prop_record_refresh). What that lane cannot see is the
// BIRTH: a dig spawns a new dirthole actor, no hole verb runs, and the keyed-prop rule ("the host
// authors every keyed prop", prop_spawn_authoring's who-asked doctrine) leaves the client's world
// mints local. This lane closes exactly that: a dirthole birth on a client is reported to the host
// as a HoleBirthIntent (protocol.h), the host spawns the authoritative copy at the class, key and
// pose the intent names, and its express tells the other clients; the author adopts its own local
// copy by the exact key when the host's PropSpawn row comes back. A hole birth on the HOST is
// expressed directly. The record behind either birth is prop_save_data's, the same record the verb
// watches republish, so the hole's state (its ground type, a buried item, a plant) lands on the
// peers the same way every other save-keeping prop's does.
//
// The terrain cut itself is the hole actor's own to render: the holes a joiner's save load births
// render correctly on another peer's machine (every peer loads the host's save), so a mid-session
// spawn of the same class at the same pose renders the same way.
//
// Game thread only (a script-gate watch and the spawn it authors). Installed per session.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropDropIntentPayload;
}  // namespace coop::net

namespace coop::hole_sync {

// The script-gate birth watch on dirthole_C::ReceiveBeginPlay (Declared), and the pending-birth
// drain's pump. Idempotent; safe to call every subsystem-install tick. Game thread.
void Install(coop::net::Session* session);

// Per-net-pump-tick: drain the pending births (a birth waits for its Key to become readable, a
// tick or two after BeginPlay), and the watch's settle. Game thread.
void Tick(coop::net::Session& session);

// HOST handler for a received HoleBirthIntent: spawn the authoritative hole at the class, key and
// pose the intent names, dup-guarded by the key index, the author's record awaited behind it, and
// the birth expressed to the other clients on the drain. Game thread.
void OnHoleBirthIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                       uint8_t senderSlot);

// Session teardown: counters and the pending list.
void OnDisconnect();

}  // namespace coop::hole_sync
