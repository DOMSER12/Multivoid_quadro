// coop/props/hole_sync.cpp -- see coop/props/hole_sync.h.

#include "coop/props/hole_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_fresh_spawn.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a world this peer has not announced is its load

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_census.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::hole_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;

// The hole family's birth: ReceiveBeginPlay declared on dirthole_C, the base the shovel's hole
// variants inherit (a variant that does not override the event dispatches the base's UFunction, so
// one watch covers the family; one that does declares its own, and its births stay local -- said
// where the verb watches say the same thing). Every spawn route fires it once: the game's own dig,
// the fresh-spawn lane's materialization of a wire row, the save's own load.
UE_FACT_FN(kRowHoleBirth, "hole_sync", L"dirthole_C", L"ReceiveBeginPlay", Declared);

constexpr int kTagBirth = 0x44484F4C;  // 'DHOL'

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

unsigned long long g_clientIntents = 0, g_hostExpressed = 0, g_hostRecords = 0;
unsigned long long g_skippedRemote = 0, g_skippedLoad = 0, g_spawned = 0, g_dup = 0, g_dropped = 0;

// A birth waiting for its Key: the drain reads it a tick or more after BeginPlay, the shape the
// drop intent's drain keeps (its Key is restored by loadData there; the hole's is minted by the
// construction script -- either way the read is the gate, never an assumption). Game thread only.
struct Birth {
    void*    actor;
    int      tries;
    uint8_t  awaitSlot;  // 0 = this peer authored the birth itself; else whose record is in flight
};
std::vector<Birth> g_pending;

// The drop intent's wire short-string helpers, the same shape byte for byte.
template <size_t N>
void FillWireStr(uint8_t& len, char (&data)[N], const std::wstring& s) {
    len = 0;
    for (size_t i = 0; i < s.size() && i < (N - 1); ++i) data[len++] = static_cast<char>(s[i]);
}

void OnBirthPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object) return;
    // A hole this peer is materializing off the wire (the host's PropSpawn row reaching a client,
    // or the host's own copy of a client's intent, which Materialize mirror-marks before its
    // finish) is already named on the wire: the host authored it, and expressing or intenting it
    // again would loop. The host's copy of an intent enters the drain from OnHoleBirthIntent
    // directly, which is why the mark is asked on the host too.
    if (prop_echo_suppress::IsMirrorSpawn(call.object)) {
        ++g_skippedRemote;
        return;
    }
    // CLIENT ONLY: a world this client has not announced ready is its own save's load -- the join
    // snapshot and the save transfer already carry those holes to every peer, and the join
    // quiescence gate (the record refresh lane's) says the same for the intents a load would
    // otherwise fire. The host never announces (the check is false on it by definition), and its
    // own save's load births nothing a client has not already loaded from the same save.
    if (s->role() == coop::net::Role::Client) {
        if (!coop::net_pump::IsInAnnouncedWorld(call.object) ||
            !coop::join_membership_sweep::HasLoadTailQuiesced()) {
            ++g_skippedLoad;
            return;
        }
    }
    g_pending.push_back(Birth{call.object, 0, 0});
}

void Drain(coop::net::Session& s) {
    std::vector<Birth> keep;
    for (Birth& b : g_pending) {
        if (!R::IsLive(b.actor)) continue;  // destroyed inside its birth window (filled, purged)
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(b.actor);
        if (key.empty() || key == L"None") {
            if (++b.tries > 120) {
                ++g_dropped;
                UE_LOGW("hole_sync: a %ls birth never grew a Key (%p) -- nothing to address a birth "
                        "or a record to, dropped",
                        R::ClassNameOf(b.actor).c_str(), b.actor);
                continue;
            }
            keep.push_back(b);
            continue;
        }
        if (s.role() == coop::net::Role::Host) {
            // The host authors every keyed prop: the ordinary lifecycle express names the birth to
            // every client (PropSpawn), idempotent against the Init-POST latch the express shares
            // with the spawn seams. The record follows it -- except while the author's own record
            // is still in flight behind an intent, which the wait's land publishes instead: what
            // this copy holds before it lands is the class default.
            prop_lifecycle::ExpressSpawnedProp(b.actor);
            ++g_hostExpressed;
            bool record = false;
            if (b.awaitSlot == 0 || !prop_save_data::AwaitsRecordFrom(key, b.awaitSlot))
                record = prop_save_data::PublishHostBirth(&s, b.actor);
            if (record) ++g_hostRecords;
            UE_LOGI("hole_sync: HOST expressed a hole birth key='%ls' cls='%ls' record=%d%s",
                    key.c_str(), R::ClassNameOf(b.actor).c_str(), record ? 1 : 0,
                    b.awaitSlot ? " (the author's record is awaited)" : "");
        } else {
            // The client's own dig: keep the local hole and report it. The host spawns the
            // authoritative copy at this class, key and pose, its express tells the other clients,
            // and the row comes back to us to adopt by the exact key. The record intent follows on
            // the same FIFO, the drop intent's ordering.
            ue_wrap::FVector loc{};
            ue_wrap::FRotator rot{};
            if (!E::TryGetActorLocation(b.actor, loc) || !E::TryGetActorRotation(b.actor, rot)) {
                ++g_dropped;
                UE_LOGW("hole_sync: a hole birth's pose did not read (%p) -- dropped", b.actor);
                continue;
            }
            const auto scl = E::GetActorScale3D(b.actor);
            coop::net::PropDropIntentPayload p{};
            FillWireStr(p.className.len, p.className.data, R::ClassNameOf(b.actor));
            FillWireStr(p.key.len, p.key.data, key);
            p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
            p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
            p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
            p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
            // physFlags stay 0 and throwSeq 0: a hole is static terrain furniture (a non-prop's
            // parity flags read is meaningless) and a dig is not a throw.
            if (!s.SendReliable(coop::net::ReliableKind::HoleBirthIntent, &p, sizeof(p))) {
                UE_LOGW("hole_sync: the hole birth intent for key='%ls' was not sent (the session "
                        "refused it); the hole stays local", key.c_str());
                ++g_dropped;
                continue;
            }
            ++g_clientIntents;
            coop::prop_save_data::Publish(&s, b.actor, key);
            coop::join_membership_sweep::RecordClaimIfTracking(b.actor);
            UE_LOGI("hole_sync: CLIENT authored a hole birth key='%ls' cls='%ls' loc=(%.1f,%.1f,%.1f)%s",
                    key.c_str(), R::ClassNameOf(b.actor).c_str(), p.locX, p.locY, p.locZ,
                    coop::prop_save_data::Covers(b.actor) ? " +record" : "");
        }
    }
    g_pending.swap(keep);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    if (sg::WatchName(kRowHoleBirth, kTagBirth, nullptr, &OnBirthPost)) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("hole_sync: the script gate refused the dirthole birth watch -- mid-session digs stay "
            "local for the rest of this process");
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowHoleBirth, kTagBirth)) {
            g_settled = true;
            UE_LOGI("hole_sync: the dirthole birth watch is live");
        } else if (g_reg == Reg::Refused && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("hole_sync: the dirthole birth watch is dead (refused, or resolved into a full "
                    "table) -- mid-session digs stay local");
        } else if (session.running()) {
            // The watch is still waiting for its class. A class the build spells otherwise holds
            // this forever with no line, so once in 30 s the index's class log is walked for the
            // names the hole family could plausibly carry, functions named -- the real spelling
            // lands in the log instead of a guess.
            static std::chrono::steady_clock::time_point s_next{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_next) {
                s_next = now + std::chrono::seconds(30);
                static const wchar_t* const kSubs[] = {L"hole"};
                UE_LOGW("hole_sync: the birth watch is still pending on dirthole_C::ReceiveBeginPlay "
                        "-- the class may not be loaded yet, or the name spells otherwise; census "
                        "of loaded classes matching 'hole':");
                if (!ue_wrap::class_census::LogMatchingClasses("hole_sync", kSubs, 1))
                    UE_LOGW("hole_sync: census found no loaded class matching 'hole' -- dig a hole "
                            "once and the family's class will load into the next census");
            }
        }
    }
    if (!session.running() || g_pending.empty()) return;
    Drain(session);
}

void OnHoleBirthIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                       uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const std::wstring cls = coop::remote_prop_spawn::ClassNameToWString(p.className);
    if (key.empty() || cls.empty()) {
        UE_LOGW("hole_sync: a hole birth intent with an empty key or class (slot %u) -- dropping",
                static_cast<unsigned>(senderSlot));
        return;
    }
    // Dup guard: the host already holds the key (a retry, or both peers dug into the same ground).
    void* existing = nullptr;
    if (prop_key_index::Resolve(key, &existing) == prop_key_index::Read::Hit) {
        ++g_dup;
        static uint64_t s_nextSayMs = 0;
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now >= s_nextSayMs) {
            s_nextSayMs = now + 10000;
            UE_LOGI("hole_sync: slot %u's hole key='%ls' already lives here -- the intent is a "
                    "retry, nothing spawned", static_cast<unsigned>(senderSlot), key.c_str());
        }
        return;
    }
    // The authoritative copy. The PropSpawnPayload shape rides into Materialize for the transform
    // (and the parity fields a non-prop never reads); the element id is 0 -- no eid bookkeeping,
    // the express's own element pass names the birth to the clients. The class resolve is the
    // object index, so an attacker's unknown class costs a lookup, not a walk.
    coop::net::PropSpawnPayload sp{};
    FillWireStr(sp.className.len, sp.className.data, cls);
    FillWireStr(sp.key.len, sp.key.data, key);
    sp.locX = p.locX; sp.locY = p.locY; sp.locZ = p.locZ;
    sp.rotPitch = p.rotPitch; sp.rotYaw = p.rotYaw; sp.rotRoll = p.rotRoll;
    sp.scaleX = p.scaleX; sp.scaleY = p.scaleY; sp.scaleZ = p.scaleZ;
    sp.elementId = 0;
    void* spawned = prop_fresh_spawn::Materialize(sp, senderSlot, cls, key, L"" /*propName*/,
                                                  /*skipBind=*/true,
                                                  ue_wrap::object_index::ClassByName);
    if (!spawned) {
        UE_LOGE("hole_sync: the host's authoritative hole for key='%ls' (%ls) did not spawn",
                key.c_str(), cls.c_str());
        return;
    }
    ++g_spawned;
    // The author's record is in flight behind the intent: until it lands, this birth must not
    // publish its own (the class default), and the drain's record publish asks the same wait.
    prop_save_data::ExpectRecordFor(spawned, key, senderSlot);
    // The key Materialize wrote went through the class's own setKey; a class whose chain carries
    // none left the construction mint in the field, and every later record would address a key
    // this actor does not hold. Verify, and write the field directly when the setter was missing.
    const std::wstring got = ue_wrap::prop::GetInteractableKeyString(spawned);
    if (got != key) {
        void* c = R::ClassOf(spawned);
        const int32_t off = c ? R::FindPropertyOffset(c, L"Key") : -1;
        if (off >= 0) {
            const R::FName kfn = ue_wrap::fname_utils::StringToFName(key);
            if (kfn.ComparisonIndex != 0) {
                std::memcpy(reinterpret_cast<uint8_t*>(spawned) + off, &kfn, sizeof(kfn));
                UE_LOGW("hole_sync: key='%ls' written to the field directly (setKey gave '%ls')",
                        key.c_str(), got.empty() ? L"None" : got.c_str());
            }
        } else {
            UE_LOGE("hole_sync: no Key offset on the spawned hole for key='%ls' -- its records "
                    "cannot find it", key.c_str());
        }
    }
    g_pending.push_back(Birth{spawned, 0, senderSlot});  // the express runs on the drain
    UE_LOGI("hole_sync: HOST spawned slot %u's hole key='%ls' cls='%ls' loc=(%.1f,%.1f,%.1f)",
            static_cast<unsigned>(senderSlot), key.c_str(), cls.c_str(), p.locX, p.locY, p.locZ);
}

void OnDisconnect() {
    if (g_clientIntents || g_hostExpressed || g_hostRecords || g_skippedRemote || g_skippedLoad ||
        g_spawned || g_dup || g_dropped)
        UE_LOGI("hole_sync: session end -- client intents=%llu host expressed=%llu host "
                "records=%llu spawned from intents=%llu skipped wire-born=%llu skipped "
                "load-born=%llu dup-guarded=%llu dropped=%llu",
                static_cast<unsigned long long>(g_clientIntents),
                static_cast<unsigned long long>(g_hostExpressed),
                static_cast<unsigned long long>(g_hostRecords),
                static_cast<unsigned long long>(g_spawned),
                static_cast<unsigned long long>(g_skippedRemote),
                static_cast<unsigned long long>(g_skippedLoad),
                static_cast<unsigned long long>(g_dup),
                static_cast<unsigned long long>(g_dropped));
    g_clientIntents = g_hostExpressed = g_hostRecords = 0;
    g_skippedRemote = g_skippedLoad = g_spawned = g_dup = g_dropped = 0;
    g_pending.clear();
}

}  // namespace coop::hole_sync
