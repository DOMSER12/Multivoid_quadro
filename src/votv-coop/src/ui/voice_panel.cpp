// ui/voice_panel.cpp -- see ui/voice_panel.h.

#include "ui/voice_panel.h"

#include "coop/voice/voice_capture.h"
#include "coop/voice/voice_chat.h"
#include "coop/voice/voice_playback.h"
#include "coop/config/config.h"
#include "coop/player/roster.h"
#include "l10n/l10n.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace ui::voice_panel {
namespace {

namespace VC = coop::voice_chat;

// Atomic because the panel's open flag is written and read from three threads: Toggle runs on
// the game-window WndProc thread (the V key); Close is reached from there, from the render thread's
// re-fault guard, and from the game thread at session teardown; IsOpen and Render are read on the
// WndProc and render threads both.
std::atomic<bool> g_open{false};

// Device lists are enumerated once per panel-open (and on Rescan), never per frame:
// ma_context_init walks the audio stack. The PTT key label is cached the same way, since
// ReadIniValue is a file read.
bool g_devicesFresh = false;
std::vector<std::string> g_micDevices;
std::vector<std::string> g_outDevices;
char g_micCurrent[160] = {};
char g_outCurrent[160] = {};
char g_pttKey[32] = {};
char g_whisperKey[32] = {};
char g_muteKey[32] = {};

// The key-rebind capture, render-thread only (Render owns it; IsCapturingKey reads it on the
// same thread from the overlay's per-frame publish). `row` names the row being rebound -- 1 the
// push-to-talk key, 2 the whisper key, 3 the mute key -- and 0 is idle. `cleared` is the wait
// for the frame where NOTHING is down any more: the click that pressed the rebind button is
// itself a key, and without the clear it would be captured as the new bind.
struct KeyCapture {
    int  row = 0;
    bool cleared = false;
};
KeyCapture g_keyCapture;

// Each slider's handle value -- stored on every frame the handle is held, and on the frame a
// typed value (Ctrl+click, Enter) applies, when the item is already inactive -- and whether an
// edit is open (a drag, or a typed value not yet committed). Written by Render and read by the
// release branch and by CommitAbandonedDrag, all on the render thread; Close(), which runs on
// three threads, never touches them. The release branch commits the handle's own value from
// these, and an edit the panel's closing abandons is committed from these.
std::atomic<float> g_pendingThreshold{0.0f};
std::atomic<float> g_pendingGain{0.0f};
std::atomic<float> g_pendingVolume{0.0f};
std::atomic<bool>  g_draggingThreshold{false};
std::atomic<bool>  g_draggingGain{false};
std::atomic<bool>  g_draggingVolume{false};

// The one path from a slider to its row: the release branch and CommitAbandonedDrag. fmt is the
// slider's own format, so the row holds what the handle showed.
void CommitSlider(const coop::config_registry::FloatRow& row, float v, const char* fmt) {
    char text[16];
    std::snprintf(text, sizeof(text), fmt, v);
    coop::config::SetValue(row, text);
}

void RefreshDevices() {
    g_micDevices = coop::voice::Capture::EnumerateDevices();
    g_outDevices = coop::voice::Playback::EnumerateDevices();
    std::snprintf(g_micCurrent, sizeof(g_micCurrent), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_mic_device).c_str());
    std::snprintf(g_outCurrent, sizeof(g_outCurrent), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_output_device).c_str());
    std::snprintf(g_pttKey, sizeof(g_pttKey), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_ptt_key).c_str());
    std::snprintf(g_whisperKey, sizeof(g_whisperKey), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_whisper_key).c_str());
    std::snprintf(g_muteKey, sizeof(g_muteKey), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_mute_key).c_str());
    g_devicesFresh = true;
}

// A device combo: "(system default)" + the enumerated names. Sets the row on selection; the row's
// subscriber reopens the devices.
void DeviceCombo(const char* text, const char* id, const coop::config_registry::StringRow& row,
                 char* current, size_t currentCap, const std::vector<std::string>& names) {
    const char* shown = current[0] ? current : l10n::T("(system default)");
    if (ImGui::BeginCombo(l10n::Label(text, id), shown)) {
        if (ImGui::Selectable(l10n::Label(l10n::T("(system default)"), "system_default"), !current[0])) {
            current[0] = 0;
            coop::config::SetValue(row, "");
        }
        for (const std::string& n : names) {
            const bool sel = n == current;
            if (ImGui::Selectable(n.c_str(), sel)) {
                std::snprintf(current, currentCap, "%s", n.c_str());
                coop::config::SetValue(row, n.c_str());
            }
        }
        ImGui::EndCombo();
    }
}

// ---- the key rebinds -------------------------------------------------------------------------

// The key scan for the capture: 0 = nothing new, -1 = cancel (Esc), else the virtual-key code
// that is down. The middle and the two side mouse buttons (VK_MBUTTON..VK_XBUTTON2, 4..6) are
// scanned FIRST -- the common mouse PTT binds; LMB/RMB stay out (they are the panel's own
// clicks and the game's fire/aim), and so do the two reserved no-key codes. GetAsyncKeyState
// is process-global, but the panel is open only over our own window, which is the same
// foreground gate the voice polls sit behind.
int PollCapturedKey() {
    for (int vk = VK_MBUTTON; vk <= VK_XBUTTON2; ++vk)
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) return vk;
    for (int vk = 8; vk < 256; ++vk) {
        if (vk == 0x0A || vk == 0x0B) continue;  // reserved, no key
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) return vk == VK_ESCAPE ? -1 : vk;
    }
    return 0;
}

bool AnyKeyDown() {
    // From VK_LBUTTON on: the arming click is a mouse button too, so the clear wait must see
    // the buttons released before the capture takes the next key down.
    for (int vk = VK_LBUTTON; vk <= VK_XBUTTON2; ++vk)
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) return true;
    for (int vk = 8; vk < 256; ++vk) {
        if (vk == 0x0A || vk == 0x0B) continue;
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) return true;
    }
    return false;
}

// The row value a captured key becomes: a letter or a digit as itself, a mouse button as its
// "mouseN" token, any other key as its virtual-key number -- the spellings voice_chat's
// ParseKey reads.
void VkToText(int vk, char* out, size_t cap) {
    if (vk >= VK_MBUTTON && vk <= VK_XBUTTON2) {
        std::snprintf(out, cap, "mouse%d", vk);
        return;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        std::snprintf(out, cap, "%c", vk);
    else
        std::snprintf(out, cap, "%d", vk);
}

// The capture's per-frame step, run at the top of Render: the click's release wait, then the
// first key down is the bind -- Esc cancels -- and the row write lands through the config
// subscriber live. The label cache is written here too, so the next frame's button shows the
// new key without a file read.
void TickKeyCapture() {
    if (g_keyCapture.row == 0) return;
    if (!g_keyCapture.cleared) {
        if (!AnyKeyDown()) g_keyCapture.cleared = true;  // the click that armed us is up
        return;
    }
    const int vk = PollCapturedKey();
    if (vk == 0) return;                       // nothing down yet
    if (vk == -1) { g_keyCapture.row = 0; return; }  // Esc: the bind stays as it was
    char text[16];
    VkToText(vk, text, sizeof text);
    // Fully qualified on purpose: config_registry.h forbids using-declarations and aliases for
    // the rows namespace (CI-asserted, the reference census depends on it).
    switch (g_keyCapture.row) {
    case 1:
        coop::config::SetValue(coop::config_registry::rows::voice_ptt_key, text);
        std::snprintf(g_pttKey, sizeof(g_pttKey), "%s", text);
        break;
    case 2:
        coop::config::SetValue(coop::config_registry::rows::voice_whisper_key, text);
        std::snprintf(g_whisperKey, sizeof(g_whisperKey), "%s", text);
        break;
    case 3:
        coop::config::SetValue(coop::config_registry::rows::voice_mute_key, text);
        std::snprintf(g_muteKey, sizeof(g_muteKey), "%s", text);
        break;
    default: break;
    }
    g_keyCapture.row = 0;
}

// One key row of the Keys section: the label, the current key, the Change button (which shows
// "Press a key..." while its capture is armed) and the Clear button, which writes the empty
// value -- for the whisper and mute keys that is "disabled", for the push-to-talk key the
// engine default 'G'.
void KeyRebindRow(const char* label, const char* id, int rowId, const char* current,
                  const char* emptyMeaning) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ui::scale::S(150.0f));
    const bool armed = g_keyCapture.row == rowId;
    char changeId[64];
    std::snprintf(changeId, sizeof(changeId), "%s##coop_key%d", armed ? "*" : (current[0] ? current : "-"), rowId);
    const char* changeLabel = armed ? l10n::T("Press a key... (Esc to cancel)") : changeId;
    if (ImGui::SmallButton(l10n::Label(changeLabel, id))) {
        g_keyCapture.row = rowId;
        g_keyCapture.cleared = false;
    }
    ImGui::SameLine();
    char clearId[64];
    std::snprintf(clearId, sizeof(clearId), "%s##coop_keyclear%d", l10n::T("Clear"), rowId);
    if (ImGui::SmallButton(l10n::Label(clearId, id))) {
        // Fully qualified: the rows namespace may not be aliased (config_registry.h, CI-asserted).
        switch (rowId) {
        case 1: coop::config::SetValue(coop::config_registry::rows::voice_ptt_key, ""); std::snprintf(g_pttKey, sizeof(g_pttKey), "%s", ""); break;
        case 2: coop::config::SetValue(coop::config_registry::rows::voice_whisper_key, ""); std::snprintf(g_whisperKey, sizeof(g_whisperKey), "%s", ""); break;
        case 3: coop::config::SetValue(coop::config_registry::rows::voice_mute_key, ""); std::snprintf(g_muteKey, sizeof(g_muteKey), "%s", ""); break;
        default: break;
        }
        if (g_keyCapture.row == rowId) g_keyCapture.row = 0;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", current[0] ? current : emptyMeaning);
}

}  // namespace

void Toggle() {
    g_open = !g_open;
    if (g_open) g_devicesFresh = false;  // re-enumerate on each open
}

void Close() { g_open = false; }

bool IsOpen() { return g_open; }

// Which key row the overlay's text-capture gate must also cover: while a rebind is armed, a
// keystroke belongs to the rebind and must not also fire the game's or the voice's own polls.
// Declared in voice_panel.h and called from imgui_overlay, so this definition stays OUT of the
// anonymous namespace -- inside it the symbol would have internal linkage and the link would
// fail with LNK2019 (exactly that happened once).
bool IsCapturingKey() { return g_keyCapture.row != 0; }

void CommitAbandonedDrag() {
    if (g_open.load(std::memory_order_relaxed)) return;
    if (g_draggingThreshold.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_threshold_db, g_pendingThreshold.load(),
                     "%.0f");
    if (g_draggingGain.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_mic_gain_db, g_pendingGain.load(), "%.0f");
    if (g_draggingVolume.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_volume, g_pendingVolume.load(), "%.2f");
}

void Render() {
    if (!g_open.load(std::memory_order_relaxed)) return;
    if (!g_devicesFresh) RefreshDevices();
    TickKeyCapture();  // the armed rebind reads its key while the panel draws

    VC::UiSnapshot s;
    VC::GetUiSnapshot(s);

    const ImGuiIO& io = ImGui::GetIO();
    using ui::scale::S;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(380.0f), 0.0f), ImGuiCond_Always);
    // Begin's close-X needs a plain bool*, so the atomic is bridged through a local and written
    // back by a destructor rather than at each exit. Written back by hand, it has to be repeated on
    // every path out of this function, and the one path that forgets it is invisible: with voice
    // enabled -- the normal case, and the only one a player ever sees -- the X cleared the local,
    // nothing consumed it, g_open stayed true, and the next frame reopened the window with a fresh
    // open=true. V kept working throughout, because V drives g_open directly. A destructor cannot
    // be forgotten by a branch someone adds later.
    bool open = true;
    struct CloseOnX {
        const bool& open;
        ~CloseOnX() {
            if (!open) g_open.store(false, std::memory_order_relaxed);
        }
    } closeOnX{open};
    if (ImGui::Begin(l10n::Label(l10n::T("Voice chat"), "coop_voice_panel"), &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.enabled) {
            char off[512];
            l10n::Fmt(off, sizeof(off),
                      l10n::T("Voice chat is turned off (%s=0 in multivoid.ini). "
                              "Set it to 1 and restart the game to use voice."),
                      "voice.enabled");
            ImGui::TextWrapped("%s", off);
            ImGui::End();
            return;  // closeOnX writes the X back on the way out
        }
        if (!s.captureOk)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                               l10n::T("No microphone -- voice is receive-only."));
        if (!s.playbackOk)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                               l10n::T("No output device -- you can't hear voice."));

        // Mic meter row: live level bar + the local state icon.
        {
            const float frac = std::clamp((s.micLevelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(l10n::T("Mic"));
            ImGui::SameLine(S(60.0f));
            const ImVec4 barCol = s.transmitting ? ImVec4(0.30f, 0.80f, 0.40f, 1.0f)
                                                 : ImVec4(0.45f, 0.50f, 0.56f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            char db[24];
            std::snprintf(db, sizeof(db), "%.0f dB", s.micLevelDb);
            ImGui::ProgressBar(frac, ImVec2(-S(34.0f), 0.0f), db);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ui::voice_icons::Draw(ImGui::GetWindowDrawList(),
                                  ImVec2(p.x + S(9.0f), p.y + ImGui::GetTextLineHeight() * 0.6f),
                                  S(16.0f), static_cast<VC::VoiceIcon>(s.localIcon), 1.0f);
            ImGui::Dummy(ImVec2(S(20.0f), ImGui::GetTextLineHeight()));
        }

        bool muted = s.muted != 0;
        if (ImGui::Checkbox(l10n::Label(l10n::T("Mute my microphone"), "mute_my_microphone"), &muted))
            VC::SetMuted(muted);
        ImGui::SameLine();
        ImGui::TextDisabled("(key: %s)", g_muteKey[0] ? g_muteKey : "off");

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Activation"), "activation"));
        int mode = s.activationMode ? 1 : 0;
        char pttLabel[256];
        l10n::Fmt(pttLabel, sizeof(pttLabel), l10n::T("Push-to-talk (key: %s)"), g_pttKey);
        if (ImGui::RadioButton(l10n::Label(pttLabel, "push_to_talk"), &mode, 0))
            coop::config::SetValue(::coop::config_registry::rows::voice_mode, "ptt");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            char tip[512];
            l10n::Fmt(tip, sizeof(tip),
                      l10n::T("Hold the key to talk. Change the key in the Keys section below."));
            ImGui::SetTooltip("%s", tip);
        }
        if (ImGui::RadioButton(l10n::Label(l10n::T("Voice activation"), "voice_activation"), &mode, 1))
            coop::config::SetValue(::coop::config_registry::rows::voice_mode, "activation");
        // The three sliders: the drag previews live through the voice_chat setters, and the
        // release commits the row (CommitSlider), whose subscriber is the apply. The pending atomic
        // holds the handle's value from every active frame and from the frame a typed value is
        // applied (SliderFloat returns true then, the item already inactive), never the frame's
        // snapshot float. The ranges are the panel's own, narrower than the rows'; AlwaysClamp
        // holds a typed value (Ctrl+click) to them.
        // MTA keeps the in-memory setting as the live value, saved on OK and reverted on Cancel
        // (CSettings.cpp:5568-5575, :3888-3889, :3904-3916).
        // Source's cvarslider has no live preview and writes the cvar at drag end or on the
        // dialog's OK, discarding on Close (cvarslider.cpp:310-333, tf_controls.cpp:746-755).
        // Ours previews live because SetValue writes the ini, logs `config: SET` and notifies on
        // every set (config_runtime.cpp), so the row cannot be MTA's per-frame in-memory store;
        // the row is set once, on release, and a drag the panel's closing abandons is committed
        // rather than reverted because this panel has no OK/Cancel and every other control
        // applies at once.
        if (mode == 1) {
            float thr = s.thresholdDb;
            if (ImGui::SliderFloat(l10n::Label(l10n::T("Threshold"), "threshold"), &thr, -100.0f, 0.0f,
                                   "%.0f dB", ImGuiSliderFlags_AlwaysClamp)) {
                VC::SetThresholdDb(thr);
                g_pendingThreshold.store(thr);
            }
            if (ImGui::IsItemActive()) {
                g_pendingThreshold.store(thr);
                g_draggingThreshold.store(true);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_draggingThreshold.exchange(false);
                CommitSlider(::coop::config_registry::rows::voice_threshold_db,
                             g_pendingThreshold.load(), "%.0f");
            } else if (ImGui::IsItemDeactivated()) {
                g_draggingThreshold.exchange(false);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", l10n::T("Speech louder than this transmits. Watch the mic\n"
                                                "meter: set the slider just above your room's noise."));
            // 0 dB is full scale, so nothing would ever transmit: warn rather than let the panel
            // sit there silently never activating.
            if (thr > -5.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                                   l10n::T("Threshold near 0 dB -- the mic will almost never "
                                           "trigger. Try -50 dB."));
        }

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Keys"), "keys"));
        KeyRebindRow(l10n::T("Push-to-talk key"), "coop_key_ptt", 1, g_pttKey,
                     l10n::T("default G"));
        KeyRebindRow(l10n::T("Whisper key"), "coop_key_whisper", 2, g_whisperKey,
                     l10n::T("disabled"));
        KeyRebindRow(l10n::T("Mic on/off key"), "coop_key_mute", 3, g_muteKey,
                     l10n::T("disabled"));
        ImGui::TextDisabled("%s", l10n::T("A key is a letter, a digit, a virtual-key number, or a\n"
                                          "mouse button (mouse4 = middle, mouse5/mouse6 = side)."));

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Levels"), "levels"));
        float gain = s.gainDb;
        if (ImGui::SliderFloat(l10n::Label(l10n::T("Mic gain"), "mic_gain"), &gain, -40.0f, 24.0f,
                               "%+.0f dB", ImGuiSliderFlags_AlwaysClamp)) {
            VC::SetGainDb(gain);
            g_pendingGain.store(gain);
        }
        if (ImGui::IsItemActive()) {
            g_pendingGain.store(gain);
            g_draggingGain.store(true);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_draggingGain.exchange(false);
            CommitSlider(::coop::config_registry::rows::voice_mic_gain_db, g_pendingGain.load(),
                         "%.0f");
        } else if (ImGui::IsItemDeactivated()) {
            g_draggingGain.exchange(false);
        }
        float vol = s.masterVolume;
        if (ImGui::SliderFloat(l10n::Label(l10n::T("Voice volume"), "voice_volume"), &vol, 0.0f, 3.0f,
                               "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
            VC::SetMasterVolume(vol);
            g_pendingVolume.store(vol);
        }
        if (ImGui::IsItemActive()) {
            g_pendingVolume.store(vol);
            g_draggingVolume.store(true);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_draggingVolume.exchange(false);
            CommitSlider(::coop::config_registry::rows::voice_volume, g_pendingVolume.load(), "%.2f");
        } else if (ImGui::IsItemDeactivated()) {
            g_draggingVolume.exchange(false);
        }
        // The range is the host's in a session; read-only here. The two roster facts are read
        // on the render thread each frame the panel draws (the roster refreshes them on a
        // throttle, so the label may lag a session's start or end by a fraction of a second).
        {
            coop::roster::Snapshot rs;
            coop::roster::GetSnapshot(rs);
            // The range as a value, then the sentence that carries it: one msgid per whose
            // range it is, so a translation orders the whole line.
            char rangeBuf[256];
            const char* range = rangeBuf;
            if (s.distanceCm > 0.0f)
                l10n::Fmt(rangeBuf, sizeof(rangeBuf), l10n::T("%.0f cm"), s.distanceCm);
            else
                range = l10n::T("unlimited");
            char line[256];
            if (!rs.inSession)
                l10n::Fmt(line, sizeof(line), l10n::T("Voice range: %s"), range);
            else if (coop::roster::LocalIsHost())
                l10n::Fmt(line, sizeof(line),
                          l10n::T("Voice range: %s (your server's; multivoid.ini until the settings page)"),
                          range);
            else
                l10n::Fmt(line, sizeof(line), l10n::T("Voice range: %s (set by the host)"), range);
            ImGui::TextDisabled("%s", line);
        }

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Devices"), "devices"));
        DeviceCombo(l10n::T("Microphone"), "microphone", coop::config_registry::rows::voice_mic_device,
                    g_micCurrent, sizeof(g_micCurrent), g_micDevices);
        DeviceCombo(l10n::T("Output"), "output", coop::config_registry::rows::voice_output_device,
                    g_outCurrent, sizeof(g_outCurrent), g_outDevices);
        if (ImGui::SmallButton(l10n::Label(l10n::T("Rescan devices"), "rescan_devices")))
            RefreshDevices();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", l10n::T("Device changes apply immediately (the voice engine\n"
                                            "reopens on the game's next tick)."));
    }
    ImGui::End();
}

}  // namespace ui::voice_panel
