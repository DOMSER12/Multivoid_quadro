// ue_wrap/devices/elevator.h -- engine access for the base CARGO LIFT (AcargoLift_C), the
// principle-7 wrapper layer: no network or coop state. coop::interactable_sync drives the sync
// through here, and coop::elevator_sync the client's press gate.
//
// FACTS (measured on the 0.9.0n pak's blueprint, Content/objects/cargoLift.uasset):
//   * AcargoLift_C descends from Aactor_save_C, so it carries the save `Key` the appliance
//     family keys its lanes by, and the lift is save-persistent like they are.
//   * Its state is the `dir` variable the press writes (the target end: one value per station),
//     guarded by `moving` while the `move` timeline runs. The timeline is a BP timeline node,
//     so the instance holds a `move` UTimelineComponent* property, and its body reads `dir` --
//     the same shape the garage's `Open` + `mov` pair has, with acivae() there and the
//     timeline start here.
//   * The only player-facing writer is `actionOptionIndex` (the int_objects interaction verb):
//     the wall buttons (buttonsElev, buttonsElev1, buttonsElev2, cargoButton) are COMPONENTS of
//     the lift actor, so every button's press dispatches this one verb on this one actor, and a
//     single watch covers the whole family.
//   * `prop_cargolift_C` (Content/objects/prop_cargolift.uasset) is the cage/platform prop the
//     level carries; the lift controller drives it, and it has no verb of its own -- it is not
//     part of this lane.
//
// INFERRED (said so, and probed by the lane's shadow probe at runtime): the press body is a
// toggle -- `dir := !dir` guarded on `moving` -- and the timeline start takes its direction from
// `dir`. The apply therefore writes `dir` to the wanted end and plays the timeline from its
// start, the garage's ApplyOpen rule; the deferred-apply retry of the channel re-aims a
// mid-move lift the same way a mid-swing opposite packet re-aims a garage door.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::elevator {

// Resolve AcargoLift_C + the `Key`, `dir`, `moving` offsets + the press verb and the timeline
// property. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is cargoLift_C or a subclass. False if not yet resolved.
bool IsLift(void* obj);

// The lift's Aactor_save_C::Key as a wide string ("" on failure, L"None" if unkeyed). This is
// the cross-peer identity, the appliance family's rule: the lift is save-persistent, so the
// game's own key is stable across peers.
std::wstring GetKeyString(void* lift);

// Read the lift's target end into `up` (the `dir` value, normalised so exactly one bit
// travels). False if the read could not be made (null / not resolved / the field's type is
// neither bool nor byte nor int); leaves `up` untouched on failure.
bool TryReadUp(void* lift, bool& up);

// Drive the lift to `up` -- idempotent (a no-op if `dir` already says so). THE PREFERRED APPLY is
// a replay of the lift's own press verb -- the body does everything the game's own press does --
// run only when this copy sits at the OPPOSITE end (a toggle from there lands at `up`; the
// press's `moving` guard can swallow a mid-move replay, which the verify reads and the channel's
// deferred retry re-aims). The fallback writes `dir` directly (a mirror that let the field go
// stale would oscillate) and plays the `move` timeline through PlayFromStart. Must run on the
// game thread. False on null or a failed apply.
bool ApplyUp(void* lift, bool up);

// The same, with the pressing body the verb's `player` parameter reads (the local pawn on the
// applying peer; null where the caller has none and the class declares the parameter away).
bool ApplyUpAs(void* lift, bool up, void* player);

// Whether the lift is mid-move, its `moving` (optional field: false when it did not resolve,
// meaning "not moving" for the lane's purposes). Game thread.
bool TryReadMoving(void* lift, bool& moving);

// The lift's own press verb, actionOptionIndex(player, action), as the buttons dispatch it:
// for the host's run of a client's intent (coop::elevator_sync). `player` may be null (the
// verb ignores it where the class declares none). False on null, an unresolved UFunction or a
// failed dispatch. Game thread.
bool CallActionOptionIndex(void* lift, void* player, uint8_t action);

}  // namespace ue_wrap::elevator
