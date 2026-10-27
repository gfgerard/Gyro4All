#pragma once
// On-screen notices: a small text box at the top of the game window, drawn by Windows (GDI), not by the
// game's renderer, so it works the same with DX11 and DX12 and hooks nothing. It never takes focus or
// clicks, follows the game window, hides while the game isn't in the foreground and fades after a while.
// Not visible in exclusive fullscreen (borderless/windowed only).
#include <string>

namespace gyro4all::notify {

// Shows text (one or more lines, '\n') for `seconds`, replacing any notice on screen. Thread-safe, cheap;
// the window and its thread are created on the first call.
void show(const std::wstring& text, int seconds = 10);

// Notices=0 in the profile: show() does nothing.
void set_enabled(bool enabled);

}  // namespace gyro4all::notify
