// coop/world/atv_start_pair.h -- the host's second ATV at the start of a game.
//
// A fresh VOTV save puts one ATV at the base, and a co-op pair then shares it or one player
// walks. This module tops the world up to two: when the host's gameplay world has settled and
// the object index can vouch for every ATV it holds, a world that holds exactly one ATV_C gets
// a second one spawned beside it, through the ATV lane's own materialization
// (ue_wrap::atv::SpawnMirror -- a preset key, physics on, a native idle rig). The spawn is the
// whole feature: the hub pass files the new rig under its key and the lane's announce machinery
// (AtvSpawn to the world-ready clients, the connect snapshot to a joiner) tells every peer the
// way it tells them of any runtime ATV, seat and syncer and condition included. A world that
// arrives already holding two or more (a save the pair kept, or a rig the spawn menu added) is
// left as it stands, and a world whose one ATV is gone is not conjured a replacement.
//
// Host only: a client's Tick is a no-op (its world is the host's, mirrored), and pure
// single-player never reaches the tick, which rides the session's gameplay chain. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::atv_start_pair {

// Every gameplay tick, from TickGameplay: the settle and readiness gates, then a two-second
// poll that walks the world's ATVs once and, in a one-ATV world, spends one spawn attempt.
// Fifteen failed attempts end the world's window, said once. A no-op on a client and during a
// flee window. Game thread.
void Tick(coop::net::Session& session, bool fleeing);

// The session ended: the attempt budget and the said-once marks are owed to the next session
// in this world. The spawned latch stays -- the world, and its second ATV, did not change.
void OnSessionEnd();

}  // namespace coop::atv_start_pair
