#pragma once
// The controller GyroLib reads through SDL (DualSense, Switch Pro, DirectInput/HID pads), shown to the game as an
// Xbox (XInput) controller: the game's own imports of XInputGetState/SetState/GetCapabilities/Enable are redirected
// (import-table hooks on game modules only, so SDL's own XInput use never sees it). For games that only support
// XInput and run without Steam Input (e.g. Epic). Off when the game already reads the pad itself (see xinput.cpp).
#include <cstdint>

struct gl_context;
struct gl_sdl;
struct gl_output;
struct SDL_Gamepad;

namespace gyro4all::xinput {

enum Mode { Off = 0, Auto = 1, Always = 2 };

struct Settings {
    int mode = Auto;
    bool nintendo_labels = false;  // swap face buttons A<->B and X<->Y (Nintendo A = Xbox A) instead of by position
    bool touchpad_back = true;     // touchpad click also presses View (Back), like the Create/Share button
    bool trace = false;            // XInputTrace: log every module that calls XInput (inline hooks on the system DLLs)
    bool export_hooks = false;     // Engine::managed_input: also hook the XInput DLLs' exports (no import table to patch)
    bool refuse_wgi = false;       // Engine::wgi_falls_back_to_xinput: with XInput=2, Windows.Gaming.Input classes refused
    const wchar_t* window_class = nullptr;  // the game's windows (Engine::window_class), nullptr = every top-level one
    void (*controllers_changed)() = nullptr;  // Engine::controllers_changed, after the WM_DEVICECHANGE (game thread)
};

// The game links Sony's pad library (auto mode's check: libScePad or hid.dll among its imports, or libScePad loaded).
bool sony_library();

// That sign is only hid.dll among the imports, no libScePad seen: Unreal's RawInput plugin looks the same, and it
// enumerates controllers well before it reads one (S.T.A.L.K.E.R. 2).
bool sony_hid_import_only();

// Whether the controller is shown to the game as XInput at all (XInput= not 0, and the hooks could be installed).
bool translates();

// The game itself lists the controller vendor:product as an XInput device (an engine module made the game's own
// device check say so, e.g. Frostbite's IsXInputDevice): auto mode then shows that controller as XInput even though
// the game also opened it through DirectInput or HID, since the game reads it through XInput only (and without the
// grace period). Until the frame decides about it, XInputGetCapabilities reads a connected gamepad in the slot it will
// get (games that pick the slot once, right after the check: 007 First Light). wait: also waits up to 1 s for the
// frame to show it (Battlefield 4); only from a thread that isn't the one running the frame.
void listed_as_xinput(uint16_t vendor, uint16_t product, bool wait = true);

// The controller shown as XInput has its right stick pushed past a quarter (the player's own stick, as last given to
// the game, before the stick fallback's deflection). Any thread; false when no controller is shown.
bool right_stick_pushed();

// The buttons of the controller shown as XInput, as the game reads them (XINPUT_GAMEPAD_*: idle while GyroLib's panel
// has the controller). Any thread; false when no controller is shown.
bool shown_buttons(uint16_t& buttons);

// Game thread: posts WM_DEVICECHANGE (DBT_DEVNODES_CHANGED) to the game's windows (Settings::window_class), as Windows
// does when a controller is plugged in or out: the engine (and Sony's pad libraries linked into it) then look for
// controllers again.
void announce_device_change();

// DllMain, after hooks::init: registers the hooks the mode needs (none for Off).
void install(const Settings& settings);

// Game thread: the SDL gamepad of GyroLib's selected controller (GyroLib owns it), or null. Cached; any mode.
SDL_Gamepad* selected_gamepad(gl_context* ctx, gl_sdl* reader, const gl_output& out);

// Game thread, once per frame before frame(): while GyroLib's panel has the controller, the game reads an idle
// controller in every XInput slot (ours and real ones).
void set_neutral(bool neutral);

// Game thread, once per frame before frame(): long-press blocking's button mask (longpress.cpp, SDL button bits) for
// the controller shown as XInput and, when the selected pad is itself a real XInput controller the game polls (Xbox
// pad, Steam Input), for real XInput slots. Returns true in that second case (the filter works there).
bool set_buttons(SDL_Gamepad* pad, uint32_t hide, uint32_t tap);

// Stick fallback (stick_fallback.hpp; game thread): right-stick deflection added to the controller shown to the game,
// and to a real XInput controller while it is the selected one (x right, y up, -1..1; 0, 0 = none).
void set_stick(float x, float y);

// Game thread, once per frame after the SDL reader was polled and gl_update ran (out is that update's output).
// Updates the state the game reads and applies its rumble. Returns true while the pad is shown to the game.
// suppress_right: the stick camera (stick_camera.hpp) turns the camera, so the game reads a centred right stick (as
// with GyroLib's suppress_native_right_stick; the stick click still reaches it).
bool frame(gl_context* ctx, gl_sdl* reader, const gl_output& out, bool suppress_right = false);

// WriteWatch's xi and ri addresses (writewatch.cpp), any thread: the buffer the game last passed to XInputGetState(Ex),
// and to GetRawInputBuffer or GetRawInputData (RID_INPUT); 0 = none yet. on_stack: it was on the calling thread's stack.
uintptr_t last_state_buffer(bool& on_stack);
uintptr_t last_raw_input_buffer(bool& on_stack);

}  // namespace gyro4all::xinput
