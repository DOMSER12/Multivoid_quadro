# Vehicles

## Purpose

The ATV, the game's one vehicle: a five-body physics rig with a seat that takes the player's
pawn, a parts economy of modules, tyres and a spare, and accumulators for fuel, dirt and health.
How the peer driving it and the peers watching it share one rig, why a watching peer runs the
rig natively instead of freezing it, what condition travels, and what is still open.

## How it works

### The rig, and why a mirror is not frozen

The ATV is not an engine vehicle: its root body simulates physics and its four wheels are
separate simulating bodies held by constraints, so the vehicle's visible output is suspension
travel, and one actor transform is not the pose. The first lane froze a mirror and teleported its
root twenty times a second, dragging four constrained bodies behind it: measured suspension
travel of thirty centimetres against a native two to four, and a release that launched the other
peer's copy at a metre and a half per second. That lane was replaced whole
(`coop/interactables/atv_sync`).

A peer that does not sync an ATV runs the rig natively, physics on and tick on, and is
steered toward the syncer on every packet (`coop/interactables/atv_corrector`): the
velocity is written, the position error is closed by a bounded corrective velocity, and past a
speed-scaled threshold, or when the error stops shrinking, the mirror is cut to the syncer's
pose through the game's own teleport. This is MTA's vehicle shape, and the one thing a mirror may
not do is author collision damage: the seven hit delegates are intercepted on a peer that does
not own the rig's tick and dispatched with a zeroed impulse, so the notification runs whole and
only the damage dies (`coop/interactables/atv_hit_guard`). Cancelling the wheel delegates
outright cost a mirror its rig shape, a sag of twenty-five to forty centimetres, until the cause was
found.

### The seat and the syncer are the host's

Who sits and who writes the rig are different questions, and the host decides both
(`coop/interactables/atv_authority`); no machine decides either for itself. The seat is a claim: a
client's local sit sends `AtvSeat`, and the host judges it (the sender's world ready, the ATV known,
the sender alive, a pace, one seat at a time, the seat free, the sender within reach) and answers
every request with `AtvAnswer`. The host's own player is judged by the same table at its own tick,
without the reach test. The syncer is the peer whose copy writes the rig and runs the brain, the
accumulators, wheel torque and collision damage. It is the occupant while seated; unseated, it stays
with the peer that drove or carried the ATV until that peer is out of range (130 m) or silent for
five seconds, or the ATV has been at rest and uncarried for a second, and then the host takes it
back. A grab sends `AtvTakeover`, judged the same way and refused while the ATV is seated or another
peer carries it. A peer merely carrying an ATV syncs it but is not its occupant, so it never denies
the seat. A client acts at once on its local sit or grab and yields to the answer: a refused sit
dismounts through the game's own dismount, a refused grab drops. Every machine follows the host's
stamp, the seat and the syncer on each `AtvState` the host sends. `[?]` Built to this design; no
two-peer run has judged it yet.

### The stream

While synced by a peer, the rig's pose, full rotation, linear and angular velocity stream about
twenty times a second, keyed by the ATV's game key. A client's stream reaches the host alone: the
host applies only its syncer's blocks, paced, and re-sends each to the other clients with its stamp.
An idle ATV is
synced by the host at five hertz gated on change with a two-second keepalive floor, so a parked
vehicle costs one packet every two seconds. An ATV that appears at runtime, from the spawn menu,
has no save twin, so the host announces it under its game key.
The client parks a valid announcement until gameplay, the discovery index and load quiescence
agree, stamped with the world it arrived in: a world change drops it, the new world's own replay
bringing it again, and a spawn that fails in a ready world is final, never retried.
`[?]` This readiness path still needs a two-peer join and travel test.

### The second ATV at the start

A fresh save puts one ATV at the base, and a co-op pair then shares it. The host tops its world
up to two once the world has settled (`coop/world/atv_start_pair`): a settled gameplay world that
holds exactly one keyed `ATV_C` gets a second rig spawned beside it, through the lane's own
materialization (`ue_wrap::atv::SpawnMirror`, physics on) under a fixed namespaced key
(`MVOID_ATV_2`). The ground the rig rests on is NavMesh-first: a projection hit places it on the
point the mesh certifies, nearest a fixed offset from the first. VOTV bakes its NavMesh for the
AI walkers, though, and the base's parking lot is not on it -- so a projection miss is read as
open ground, not as a failure, and the rig spawns at the offset anyway, at the first rig's height
plus a lift for physics to settle. A first rig parked somewhere meshed (the garage) keeps the
projection's wall-safe placement; the fallback only ever fires where the mesh has nothing to say.
The spawn is the whole feature -- the
hub pass files the new rig under its key, and the announce machinery above tells every peer the
way it tells them of any runtime ATV. A world already holding two or more is left as it stands,
and a world whose ATV is gone is not conjured a replacement; fifteen failed attempts close the
world's window, said once. Host only;
a client's world is the host's, mirrored. `[?]` Built to this design; a field run hit the
parking-lot NavMesh miss (the window closed unfilled) and the fallback was added for it; no
two-peer run has walked a fresh save to the pair yet.

### Condition

The syncer's condition rides the same payload: per-wheel durability, dirt, fixes and type, the
spare's trio, body dirt, fuel and health (`coop/interactables/atv_condition_sync`). A mirror is
overwritten and re-derives its visuals through the game's own reducers on change edges, against a
baseline seeded from the actor rather than from the last packet: a zero baseline would break all
eight constraints on a settled rig, and a per-packet one would starve the dirt reducer forever.
Accumulators apply from the syncer, clamped to the game's domains when it is a client; presence,
which tyres are on and whether the spare is, is consumed only from the host's own copy, because a
client syncer's eject ships a mask whose paired wheel prop cannot travel, and applying it would
turn a divergence into an item loss the host persists. The mirror's own accrual is held at zero by the impulse neuter, so an
overwrite never races an irreversible act. The battery is an inserted prop's charge and rides
the prop lane.

### The non-syncer's condition, reported

Everything above carries the SYNCER's condition. The mirror of a peer that does not sync the rig
still runs the game natively, so a refuel with the canister, a repair with the kit or a tyre's
fix countdown all landed on that peer's copy alone -- and the host's two-second keepalive wrote
the old condition back over it. The canister drained and the gauge never moved; the second ATV
made it visible, and it was true of the first whenever the refueller was not the syncer
(`coop/interactables/atv_condition_intent`).

That peer now reports: each tick, every indexed rig whose condition differs from what the host
last told it (the entry's belief, seeded from the actor at first sight and re-seeded by every
applied block) and whose rig this peer neither syncs nor sits in is sent as `AtvConditionIntent`
-- the full `AtvStatePayload` shape, of which the host reads only the condition block, once a
second per rig. The host judges the sender's reach of that rig (the seat lanes' own judge), paces
it per sender, clamps the block to the game's domains, and applies it through the condition lane's
intent arm: values always, verbs on the change edges, presence skipped exactly as a client
syncer's is (the wheel-prop birth cannot travel), and one stamped block to every ready client at
once -- the merged state, so the other peers converge within the tick and the earner's belief
re-seeds from what comes back. Not synced still: the mask and the spare's presence itself, whose
props this lane cannot author.

### The re-mint: /atvrespawn

A rig a cliff, a lake or a rollback ate is recovered by `/atvrespawn [key]`
(`coop/world/atv_respawn`), a host command (a permitted operator's on a client, relayed the way
commands are): it destroys the named rig -- or the world's one rig, or lists the keys when
several stand -- and spawns a fresh one under the SAME game key beside the host's own player,
through the start-pair's materialization and its NavMesh-then-open-ground placement. The fresh
rig's condition is forced to what a new ATV carries -- full tank, all four wheels and the spare
present, no wear, no dirt, health 100 -- written and then expressed through the game's own
reducers. The key's reuse is the delivery: the hub pass prunes the dead row, the announce names
the key again, and every peer's mirror is replaced the same way.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the pose while driven or carried | the syncer | a twenty-hertz stream; a client's reaches the host, which re-sends it stamped |
| the pose while idle | the host | five hertz on change, a keepalive floor |
| the seat | the host | a judged request, answered, stamped to every client |
| the syncer | the host | assigned on a sit or a grab, taken back at rest, out of range or on silence |
| collision damage, the accumulators, wheel torque | the tick owner, the syncer's machine | a mirror's impulses are zeroed |
| every route to an ATV's damage and death (the two blast verbs, the explosion) | the tick owner | a copy that does not own the tick cancels them at the script gate; the owner takes every blast that reaches it |
| the story event's trap | the host | the host copy's `trap` rides the stamp (bit 4); the driven copy fires it |
| the condition values | the syncer | overwritten on the mirror, re-derived on edges |
| a non-syncer's own condition change | the reporting peer, the host judged | an `AtvConditionIntent` once a second per rig; the host clamps, applies and stamps the merge to every client |
| tyre and spare presence | the host | consumed from the host's own copy only |
| a runtime ATV's identity | the host | its game key, announced |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AtvState` | the syncer (a client's to the host alone), re-sent by the host; the host when idle | key, pose, rotation, velocities, the seat and the syncer the host stamps, the host copy's trap, the condition block, an adopt flag |
| `AtvConditionIntent` | a client to the host | the condition block of a rig the sender does not sync, in the `AtvState` shape; the host reads the condition only, judges the reach and merges it |
| `AtvSeat`, `AtvTakeover` | a client to the host | a sit in or leave of the driver's seat; a grab of the ATV |
| `AtvAnswer` | the host to the asking client | the verdict on one request, with the seat and the syncer the host holds |
| `AtvExplodeRequest` | a client to the host | the ATV the client syncs that exploded, by key, and whether the story event's trap fell in that frame; the host blasts at the ATV's own `tp` |
| `AtvSpawn`, `AtvDestroy` | the host to all | an ATV's key, class and pose; its teardown |
| `AtvSetComplete` | the host to a joiner | the count of distinct keys its connect snapshot named |

## Late join

The joiner loads the host's save first, so it already holds the ATVs the save placed. Once its
save has finished loading, the host walks every ATV its world holds and announces each by key, then
closes the set with `AtvSetComplete`. The joiner adopts the copy it holds under that key and spawns
a mirror only when it holds none, so it ends with exactly one actor per host ATV. It then removes
every ATV the set did not name, such as one the host lost after the save was taken. Arrivals that
come before the joiner's world is ready wait in a park. A set the host cannot vouch for (an ATV
still loading, a send that failed) is withheld, which removes nothing; the host owes it and
retries. Each ATV's pose, velocity, condition and the host's stamp (the seat and the syncer) follow,
so an ATV airborne at the join arrives moving and lands, and the joiner's mirror is cut to the pose
on its first packet, even for an ATV a client syncs; after that it follows the syncer's blocks. A
joiner sends no request before its own world is ready: a sit or a grab it makes before then is held,
not yielded, and goes out at that point. A rejoin into the same slot is a departure and an arrival,
and the leaver's seat and syncerships were cleared at its departure. `[V]` the ATV spawn drill's
run, join, park and divergence arms, each red arm failing as designed (`coop/dev/atv_spawn_drill`).

## Known limits

| Limit | Evidence |
|---|---|
| While driving, the mirror trails the syncer: the correction is a bounded velocity bias, and the cut to the syncer's pose happens only past a speed-scaled error or when the error stops shrinking | `[V]` `coop/interactables/atv_corrector` |
| A client drives for one round trip before the host's answer can refuse its sit or grab; a refused sit dismounts, a refused grab drops, and the blocks it streamed meanwhile are dropped and counted, never applied | `[?]` `coop/interactables/atv_authority`, built to the design and not yet run |
| At a hand-back to the host the old syncer's last blocks may still be in flight; the host drops and counts them, and the stamp moves every client to the host's copy | `[?]` `coop/interactables/atv_authority`, `coop/interactables/atv_sync` |
| A client's tyre eject is refused by the arbiter, so the host keeps the wheel and the peers disagree until the eject intent lane exists | `[V]` `coop/interactables/atv_condition_sync`, by design |
| The pose stream rides the reliable lane; a sequenced unreliable stream is the intended shape | `[V]` `coop/interactables/atv_sync` sends it reliable |
| Not synced: the thirteen modules and every upgrade effect, lights, brake and turbo, honk, the radio, the map, the kerfur passenger, and a remote driver's body is not seated on the ATV; REPAIR as an act is not mirrored, but its OUTCOME now is -- the fixed values travel by `AtvConditionIntent` from whichever peer did the work | `[V]` nothing under `coop/interactables/atv_*` carries the acts; `coop/interactables/atv_condition_intent` carries the values |

## Code map

| Concept | Files |
|---|---|
| the lane | `coop/interactables/atv_sync`, `coop/interactables/atv_sync_internal.h`; `coop/interactables/atv_body` (this machine's own seat and carry against the table) |
| the seat and syncer table, and the host's judgements | `coop/interactables/atv_authority` |
| the corrector, the hit guard and the damage gate | `coop/interactables/atv_corrector`, `coop/interactables/atv_hit_guard`, `coop/interactables/atv_damage_gate` |
| condition | `coop/interactables/atv_condition_sync`, `ue_wrap/devices/atv_condition`; `coop/interactables/atv_condition_intent` (the non-syncer's report) |
| the re-mint | `coop/world/atv_respawn` (the `/atvrespawn` command) |
| the engine wrapper | `ue_wrap/devices/atv` |
| the seat | `coop/interactables/device_occupancy` |
| tests and probes | `coop/dev/atv_probe` (the drive arm), `coop/dev/atv_tire_probe`, `coop/dev/atv_eject_drill`; the smoke run with the ATV scenario enabled |
