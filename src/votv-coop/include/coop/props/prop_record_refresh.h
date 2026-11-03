// coop/props/prop_record_refresh.h -- a prop whose save record changes while it sits in place
// republishes that record, at the verb that changed it. Four families today: the drive box
// (Aprop_box_C, the "data case"), the tape reel case (Aprop_reelbox_C), the mining frame
// (Aprop_miner_C, the GPU/cooler farm) and the dug hole family (Adirthole_C, the shovel's holes
// with their buried-item and rake variants). Overview: docs/props.md.
//
// prop_save_data carries a prop's record on every BIRTH path -- a spawn, a drop intent, the join
// snapshot -- and on none other, so a prop whose save state changes while it sits there never
// tells anyone. The box keeps a lid and its contents in the record (`opened`, `drives_in`), the
// reel case its `lid`, `reeltop` and `reelBottom` (a reel's progress, -1 = empty), the mining
// frame its seated hardware, its upgrades and its accumulated points, and a hole what was buried
// in it or planted on it. So a guest's open left every other peer with a closed case, a reel a
// guest put in vanished for the host, a GPU a guest seated fell out of the frame, and a hole a
// guest dug stayed shut for everyone else: their destroys crossed, the records did not.
//
// The verb watched is each class's own upd() for the box, the reel case and the mining frame:
// every mutation ends there and no tick or look-at path calls it (the mining frame's own point
// accumulation ends there too, so its record crosses at the game's own cadence). The hole family
// has no upd() of its own, so its four mutating verbs are watched Declared on the hole's class --
// the use and its latent tail, the dig-up and the buried-item process; an inherited dispatch of a
// variant reaches the same UFunction the watch sits on. Every watch is a script_gate one, not a
// ProcessEvent observer -- the prop calls its own verbs from inside its Blueprint, a route
// ProcessEvent never sees. The .cpp carries the rest.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::prop_record_refresh {

// Watch each class's refresh verb and cache the session. Idempotent; a class not loaded yet is
// retried on the next call. Game thread.
void Install(coop::net::Session* session);

// Session teardown: release the session and reset publication counters.
void OnDisconnect();

}  // namespace coop::props::prop_record_refresh
