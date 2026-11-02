// coop/world/atv_respawn.cpp -- see coop/world/atv_respawn.h. The destroy half is the ATV lane's
// own DestroyMirror (a seated driver is dismounted first); the spawn half is SpawnMirror, the
// deferred-actor path the start pair and the client mirrors use, which writes the Key into the
// deferred actor so the construction script keeps it instead of minting a random one. The hub's
// next pass files the new actor under its Key, prunes the row the dead actor held, and the lane's
// announce (a key lost, then a key gained) replaces every peer's mirror with the fresh rig.

#include "coop/world/atv_respawn.h"

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_line.h"      // EqualsAsciiNoCase: the key word's one compare
#include "coop/commands/command_registry.h"
#include "coop/interactables/atv_sync.h"     // AnnounceableAtvKey: the wire's own key test
#include "coop/player/players_registry.h"    // Registry::Local: the spawn lands beside this player
#include "coop/text/utf8_codec.h"             // ToUtf8: the reply lines
#include "ue_wrap/core/fname_utils.h"        // Resolved: the key must convert to a name
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_condition.h"
#include "ue_wrap/engine/engine.h"       // TryGetActorLocation/Rotation, ProjectToNav
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <utility>

namespace coop::atv_respawn {

namespace {

namespace A  = ue_wrap::atv;
namespace AC = ue_wrap::atv_condition;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;
using ue_wrap::FVector;
using ue_wrap::FRotator;

// The placement, the start pair's own shape: this far from the player, on the world axes, with
// the NavMesh projection finding the ground and the open-ground fallback (the base's parking lot
// is NOT on the baked NavMesh) spawning at the player's own height plus a lift, where physics
// drops the five-body rig onto the concrete.
constexpr float kOffsetXcm = 250.f;
constexpr float kOffsetYcm = -250.f;
constexpr float kProjectCm = 300.f;
constexpr float kLiftCm    = 50.f;

// One candidate: the game's own quad, keyed, in the world this machine is in now. The same rule
// the start pair counts by -- the witch car is ATV lineage of its own and is never re-minted.
struct Found {
    std::wstring key;
    void*        actor = nullptr;
    std::wstring className;
};

std::vector<Found> WalkAtvs() {
    std::vector<Found> out;
    struct Ctx { std::vector<Found>* out; } ctx{&out};
    A::ForEachAtv([](void* p, void* actor, int32_t /*idx*/) {
        Ctx& c = *static_cast<Ctx*>(p);
        if (WI::WorldOf(actor) != WI::CurrentWorld()) return;
        if (R::ClassNameOf(actor) != L"ATV_C") return;
        const std::wstring key = A::GetKeyString(actor);
        if (!coop::atv_sync::AnnounceableAtvKey(key)) return;
        c.out->push_back(Found{ key, actor, R::ClassNameOf(actor) });
    }, &ctx);
    return out;
}

// The full condition a new ATV carries, written and then EXPRESSED through the game's own
// reducers, in the apply's own order (spare first, tires second -- both chain updDirt -- and
// health last). The fresh actor's own snapshot supplies what the game defaults (tire types), so
// the write only forces what "full" means: everything present, nothing worn, nothing dirty.
bool ForceFullCondition(void* atv) {
    if (!AC::Resolved() && !AC::Resolve()) return false;
    AC::Snapshot s{};
    if (!AC::Read(atv, s)) return false;   // the types stay the fresh rig's own
    for (int i = 0; i < 4; ++i) { s.dur[i] = 100.f; s.dirt[i] = 0.f; s.fixes[i] = 0; }
    s.bodyDirt  = 0.f;
    s.spareDur  = 100.f;
    s.spareDirt = 0.f;
    s.spareFixes = 0;
    s.fuel   = 100.f;
    s.health = 100.f;
    s.mask = 0x0F;          // all four wheels present
    s.hasSpare = true;
    if (!AC::WriteAccumulators(atv, s)) return false;
    if (!AC::WritePresence(atv, s.mask, s.hasSpare)) return false;
    // The defer guard, the apply's own test: an external skipTireUpdate writer means the reducers
    // would no-op, and the values above are already the truth we came to leave.
    bool skip = true;
    if (!AC::ReadSkipTireUpdate(atv, skip)) skip = true;
    if (skip) return true;
    AC::CallVerb(atv, AC::Verb::UpdSpareTire);
    AC::CallVerb(atv, AC::Verb::UpdTires);   // ends in updDirt (measured in the apply)
    AC::CallVerb(atv, AC::Verb::UpdHealth);
    return true;
}

// One re-mint. False (with the line said) when any step refused.
bool RespawnOne(const Found& f, std::vector<std::string>& reply) {
    // The pose: beside the LOCAL player (the command exists to recover a rig its old spot ate),
    // rotated the way the player faces so it spawns pointing somewhere useful.
    void* local = coop::players::Registry::Get().Local();
    FVector at{}; FRotator rot{};
    const bool havePlayer = local && E::TryGetActorLocation(local, at) && E::TryGetActorRotation(local, rot);
    FVector oldLoc{}; FRotator oldRot{};
    const bool haveOld = A::GetRootTransform(f.actor, oldLoc, oldRot);
    if (!havePlayer && !haveOld) {
        reply.push_back("The ATV key='" + coop::text::ToUtf8(f.key) +
                        "' could not be placed: neither the player nor the rig would read a pose.");
        return false;
    }
    if (!havePlayer) { at = oldLoc; rot = oldRot; }
    FVector spawn{at.X + kOffsetXcm, at.Y + kOffsetYcm, at.Z};
    FVector onMesh{};
    const bool onNav = E::ProjectToNav(f.actor, spawn, {kProjectCm, kProjectCm, kProjectCm}, &onMesh);
    spawn = onNav ? FVector{onMesh.X, onMesh.Y, onMesh.Z + kLiftCm}
                  : FVector{spawn.X, spawn.Y, spawn.Z + kLiftCm};

    const std::string keyUtf8 = coop::text::ToUtf8(f.key);
    if (!A::DestroyMirror(f.actor)) {
        reply.push_back("The old rig key='" + keyUtf8 + "' refused to be destroyed; nothing spawned.");
        return false;
    }
    A::SpawnFail why = A::SpawnFail::None;
    void* fresh = A::SpawnMirror(f.className, spawn, rot, f.key, &why);
    if (!fresh) {
        reply.push_back("The fresh rig key='" + keyUtf8 + "' did not spawn: " +
                        std::string(A::SpawnFailText(why)) +
                        " -- the key is now free, /atvrespawn " + keyUtf8 + " tries again.");
        return false;
    }
    const bool full = ForceFullCondition(fresh);
    reply.push_back("Respawned the ATV key='" + keyUtf8 + "' beside you" +
                    std::string(onNav ? " (on the NavMesh)" : " (open ground)") +
                    (full ? " -- full tank, all wheels, the spare, no wear."
                          : " -- the rig is fresh; its condition could not be forced full (the "
                            "condition layout did not resolve)."));
    UE_LOGI("atv_respawn: re-minted key='%ls' class='%ls' at %.0f,%.0f,%.0f (%s), condition %s",
            f.key.c_str(), f.className.c_str(), spawn.X, spawn.Y, spawn.Z,
            onNav ? "NavMesh" : "open ground", full ? "forced full" : "fresh defaults");
    return true;
}

}  // namespace

std::vector<std::string> Respawn(const std::string& keyWord) {
    std::vector<std::string> reply;
    // Solo play is a legitimate caller: the spawn is local, and with no session there is no
    // announce to send -- the fresh rig is simply the world's.
    if (!A::EnsureResolved() || !ue_wrap::fname_utils::Resolved()) {
        reply.push_back("The ATV wrapper or the name library is not resolved yet -- try again in a moment.");
        return reply;
    }
    const std::vector<Found> atvs = WalkAtvs();
    if (atvs.empty()) {
        reply.push_back("This world holds no ATV to respawn.");
        return reply;
    }
    if (keyWord.empty()) {
        if (atvs.size() > 1) {
            std::string keys;
            for (const Found& f : atvs) {
                if (!keys.empty()) keys += ", ";
                keys += "'" + coop::text::ToUtf8(f.key) + "'";
            }
            reply.push_back("Several ATVs stand here (" + keys +
                            ") -- name one: /atvrespawn <key>.");
            return reply;
        }
        RespawnOne(atvs.front(), reply);
        return reply;
    }
    for (const Found& f : atvs) {
        if (!coop::commands::EqualsAsciiNoCase(coop::text::ToUtf8(f.key), keyWord)) continue;
        RespawnOne(f, reply);
        return reply;
    }
    reply.push_back("No ATV is keyed '" + keyWord + "'.");
    return reply;
}

bool RegisterCommand(coop::commands::Registry& reg) {
    coop::commands::CommandSpec c;
    c.name = "atvrespawn";
    c.description = "Re-mints an ATV beside you with full fuel, all wheels, the spare and no wear.";
    c.args = {{"key", coop::commands::ArgKind::Word, true}};
    c.handler = [](coop::commands::Context& ctx) {
        const std::string word = ctx.given.size() > 0 && ctx.given[0] ? ctx.texts[0] : std::string();
        for (std::string& line : Respawn(word)) ctx.Reply(std::move(line));
    };
    return reg.Register(c, nullptr);
}

}  // namespace coop::atv_respawn
