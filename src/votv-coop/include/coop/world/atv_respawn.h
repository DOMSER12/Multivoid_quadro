// coop/world/atv_respawn.h -- the host's ATV re-mint: destroy a rig and spawn a fresh one under
// the SAME game Key, beside the host's own player, with its condition forced to the full state a
// new ATV carries (a full tank, all four wheels and the spare present, no wear, no dirt, health
// 100). The chat command `/atvrespawn [key]` reaches it; the host (or a permitted operator) runs
// it, because the SPAWN is the host's to author -- the ATV lane's announce then carries the new
// rig to every client, and each peer's fresh mirror under the same Key is replaced the same way.
//
// Why beside the PLAYER and not at the old spot: the command exists for the rig a cliff, a lake
// or a rollback ate -- its old transform is exactly the place the rig cannot be recovered from.
// The NavMesh projection and the open-ground fallback are the start-pair spawn's own two-step.
//
// Game thread throughout (the command dispatch runs there; the spawn helpers demand it).

#pragma once

#include <string>
#include <vector>

namespace coop::commands { class Registry; }

namespace coop::atv_respawn {

// Register `/atvrespawn` into the production command registry. False when the registry refuses
// it (said once by the caller). The command runs only on a host's or a solo dispatch.
bool RegisterCommand(coop::commands::Registry& reg);

// The command's body: destroy the named ATV (or the one ATV of the world, or list the keys when
// the word names none and several stand) and re-mint it full. The reply lines go back through the
// command context. Game thread.
std::vector<std::string> Respawn(const std::string& keyWord);

}  // namespace coop::atv_respawn
