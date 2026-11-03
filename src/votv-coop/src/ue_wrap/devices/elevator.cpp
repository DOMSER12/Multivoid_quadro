// ue_wrap/devices/elevator.cpp -- see ue_wrap/devices/elevator.h. Engine access for the base
// cargo lift (AcargoLift_C). Offsets and verbs are resolved from the live class by name; a
// class whose `dir` or press verb does not resolve leaves the lane off, said once, since a
// guessed offset would write whatever a newer build keeps there.

#include "ue_wrap/devices/elevator.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_census.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::elevator {
namespace {

namespace R  = reflection;
namespace P  = profile;

std::atomic<bool> g_resolved{false};

int32_t g_keyOff    = -1;   // AcargoLift_C::Key (declared on the Aactor_save_C base)
int32_t g_dirOff    = -1;   // AcargoLift_C::dir, the target end the press writes
int32_t g_movingOff = -1;   // AcargoLift_C::moving, true mid-move (optional)
int32_t g_moveOff   = -1;   // AcargoLift_C::move, the UTimelineComponent* property (optional)
std::wstring g_dirClass;    // the `dir` property's FFieldClass name, for the typed read

// The lift class as this world holds it: kept only while its slot and serial still hold it,
// looked up by name again after a world gave the class a new object. Game thread.
ue_wrap::CachedObjRef g_liftClsRef;
void* LiftClass() {
    if (void* c = g_liftClsRef.Get()) return c;
    void* c = ue_wrap::object_index::ClassByName(L"cargoLift_C");
    if (c) g_liftClsRef.Set(c);
    return c;
}

// One of the lift's verbs on this instance: its class's own, memoised by the reflection layer.
void* Verb(void* lift, const wchar_t* name) {
    void* cls = lift ? R::ClassOf(lift) : nullptr;
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

// The class the lane names is not loaded (yet, or ever). A wrong name would otherwise say nothing:
// the press gate's row waits for its class forever, and the toggle-verb row with it -- the lift
// lane just looks dead with no line. Once in 30 s while the miss goes on, walk the index's class
// log for the names a lift could plausibly carry and name their functions, so the real spelling
// (if this build carries one) lands in the log instead of a guess.
void CensusMissingClass() {
    static std::chrono::steady_clock::time_point s_next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next) return;
    s_next = now + std::chrono::seconds(30);
    static const wchar_t* const kSubs[] = {L"lift", L"cargo"};
    UE_LOGW("elevator: cargoLift_C is not loaded -- the lane is off; census of loaded classes "
            "whose name contains 'lift'/'cargo':");
    if (!class_census::LogMatchingClasses("elevator", kSubs, 2))
        UE_LOGW("elevator: census found no loaded class matching 'lift' or 'cargo' -- the lift "
                "actor may not be in this world yet, or the class spells otherwise");
}

// The class loaded but one of its fields did not resolve by the name the lane knows. The field
// walk names what the class really keeps, so a renamed `dir` or timeline property is one log read
// away. A read, once per resolve failure of each field.
void CensusClassProperties(const wchar_t* missing) {
    void* cls = LiftClass();
    if (!cls) return;
    std::wstring props;
    int said = 0;
    for (const R::ObjectRef& ref : R::ChildObjectsOf(cls)) {
        if (ref.className == L"Function" || ref.className == L"DelegateFunction") continue;
        if (ref.className.find(L"Property") == std::wstring::npos) continue;
        if (!props.empty()) props += L" ";
        props += ref.name;
        props += L"(";
        props += ref.className;
        props += L")";
        if (++said >= 48) { props += L" ..."; break; }
    }
    UE_LOGW("elevator: cargoLift_C's `%ls` did not resolve -- the class keeps: %ls", missing,
            props.empty() ? L"<no properties>" : props.c_str());
}

// The `move` timeline component of this lift: the BP timeline node's component property.
void* MoveTimeline(void* lift) {
    if (!lift || g_moveOff < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(lift) + g_moveOff);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = LiftClass();
    if (!cls) {
        CensusMissingClass();
        return false;
    }
    const int32_t keyOff = R::FindPropertyOffset(cls, L"Key");
    void* pressFn = R::FindFunction(cls, L"actionOptionIndex");
    if (keyOff < 0 || !pressFn) {
        static void* s_failedCls = nullptr;
        if (cls != s_failedCls) {
            s_failedCls = cls;
            UE_LOGE("elevator: cargoLift_C did not resolve by name (Key@%d actionOptionIndex=%p) -- "
                    "the lift lane stays off for this class", keyOff, pressFn);
            if (keyOff < 0) CensusClassProperties(L"Key");
        }
        return false;
    }

    // `dir` is the lane's state bit. Its type is whatever the blueprint declared -- a bool, a
    // byte or an int are all in reach -- so the resolve takes the offset AND the field class in
    // one walk, and the typed read below refuses a type it cannot read rather than guessing.
    int32_t dirOff = -1;
    std::wstring dirClass;
    if (!R::PropertyOffsetAndClass(cls, L"dir", dirOff, dirClass) || dirOff < 0) {
        static void* s_failedDirCls = nullptr;
        if (cls != s_failedDirCls) {
            s_failedDirCls = cls;
            UE_LOGE("elevator: cargoLift_C's `dir` did not resolve -- the lift lane stays off");
            CensusClassProperties(L"dir");
        }
        return false;
    }
    if (dirClass != L"BoolProperty" && dirClass != L"ByteProperty" &&
        dirClass != L"IntProperty" && dirClass != L"UIntProperty") {
        static bool s_saidType = false;
        if (!s_saidType) {
            s_saidType = true;
            UE_LOGE("elevator: cargoLift_C's `dir` is a %ls -- the lane reads bool, byte and int "
                    "only, so the lift lane stays off", dirClass.c_str());
        }
        return false;
    }

    g_keyOff    = keyOff;
    g_dirOff    = dirOff;
    g_dirClass  = dirClass;
    g_movingOff = R::FindPropertyOffset(cls, L"moving");
    g_moveOff   = R::FindPropertyOffset(cls, L"move");
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("elevator: resolved cargoLift_C=%p Key@0x%04X dir@0x%04X(%ls) moving@0x%04X move@0x%04X "
            "actionOptionIndex=%p",
            cls, keyOff, dirOff, dirClass.c_str(), g_movingOff, g_moveOff, pressFn);
    // One-time verb census for the log: the UFunctions the lift's class declares, so the next
    // remote-apply question ("which body animates it -- the press verb, a settime, the timeline?")
    // is answered from the log instead of a guess. ChildObjectsOf walks the class's children; a
    // UFunction names its own class 'Function'. Capped: a census is a read, never a flood.
    {
        int said = 0;
        std::wstring names;
        for (const R::ObjectRef& ref : R::ChildObjectsOf(cls)) {
            if (ref.className != L"Function") continue;
            if (!names.empty()) names += L" ";
            names += ref.name;
            if (++said >= 40) { names += L" ..."; break; }
        }
        if (!names.empty())
            UE_LOGI("elevator: cargoLift_C declares: %ls", names.c_str());
    }
    if (g_moveOff < 0)
        UE_LOGW("elevator: the `move` timeline property did not resolve -- an apply can set `dir` "
                "but cannot start the animation (the lift re-aims at its next press)");
    return true;
}

bool IsLift(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* liftCls = LiftClass();
    void* cls = R::ClassOf(obj);
    return liftCls && cls && R::IsDescendantOfAny(cls, &liftCls, 1);
}

std::wstring GetKeyString(void* lift) {
    if (!lift || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(lift) + g_keyOff);
    return R::ToString(key);
}

bool TryReadUp(void* lift, bool& up) {
    if (!lift || g_dirOff < 0) return false;
    const char* at = reinterpret_cast<const char*>(lift) + g_dirOff;
    if (g_dirClass == L"BoolProperty") {
        up = *reinterpret_cast<const bool*>(at) != 0;
    } else if (g_dirClass == L"ByteProperty") {
        up = *reinterpret_cast<const uint8_t*>(at) != 0;
    } else {  // IntProperty / UIntProperty, the resolve refuses the rest
        up = *reinterpret_cast<const int32_t*>(at) != 0;
    }
    return true;
}

bool ApplyUp(void* lift, bool up) {
    return ApplyUpAs(lift, up, nullptr);
}

bool ApplyUpAs(void* lift, bool up, void* player) {
    if (!lift) return false;
    // Idempotent: the channel applies only when cur != want, and this guard makes the double
    // delivery of an edge a no-op too.
    bool cur = false;
    const bool readable = TryReadUp(lift, cur);
    if (readable && cur == up) return true;
    // THE PREFERRED APPLY: the lift's own press verb, replayed the way the host replays a client's
    // intent (coop/interactables/elevator_sync::Execute). The verb's body does everything the
    // game's own press does -- the `dir` flip, the `moving` bookkeeping, the timeline and the
    // sound -- so the mirrored copy follows the Blueprint instead of a guess about which field
    // drives the move. The action maps the target end (the lift's two options are its two ends);
    // a body that refuses the value changes nothing, which the read below catches and falls back
    // from. The press gate's OnPre lets a fromOurCode dispatch through, so this replay is never
    // mistaken for a player's press and never sent back as an intent.
    if (Verb(lift, L"actionOptionIndex") && CallActionOptionIndex(lift, player, up ? 1 : 0)) {
        bool now = false;
        return !TryReadUp(lift, now) || now == up;
    }
    // The fallback (the pre-replay shape): write the `dir` field ourselves and start the `move`
    // timeline -- the BP's own driver. PlayFromStart (a native UTimelineComponent method) makes
    // the timeline's update function run, and it reads `dir` for its direction -- the same
    // delegation the garage's acivae() does for `Open`. The lane never drives the platform itself.
    if (!readable || g_dirOff < 0) return false;
    char* at = reinterpret_cast<char*>(lift) + g_dirOff;
    if (g_dirClass == L"BoolProperty") {
        *reinterpret_cast<bool*>(at) = up;
    } else if (g_dirClass == L"ByteProperty") {
        *reinterpret_cast<uint8_t*>(at) = up ? 1 : 0;
    } else {  // IntProperty / UIntProperty
        *reinterpret_cast<int32_t*>(at) = up ? 1 : 0;
    }
    // The animation: the `move` timeline, the BP's own driver. PlayFromStart (a native
    // UTimelineComponent method) makes the timeline's update function run, and it reads `dir`
    // for its direction -- the same delegation the garage's acivae() does for `Open`. The
    // lift's own Blueprint therefore animates the move; the lane never drives the platform
    // itself.
    void* const tl = MoveTimeline(lift);
    if (!tl) return false;
    void* const tlCls = R::ClassOf(tl);
    void* const playFn = tlCls ? R::FindDispatchFunctionCached(tlCls, P::name::TimelinePlayFromStartFn)
                               : nullptr;
    if (!playFn) return false;
    ParamFrame f(playFn);  // ue_wrap::ParamFrame (call.h), like garage.cpp's acivae call
    if (!f.valid()) return false;
    return Call(tl, f);
}

bool TryReadMoving(void* lift, bool& moving) {
    if (!lift || g_movingOff < 0) return false;
    moving = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(lift) + g_movingOff);
    return true;
}

bool CallActionOptionIndex(void* lift, void* player, uint8_t action) {
    void* const fn = Verb(lift, L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"player", player);   // a class without the param skips it
    f.Set<uint8_t>(L"action", action);
    return Call(lift, f);
}

}  // namespace ue_wrap::elevator
