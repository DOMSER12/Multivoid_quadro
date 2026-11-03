// ui/voice_panel.h -- the voice-chat settings window. Opened with the V key
// (game-focus edge in the overlay WndProc). V opens voice settings and is
// independent of the tilde scoreboard. Rendered by imgui_overlay as its own
// interactive surface (it joins the input-capture set); the open state latches
// until V / the window's X closes it.
//
// Rows are applied by their subscribers (voice_chat::SubscribeRows): a device/mode change reopens
// the devices on the next game tick, never here. The panel calls voice_chat directly only for the
// mute toggle and the sliders' live drag preview (its atomic setters); each slider commits its row
// on release. Render thread only.

#pragma once

namespace ui::voice_panel {

void Toggle();
void Close();
bool IsOpen();
void Render();

// Render thread, while the panel draws: true while a key-rebind capture is armed. The overlay's
// per-frame input-capture publish adds it to the text-capture gate, so a keystroke a rebind is
// waiting for never also fires the game's or the voice's own key polls.
bool IsCapturingKey();

// Render thread, once per drawn frame: a slider that was being dragged when the panel closed
// commits its last previewed value to its row.
void CommitAbandonedDrag();

}  // namespace ui::voice_panel
