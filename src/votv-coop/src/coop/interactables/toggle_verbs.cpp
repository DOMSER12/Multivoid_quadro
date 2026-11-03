// coop/interactables/toggle_verbs.cpp -- see coop/interactables/toggle_verbs.h.

#include "coop/interactables/toggle_verbs.h"

#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load stays local

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/door_box.h"
#include "ue_wrap/devices/elevator.h"
#include "ue_wrap/devices/garage.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/devices/security_camera.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace coop::toggle_verbs {
namespace {

namespace sg = ue_wrap::script_gate;

// A verb's name matches every class's function of that name, so each row's callback first tests
// the row's class.
UE_FACT_FN(kRowUse, "toggle_verbs", nullptr, L"use", AnyClass);
UE_FACT_FN(kRowRunTrigger, "toggle_verbs", nullptr, L"runTrigger", AnyClass);
UE_FACT_FN(kRowActionOptionIndex, "toggle_verbs", nullptr, L"actionOptionIndex", AnyClass);
UE_FACT_FN(kRowVisual, "toggle_verbs", nullptr, L"visual", AnyClass);
UE_FACT_FN(kRowOpen, "toggle_verbs", nullptr, L"open", AnyClass);
UE_FACT_FN(kRowClose, "toggle_verbs", nullptr, L"close", AnyClass);
UE_FACT_FN(kRowFix, "toggle_verbs", nullptr, L"fix", AnyClass);
// A security camera's writers all end in one of these (the player's use ends in updState; the wisp's
// and the gray events' writes in upd or updState), except the cursed camera's night roll, which calls
// neither. Declared on the camera class, so another class's function of the name is not asked.
UE_FACT_FN(kRowCameraActionOptionIndex, "toggle_verbs", L"prop_camera_bad_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowCameraUpd, "toggle_verbs", L"prop_camera_bad_C", L"upd", Declared);
UE_FACT_FN(kRowCameraUpdState, "toggle_verbs", L"prop_camera_bad_C", L"updState", Declared);
UE_FACT_FN(kRowCursedReceiveTick, "toggle_verbs", L"prop_camera_cursed_C", L"ReceiveTick", Declared);
// The base cargo lift's press: its buttons (its own components) all dispatch actionOptionIndex on
// the lift actor, Declared on the lift's class. The lift channel is HostAuth, so the edge sends
// from the host alone; a client's press is refused upstream (coop/interactables/elevator_sync).
UE_FACT_FN(kRowLiftAction, "toggle_verbs", L"cargoLift_C", L"actionOptionIndex", Declared);

struct Row {
    const ue_wrap::facts::Function* verb;
    int tag;
    const char* what;
    bool (*IsInstance)(void* obj);
    void (*Edge)(void* actor);
};
const Row kRows[] = {
    { &kRowUse, 0x4C535755 /*'LSWU'*/, "a light switch's use", &ue_wrap::lightswitch::IsLightSwitch,
      &coop::interactable_sync::OnLightSwitchVerb },
    { &kRowRunTrigger, 0x47525254 /*'GRRT'*/, "a garage's runTrigger", &ue_wrap::garage::IsGarage,
      &coop::interactable_sync::OnGarageVerb },
    { &kRowActionOptionIndex, 0x41504149 /*'APAI'*/, "an appliance's action", &ue_wrap::appliance::IsAppliance,
      &coop::interactable_sync::OnApplianceVerb },
    { &kRowVisual, 0x41505653 /*'APVS'*/, "a server box's visual", &ue_wrap::appliance::IsAppliance,
      &coop::interactable_sync::OnApplianceVerb },
    { &kRowOpen, 0x44424F50 /*'DBOP'*/, "a locker's open", &ue_wrap::door_box::IsLocker,
      &coop::interactable_sync::OnDoorBoxVerb },
    { &kRowActionOptionIndex, 0x44424149 /*'DBAI'*/, "a drone console's action", &ue_wrap::door_box::IsDroneConsole,
      &coop::interactable_sync::OnDoorBoxVerb },
    { &kRowOpen, 0x53574F50 /*'SWOP'*/, "a lid's open", &ue_wrap::swinger::IsSwinger,
      &coop::interactable_sync::OnContainerVerb },
    { &kRowClose, 0x5357434C /*'SWCL'*/, "a lid's close", &ue_wrap::swinger::IsSwinger,
      &coop::interactable_sync::OnContainerVerb },
    { &kRowFix, 0x4F564658 /*'OVFX'*/, "an oven's repair", &ue_wrap::appliance::IsOven,
      &coop::interactable_sync::OnOvenVerb },
    // The camera lanes are the host's (HostAuth): the host's edge sends, a client's does nothing here.
    { &kRowCameraActionOptionIndex, 0x43414149 /*'CAAI'*/, "a camera's action", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCameraUpd, 0x43415550 /*'CAUP'*/, "a camera's upd", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCameraUpdState, 0x43415553 /*'CAUS'*/, "a camera's updState", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCursedReceiveTick, 0x43435254 /*'CCRT'*/, "the cursed camera's tick", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowLiftAction, 0x454C5653 /*'ELVS'*/, "the cargo lift's press", &ue_wrap::elevator::IsLift,
      &coop::interactable_sync::OnElevatorVerb },
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRowCount] = {};
bool g_settled = false;
uint64_t g_edges[kRowCount] = {};

const Row* RowOf(int tag, int* index) {
    for (int i = 0; i < kRowCount; ++i)
        if (kRows[i].tag == tag) { *index = i; return &kRows[i]; }
    return nullptr;
}

// Every peer, after the body.
void OnVerbPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object) return;
    int i = -1;
    const Row* row = RowOf(call.tag, &i);
    if (!row || !row->IsInstance(call.object)) return;
    // A client's load of a world is the host's save; its devices' own calls there are not its player's.
    if (s->role() != coop::net::Role::Host && !coop::net_pump::IsInAnnouncedWorld(call.object)) return;
    ++g_edges[i];
    row->Edge(call.object);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kRows[i].verb, kRows[i].tag, nullptr, &OnVerbPost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[TOGGLE-VERB] the script-body gate refused the watch on %s -- for the rest of this process that "
                "lane cannot see it", kRows[i].what);
    }
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    std::wstring pendingNames;
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRows[i].verb, kRows[i].tag)) ++live;
        else if (g_reg[i] == Reg::Registered) {
            // Registered, its names not resolved yet: the class may still be loading, or the build
            // spells it otherwise. Named here, so a wrong class name is a log read, not silence.
            if (!pendingNames.empty()) pendingNames += L", ";
            pendingNames += kRows[i].verb->Class();
            pendingNames += L"::";
            pendingNames += kRows[i].verb->Name();
        }
    }
    if (live == kRowCount) {
        g_settled = true;
        UE_LOGI("[TOGGLE-VERB] the toggle verb gates are live: %d of %d", live, kRowCount);
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[TOGGLE-VERB] %d of %d toggle verb gates are dead (refused, or resolved into a full table)",
                kRowCount - live, kRowCount);
    } else {
        // Still waiting on names: say which, once in 45 s, so a class the build never loads by
        // this name is visible instead of holding the settle forever in silence.
        static std::chrono::steady_clock::time_point s_next{};
        const auto now = std::chrono::steady_clock::now();
        if (!pendingNames.empty() && now >= s_next) {
            s_next = now + std::chrono::seconds(45);
            UE_LOGW("[TOGGLE-VERB] %d of %d watch(es) not live yet (class not loaded yet, refused, "
                    "or the name spells otherwise): %ls",
                    kRowCount - live, kRowCount, pendingNames.c_str());
        }
    }
}

void OnDisconnect() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_edges[i])
            UE_LOGI("[TOGGLE-VERB] session end -- %s ran on a device of its row %llu time(s), the "
                    "lane's own applies among them", kRows[i].what, static_cast<unsigned long long>(g_edges[i]));
        g_edges[i] = 0;
    }
}

}  // namespace coop::toggle_verbs
