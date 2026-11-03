// coop/props/prop_record_refresh.cpp -- see coop/props/prop_record_refresh.h.

#include "coop/props/prop_record_refresh.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_census.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>

namespace coop::props::prop_record_refresh {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

unsigned long long g_cPublished = 0;
unsigned long long g_cRelooked  = 0;

// `opened` is read to drive the look below; `name` and init() are what re-derive it.
int32_t g_openedOff = -1;
int32_t g_nameOff   = -1;

// The box's two names, exactly as its own Blueprint spells them: the open action sets
// 'drivebox_sb', putLidOn sets 'drivebox_s' back.
constexpr const wchar_t* kNameOpen   = L"drivebox_sb";
constexpr const wchar_t* kNameClosed = L"drivebox_s";

bool ReadOpened(void* box, bool& out) {
    if (!box) return false;
    if (g_openedOff < 0) {
        void* cls = R::ClassOf(box);
        if (!cls) return false;
        g_openedOff = R::FindPropertyOffset(cls, L"opened");
        if (g_openedOff < 0) return false;
    }
    out = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(box) + g_openedOff);
    return true;
}

// A record has just landed on `box`. Its LOOK follows `name`, NOT `opened`: Aprop_C::init picks
// the mesh from that field and the box's own init() chains to it. And `name` is the one thing the
// record deliberately does not carry -- save_record's splice keeps element 0 of every group, which
// is where Aprop_C writes name, for the RECEIVER, so a sender's name is dropped by design. So the
// look is re-derived here from the state that did travel, with the pair the Blueprint itself uses,
// and init() re-runs the mesh pick. Idempotent: a name already right returns before the write, so
// an init() that loops back through upd() stops on the second pass.
void RefreshLook(void* box, bool opened) {
    void* cls = R::ClassOf(box);
    if (!cls) return;
    if (g_nameOff < 0) g_nameOff = R::FindPropertyOffset(cls, L"name");
    // Looked up on the box's own class at each use, through the lookup memoised by the class's slot
    // and serial: two hash lookups and two slot-and-serial checks, and never a function of a class that
    // is gone.
    void* const initFn = R::FindDispatchFunctionCached(cls, L"init");
    if (g_nameOff < 0 || !initFn) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("prop_record_refresh: no `name` offset or no init() on the box -- a mirrored open "
                    "will set the state without the open mesh (it behaves open and looks shut)"); }
        return;
    }
    const wchar_t* want = opened ? kNameOpen : kNameClosed;
    if (ue_wrap::prop::GetPropNameString(box) == want) return;  // already the right look
    R::FName n = ue_wrap::fname_utils::StringToFName(want);
    std::memcpy(reinterpret_cast<uint8_t*>(box) + g_nameOff, &n, sizeof(n));
    ue_wrap::ParamFrame f(initFn);
    if (f.valid()) ue_wrap::Call(box, f);
    ++g_cRelooked;
    UE_LOGI("prop_record_refresh: a record landed on a drive box -> opened=%d, look re-derived to '%ls'",
            opened ? 1 : 0, want);
}

// The reel case's record fields, for the logs: `lid`, and each slot's reel progress (-1 = empty).
int32_t g_lidOff = -1, g_topOff = -1, g_bottomOff = -1;

std::string DescribeBox(void* box) {
    bool opened = false;
    return ReadOpened(box, opened) ? (opened ? "opened=1" : "opened=0") : "opened=?";
}

std::string DescribeReelCase(void* rc) {
    if (g_lidOff < 0) {
        void* cls = R::ClassOf(rc);
        if (!cls) return "?";
        g_lidOff    = R::FindPropertyOffset(cls, L"lid");
        g_topOff    = R::FindPropertyOffset(cls, L"reeltop");
        g_bottomOff = R::FindPropertyOffset(cls, L"reelBottom");
    }
    if (g_lidOff < 0 || g_topOff < 0 || g_bottomOff < 0) return "?";
    const char* b = reinterpret_cast<const char*>(rc);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "lid=%d reeltop=%.2f reelBottom=%.2f",
                  *reinterpret_cast<const bool*>(b + g_lidOff) ? 1 : 0,
                  *reinterpret_cast<const float*>(b + g_topOff),
                  *reinterpret_cast<const float*>(b + g_bottomOff));
    return buf;
}

// The mining frame's record fields, for the logs: the seated GPUs (its `gpus` TArray's count)
// and the points it has accumulated. Both offsets are resolved on the frame's own class and kept;
// a field that does not resolve reads as a question mark, which is a log shape, never a lane gate.
int32_t g_gpusOff = -1, g_pointsOff = -1;

std::string DescribeMiner(void* miner) {
    void* cls = R::ClassOf(miner);
    if (!cls) return "?";
    if (g_gpusOff < 0) {
        g_gpusOff   = R::FindPropertyOffset(cls, L"gpus");
        g_pointsOff = R::FindPropertyOffset(cls, L"accumulatedPoints");
    }
    if (g_gpusOff < 0 && g_pointsOff < 0) return "?";
    const char* b = reinterpret_cast<const char*>(miner);
    char buf[80];
    // A TArray's count sits at +4 (data pointer, then Num, then Max); the walk the count
    // justifies is the receiver's business, and a log must not cost one.
    const int32_t gpus = g_gpusOff >= 0
        ? *reinterpret_cast<const int32_t*>(b + g_gpusOff + 4) : -1;
    if (g_gpusOff >= 0 && g_pointsOff >= 0)
        std::snprintf(buf, sizeof(buf), "gpus=%d points=%.0f", gpus,
                      *reinterpret_cast<const float*>(b + g_pointsOff));
    else if (g_gpusOff >= 0)
        std::snprintf(buf, sizeof(buf), "gpus=%d", gpus);
    else
        std::snprintf(buf, sizeof(buf), "points=%.0f",
                      *reinterpret_cast<const float*>(b + g_pointsOff));
    return buf;
}

// The dug hole's record fields, for the logs: `matType` (the ground the hole went into) and
// whether the hole holds a plant (its `plant` object reference is set). Offsets resolved on the
// hole's own class and kept; a field that does not resolve reads as a question mark.
int32_t g_matTypeOff = -1, g_plantOff = -1;

std::string DescribeHole(void* hole) {
    void* cls = R::ClassOf(hole);
    if (!cls) return "?";
    if (g_matTypeOff < 0) {
        g_matTypeOff = R::FindPropertyOffset(cls, L"matType");
        g_plantOff   = R::FindPropertyOffset(cls, L"plant");
    }
    if (g_matTypeOff < 0 && g_plantOff < 0) return "?";
    const char* b = reinterpret_cast<const char*>(hole);
    char buf[64];
    if (g_matTypeOff >= 0 && g_plantOff >= 0)
        std::snprintf(buf, sizeof(buf), "matType=%d plant=%d",
                      *reinterpret_cast<const uint8_t*>(b + g_matTypeOff),
                      *reinterpret_cast<void* const*>(b + g_plantOff) != nullptr ? 1 : 0);
    else if (g_plantOff >= 0)
        std::snprintf(buf, sizeof(buf), "plant=%d",
                      *reinterpret_cast<void* const*>(b + g_plantOff) != nullptr ? 1 : 0);
    else
        std::snprintf(buf, sizeof(buf), "matType=%d",
                      *reinterpret_cast<const uint8_t*>(b + g_matTypeOff));
    return buf;
}

// The watched classes. `relook`: the drive box's look does not follow its record and is
// re-derived on receipt (RefreshLook); the reel case's upd() reads `lid` itself, and the mining
// frame's loadData applies its own record the same way, so neither re-derives anything.
//
// prop_miner_C is the mining frame -- the GPU/cooler farm the shop sells GPUs and coolers for
// (its `gpus`, its `upg_gpu`/`upg_slots`/`upg_fans`, its `accumulatedPoints`). Every mutation a
// player drives (seating and taking hardware, the upgrade, the repair) and every tick the game
// itself makes (the points it accumulates while it runs, the fans, the overheat) ends in its own
// upd(), the same convention the drive box and the reel case keep -- so one watch on upd()
// republishes the record at the game's own cadence, event-driven, where the record poll used to
// re-read it once a second from a rotating slice.
UE_FACT_FN(kRowUpdBox,     "prop_record_refresh", L"prop_box_C",     L"upd", Dispatch);
UE_FACT_FN(kRowUpdReelbox, "prop_record_refresh", L"prop_reelbox_C", L"upd", Dispatch);
UE_FACT_FN(kRowUpdMiner,   "prop_record_refresh", L"prop_miner_C",   L"upd", Dispatch);

// The dug hole (Adirthole_C, the shovel's holes; the buried-item and rake variants inherit its
// bodies, and a watch on the base's own UFunction reaches an inherited dispatch the same way the
// camera's use reaches a subclass). A hole has no upd() of its own: its record changes under the
// verbs a player's E-press and the game's own item flow run -- the use (planting a seed,
// collecting, digging the item up), its latent tail, the dig-up, and the buried-item process --
// so those four verbs are watched Declared on the hole's class, and each publishes the record the
// verb left. The growth the seed then makes is the plant prop's own record, whose class the
// record poll still names.
UE_FACT_FN(kRowHoleUse,      "prop_record_refresh", L"dirthole_C", L"playerUsedOn",       Declared);
UE_FACT_FN(kRowHoleUseDelay, "prop_record_refresh", L"dirthole_C", L"playerUsedOn_delay", Declared);
UE_FACT_FN(kRowHoleDigUp,    "prop_record_refresh", L"dirthole_C", L"digUp",              Declared);
UE_FACT_FN(kRowHoleBury,     "prop_record_refresh", L"dirthole_C", L"processBuriedItem",  Declared);

struct Watched {
    const ue_wrap::facts::Function* row;
    int tag;
    bool relook;
    std::string (*describe)(void*);
};
constexpr Watched kWatched[] = {
    { &kRowUpdBox,     0x44424F58, true,  &DescribeBox },       // 'DBOX'
    { &kRowUpdReelbox, 0x5242584F, false, &DescribeReelCase },  // 'RBXO'
    { &kRowUpdMiner,   0x4D494E52, false, &DescribeMiner },     // 'MINR'
    { &kRowHoleUse,      0x44485531, false, &DescribeHole },     // 'DHU1'
    { &kRowHoleUseDelay, 0x44485532, false, &DescribeHole },     // 'DHU2'
    { &kRowHoleDigUp,    0x44484455, false, &DescribeHole },     // 'DHDU'
    { &kRowHoleBury,     0x44484249, false, &DescribeHole },     // 'DHBI'
};
// The UFunction each watch was armed on, so a re-armed one can be recognised. A world change can
// re-create a Blueprint class and its functions (127 were in a same-process rejoin; these two kept
// theirs): at a new address the old watch would never fire again, and at a recycled one it would
// fire for whatever lives there. An exact watch is re-validated; the neighbouring lanes use name
// watches, which resolve themselves.
ue_wrap::facts::Resolved g_watchedFn[std::size(kWatched)] = {};  // game thread only

const Watched* ByTag(int tag) {
    for (const Watched& w : kWatched)
        if (w.tag == tag) return &w;
    return nullptr;
}

// Drill red (pr64_record_window_red_ms, read only while the pr64 drill is set): puts back the leading-edge window this
// lane no longer has. A publish for a box whose last publish is younger than the row's milliseconds is dropped, so a
// change that follows another closely never reaches the far peer. Keyed by the box's interactable key; game thread.
std::unordered_map<std::wstring, uint64_t> g_lastPublishMs;

bool DroppedByRedWindow(const std::wstring& key) {
    static const long kRedMs =
        coop::config::ResolveEnum(coop::config_registry::rows::pr64_drill) != "off"
            ? coop::config::ResolveInt(coop::config_registry::rows::pr64_record_window_red_ms) : 0;
    if (kRedMs <= 0) return false;
    const uint64_t now = coop::net::NowMs();
    const auto it = g_lastPublishMs.find(key);
    if (it != g_lastPublishMs.end() && now - it->second < static_cast<uint64_t>(kRedMs)) {
        UE_LOGI("prop_record_refresh: RED window dropped a publish for key='%ls' (%llu ms after the last)", key.c_str(),
                static_cast<unsigned long long>(now - it->second));
        return true;
    }
    g_lastPublishMs[key] = now;
    return false;
}

void OnUpdPost(const sg::Call& call) {
    const Watched* w = ByTag(call.tag);
    if (!w || !call.object) return;
    // Our own apply is the one caller to ignore: prop_save_data puts a record in through the
    // prop's loadData, whose tail calls upd(), so without this an applied record would publish
    // itself straight back and the two peers would trade one record forever. The neighbouring
    // lanes gate on the same flag for the same reason. Logged rather than dropped silently: this
    // line is the far side's receipt, and it says which state actually landed.
    if (call.fromOurCode) {
        bool opened = false;
        if (w->relook) {
            if (ReadOpened(call.object, opened)) RefreshLook(call.object, opened);
        } else {
            UE_LOGI("prop_record_refresh: a record landed on %ls -> %s", w->row->Class(),
                    w->describe(call.object).c_str());
        }
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    void* actor = call.object;
    // The load churn is not a change. A joining client runs every save prop's loadData, which
    // ends in upd(), so without this the join would send the host an intent per prop describing
    // the state the host has just sent us. The host's world is the reconciled authority, so the
    // gate never holds there.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        return;
    }
    // A host with no world-ready peer has nobody to publish to: the same test prop_save_data::Publish
    // returns on before its capture, asked here before the key string is built.
    if (s->role() == coop::net::Role::Host && !s->AnyWorldReadyPeer()) return;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None") {
        // A prop mid-construction: the key is minted inside the construction script, which calls
        // upd() itself. Nothing to address a record to, and the birth paths publish it anyway.
        return;
    }
    if (DroppedByRedWindow(key)) return;
    // Every native mutation has a final record, even when another mutation just ran. A time
    // window cannot distinguish duplicate notifications from a newly inserted drive or reel.
    // Host: the record to every peer. Client: an intent the host validates and re-publishes, which
    // is also the acknowledgement. Either way the far side's loadData applies it and calls upd(),
    // so the lid, the contents and the repaint all land together.
    if (coop::prop_save_data::Publish(s, actor, key)) {
        ++g_cPublished;
        UE_LOGI("prop_record_refresh: published %ls::%ls key='%ls' %s (%s)", w->row->Class(),
                w->row->Name(), key.c_str(), w->describe(actor).c_str(),
                s->role() == coop::net::Role::Client ? "client intent" : "host broadcast");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!GT::IsGameThread()) return;
    // Throttle the class walks while a class is unresolved, the shape prop_spawn_authoring and
    // prop_drop_intent use: this Install is the per-tick retry pump, a FindClass MISS is not cached
    // (a class can load later), and a walk renders a name per object, so an unresolved class would
    // cost one full object-array walk per frame for as long as the world goes without one. A class
    // that IS resolved costs a memoised lookup, so the pass below still runs every frame for it and
    // re-arms the watch when a world load hands back a new function.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }
    for (size_t i = 0; i < std::size(kWatched); ++i) {
        const Watched& w = kWatched[i];
        void* cls = R::FindClass(w.row->Class());
        if (!cls) {
            // Not loaded yet -- a second of frames from now. Said once per row, so a class this
            // build never loads by this name is one log read, not silence.
            static bool s_saidMissingCls[std::size(kWatched)] = {};
            if (!s_saidMissingCls[i]) {
                s_saidMissingCls[i] = true;
                UE_LOGW("prop_record_refresh: class %ls is not loaded (yet?) -- its %ls watch waits "
                        "for the class to load", w.row->Class(), w.row->Name());
            }
            // The mining frame and the holes are this fork's additions, their class names not
            // measured on the reference build: if the name spells otherwise, the census names what
            // the build does carry, once in 30 s while the miss goes on.
            if (w.row == &kRowUpdMiner || w.row == &kRowHoleUse) {
                static std::chrono::steady_clock::time_point s_next{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= s_next) {
                    s_next = now + std::chrono::seconds(30);
                    static const wchar_t* const kSubs[] = {L"miner", L"hole"};
                    UE_LOGW("prop_record_refresh: census of loaded classes matching 'miner'/'hole' "
                            "(the family's class loads with its first instance):");
                    ue_wrap::class_census::LogMatchingClasses("prop_record_refresh", kSubs, 2);
                }
            }
            s_retry = 60;
            continue;
        }
        // The memoised lookup holds its answer by slot and serial, so a class re-created by a world
        // load answers with the NEW function and the comparison below re-arms on it.
        void* fn = R::FindDispatchFunctionCached(cls, w.row->Name());
        if (fn && fn == g_watchedFn[i].Fn()) continue;  // armed, on the function that is here now
        if (!fn) {
            static bool s_saidMissing[std::size(kWatched)] = {};
            if (!s_saidMissing[i]) {
                s_saidMissing[i] = true;
                UE_LOGW("prop_record_refresh: %ls::%ls UFunction not found -- its record will only "
                        "travel on its birth paths (an in-place change stays local)", w.row->Class(),
                        w.row->Name());
            }
            s_retry = 60;
            continue;
        }
        // The script gate, not a ProcessEvent observer: the prop calls its own upd() from inside
        // its Blueprint, a route that never reaches ProcessEvent, and an observer there saw
        // nothing. The watch is on the exact UFunction rather than on the name -- `upd` alone is
        // declared by half the props in the game, and the hole family's verbs by most of them.
        if (g_watchedFn[i] && g_watchedFn[i].Fn() != fn)
            sg::Unwatch(g_watchedFn[i], w.tag, nullptr, &OnUpdPost);  // the world that owned it is gone
        const ue_wrap::facts::Resolved armed = ue_wrap::facts::Resolve(*w.row);
        if (!armed || !sg::Watch(armed, w.tag, nullptr, &OnUpdPost)) {
            // Back off with the same sixty frames the unresolved-class branch takes, and say so
            // once: a refusal persists (the gate did not install, or its table is full), so an
            // unthrottled retry would warn and re-resolve every frame for the life of the process.
            s_retry = 60;
            static bool s_saidRefused = false;
            if (!s_saidRefused) {
                s_saidRefused = true;
                UE_LOGW("prop_record_refresh: script-gate watch on %ls::%ls refused -- retrying",
                        w.row->Class(), w.row->Name());
            }
            continue;
        }
        const bool rearm = static_cast<bool>(g_watchedFn[i]);
        g_watchedFn[i] = armed;
        UE_LOGI("prop_record_refresh: %hs %ls::%ls -- it republishes its own save record on "
                "every in-place change", rearm ? "re-armed on" : "watching", w.row->Class(),
                w.row->Name());
    }
}

void OnDisconnect() {
    if (g_cPublished || g_cRelooked)
        UE_LOGI("prop_record_refresh: session end -- published=%llu relooked=%llu",
                g_cPublished, g_cRelooked);
    g_cPublished = 0;
    g_cRelooked  = 0;
    g_lastPublishMs.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::props::prop_record_refresh
