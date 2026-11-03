// coop/interactables/elevator_sync.h -- the base cargo lift's press lane: a client's press of any
// of the lift's buttons is refused at its own body and run by the host, which answers with the
// state its run left. The state half of the lane (the ElevatorState channel, its adapter, the
// connect snapshot) lives in interactable_sync with the other keyed devices; this module owns the
// client's PRE gate and the host's intent executor, the shape camera_toggle_intent owns for the
// security cameras. The model: docs/devices.md, the lift section.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ElevatorIntentPayload;
}  // namespace coop::net

namespace coop::elevator_sync {

// Arm the PRE gate on the lift's press verb and store the session. Idempotent per process;
// retried while the game's class is not loaded yet. Game thread.
void Install(coop::net::Session* session);

// Settle the gate's registration; HOST: serve one queued lift intent a tick a client.
void Tick(coop::net::Session& session);

// HOST: one of the lift's state verbs ran on `lift` -- nothing here, the edge goes through
// interactable_sync::OnElevatorVerb; kept as the gate's counterpart so the lane reads whole.
// Game thread.
void OnLiftVerb(void* lift);

// HOST: a client's press intent arrived. Queued per sender and served at a bounded rate; the
// run and the reach live in the module. Game thread (the reliable drain).
void OnElevatorIntent(coop::net::Session& session, const coop::net::ElevatorIntentPayload& payload,
                      uint8_t senderSlot);

// The receive-side counters, said once at the session's end.
void OnDisconnect();

}  // namespace coop::elevator_sync
