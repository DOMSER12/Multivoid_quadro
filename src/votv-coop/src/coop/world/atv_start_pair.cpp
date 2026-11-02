// coop/world/atv_start_pair.cpp -- see coop/world/atv_start_pair.h.

#include "coop/world/atv_start_pair.h"

#include "coop/interactables/atv_sync.h"     // AnnounceableAtvKey: the wire's own key test
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/session/host_world_settle.h"  // Settled: the save has finished placing its world
#include "ue_wrap/core/fname_utils.h"        // Resolved: the spawn's key must convert to a name
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"       // IsSeeded / Backlog: the walk can be judged whole
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"             // EnsureResolved, ForEachAtv, SpawnMirror
#include "ue_wrap/engine/engine_nav.h"       // ProjectToNav
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <cstring>

namespace coop::atv_start_pair {
namespace {

namespace A  = ue_wrap::atv;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;
using ue_wrap::FVector;
using ue_wrap::FRotator;

constexpr uint64_t kPollMs    = 2000;   // one readiness walk and at most one attempt per poll
constexpr uint32_t kAttemptCap = 15;    // attempts per world; then the window closes, said once
// The second rig is born this far from the save's own, on the world axes: beside it, the way
// the base's parking holds two, with the NavMesh projection finding the ground it rests on.
constexpr float kOffsetXcm = 250.f;
constexpr float kOffsetYcm = -250.f;
constexpr float kProjectCm = 300.f;     // the NavMesh query's box about the offset point,
                                        // the dev spawn drill's measured reach
constexpr float kLiftCm    = 50.f;      // born this far above the point it rests on (the drill's)
// The spawn's key: ASCII, within what a WireKey carries, namespaced so no key the game mints
// collides with it. Fixed, so every session and every peer's mirror name this rig the same way.
const wchar_t* const kSecondKey = L"MVOID_ATV_2";

// All state game-thread-only, and every field a per-world one: a new generation resets the
// whole record on sight.
uint32_t    g_gen          = 0;      // the world the record below belongs to
uint64_t    g_lastWalkMs   = 0;      // the poll's clock
uint32_t    g_attempts     = 0;      // spawn attempts spent in this world
uint32_t    g_spawnedAtGen = 0;      // the world that got its second ATV (0 = none has)
bool        g_saidGiveUp   = false;  // the window's closing line, once per world
const char* g_saidWhy      = nullptr;  // the failed step, once per reason per world

void ResetRecord(uint32_t gen) {
    g_gen          = gen;
    g_lastWalkMs   = 0;
    g_attempts     = 0;
    g_spawnedAtGen = 0;
    g_saidGiveUp   = false;
    g_saidWhy      = nullptr;
}

// What one walk saw of this world's ATVs: the count of the game's own keyed quads, the first
// one's actor and pose (the second is born beside it), and how many ATVs the index holds that
// are not readable yet -- a count that cannot be judged while one is.
struct Found {
    size_t   n = 0;
    size_t   loading = 0;
    void*    actor = nullptr;
    FVector  loc{};
    FRotator rot{};
};

Found WalkStartingAtv() {
    Found found;
    struct Ctx { Found* found; } ctx{&found};
    found.loading = A::ForEachAtv([](void* p, void* actor, int32_t /*idx*/) {
        Ctx& c = *static_cast<Ctx*>(p);
        if (WI::WorldOf(actor) != WI::CurrentWorld()) return;
        // The game's own quad, exactly: the witch car is an ATV lineage of its own and never
        // the rig this feature pairs.
        if (R::ClassNameOf(actor) != L"ATV_C") return;
        if (!coop::atv_sync::AnnounceableAtvKey(A::GetKeyString(actor))) return;
        if (c.found->n == 0) A::GetRootTransform(actor, c.found->loc, c.found->rot);
        ++c.found->n;
    }, &ctx);
    return found;
}

// A failed attempt, said once per reason per world; the window's fifteenth failure closes it,
// said once. `text` is a static phrase (the spawn step's, or the NavMesh miss's own).
void NoteFail(const char* text) {
    if (!g_saidWhy || std::strcmp(g_saidWhy, text) != 0) {
        g_saidWhy = text;
        UE_LOGW("atv_start_pair: the second ATV's spawn did not go (attempt %u of %u) -- %s",
                g_attempts, kAttemptCap, text);
    }
    if (g_attempts >= kAttemptCap && !g_saidGiveUp) {
        g_saidGiveUp = true;
        UE_LOGW("atv_start_pair: %u spawn attempts did not put the second ATV into this world "
                "(the last one: %s) -- the window closes until the next world", kAttemptCap, text);
    }
}

// One spawn: beside the save's rig, on the ground. NavMesh first -- a point it certifies is the
// one ground the engine itself vouches for. But VOTV bakes its NavMesh for the AI walkers, and
// the base's parking lot (where the save's own rig stands) is NOT on it -- 15 attempts in the
// field all died "no NavMesh point near the offset point". So the miss is not a failure any
// more: it is the yard telling us the offset lands in open ground. The fallback spawns there
// anyway, at the first rig's own height + a lift -- the rig is five simulating bodies, physics
// drops it onto the concrete, and the lot is flat. The meshed places (the garage, the rooms)
// still go through the projection, so a first rig parked indoors keeps its wall-safe placement;
// the fallback only ever fires where the mesh has nothing to say.
void AttemptSpawn(uint32_t gen, const Found& first) {
    ++g_attempts;
    FVector at{first.loc.X + kOffsetXcm, first.loc.Y + kOffsetYcm, first.loc.Z};
    FVector onMesh{};
    const bool onNav = E::ProjectToNav(first.actor, at, {kProjectCm, kProjectCm, kProjectCm}, &onMesh);
    at = onNav ? FVector{onMesh.X, onMesh.Y, onMesh.Z + kLiftCm}
               : FVector{at.X, at.Y, at.Z + kLiftCm};
    A::SpawnFail why = A::SpawnFail::None;
    void* atv = A::SpawnMirror(L"ATV_C", at, first.rot, kSecondKey, &why);
    if (!atv) {
        NoteFail(A::SpawnFailText(why));
        return;
    }
    g_spawnedAtGen = gen;
    UE_LOGI("atv_start_pair: the world held one ATV -- the second spawned beside it "
            "(key='%ls', at %.0f,%.0f,%.0f, attempt %u, %s); the hub's next pass files it under "
            "its key and the lane's announce tells the clients",
            kSecondKey, at.X, at.Y, at.Z, g_attempts,
            onNav ? "on the NavMesh" : "beside the first rig (open ground, no NavMesh there)");
}

}  // namespace

void Tick(coop::net::Session& session, bool fleeing) {
    if (session.role() != coop::net::Role::Host) return;
    const uint32_t gen = WI::Generation();
    if (g_gen != gen) ResetRecord(gen);
    if (g_spawnedAtGen == gen || g_saidGiveUp || fleeing) return;
    // The spawn path's own readiness, ahead of any walk: the ATV wrapper and the string library
    // the key converts through, an object index that is seeded and has applied its births, and a
    // world the save has finished placing -- one ATV of a world still coming up is not the count
    // to judge.
    if (!A::EnsureResolved() || !ue_wrap::fname_utils::Resolved()) return;
    if (!ue_wrap::object_index::IsSeeded() || ue_wrap::object_index::Backlog() > 0) return;
    if (!coop::host_world_settle::Settled()) return;
    const uint64_t now = coop::net::NowMs();
    if (g_lastWalkMs && now - g_lastWalkMs < kPollMs) return;
    g_lastWalkMs = now;
    const Found found = WalkStartingAtv();
    if (found.loading > 0) return;   // an ATV is still loading -- the count cannot be judged yet
    if (found.n != 1) return;        // none to pair (0), or the pair stands already (2 or more)
    AttemptSpawn(gen, found);
}

void OnSessionEnd() {
    // The world-scoped latch stays: the second ATV stands in the host's world for as long as the
    // world does, and the count rule would refuse a second spawn beside it anyway. What a new
    // session owes is the budget: a world whose window closed under a failure the next session
    // may not hit is given its attempts again.
    g_attempts   = 0;
    g_saidGiveUp = false;
    g_saidWhy    = nullptr;
    g_lastWalkMs = 0;
}

}  // namespace coop::atv_start_pair
