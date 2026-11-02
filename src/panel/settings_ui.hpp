#pragma once
// gyro4all.dll: Gyro4All's own settings window (settings from Gyro4All.ini, which GyroLib's panel doesn't know),
// drawn next to GyroLib's panel whenever that is open. Folded it is a small button at the top of the screen; clicking
// it expands the full window. Values go through the g4a_settings callbacks winmm.dll supplied at create (it owns the
// INI files); keys are utf-8, the effective value (profile wins) is shown, edits are written to Gyro4All.ini.
#include "gyro4all_panel.h"

namespace g4a::settings_ui {

// Under g_lock, inside the ImGui frame, while the panel is open. dpi_scale: the window's UI scales with it and the
// viewport resolution, as GyroLib's panel does. No-op without settings callbacks.
void draw(float dpi_scale);

// create: the settings access (not copied; owned by winmm.dll, which outlives this DLL in practice: it loaded us)
// and GyroLib's context (per-controller button naming and the selected device).
void set_settings(const g4a_settings* settings, struct gl_context* ctx);

// Fold/unfold the settings window (the L3+R3 chord, polled by winmm.dll, and the window's × button). Unfolding moves
// ImGui's gamepad-nav focus into the window; folding returns it to GyroLib's panel window when that window is in
// this DLL's ImGui context (non-DX12 games; in DX12 games it lives in gyrolib.dll's own context, unaffected).
void toggle();

}  // namespace g4a::settings_ui
