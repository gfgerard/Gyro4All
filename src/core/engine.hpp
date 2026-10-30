#pragma once
// The interface between the engine-neutral core and an engine module (engines/<engine>/). The core owns the loader,
// hooks, GyroLib, the controller (XInput, native pad), the overlay and notices; an engine module finds the game's
// scene and camera and turns the view. Adding an engine: a new engines/<engine>/ module implementing Engine, listed
// in src/engine_registry.cpp. The core never includes anything from engines/.
//
// Threads: detect/module_prefix/window_class/default_settings/configure/frame_source run in DllMain; everything else on
// the game thread, once per frame (read_frame, recenter_support, then apply and recenter, then after_frame), where all
// GyroLib calls happen too. The game thread is the main thread (the core's PeekMessageW hook) unless the engine
// runs the frame itself (frame_source).
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gyro4all {

// A gameplay view, registered with GyroLib as a settings context (its own sensitivity, curves...).
struct ViewInfo {
    uint32_t id = 1;     // 1 = the default view
    std::string label;
    uint32_t parent = 0; // settings inheritance (0 = none)
};

// What the core needs from the engine each frame.
struct EngineFrame {
    bool in_game = false;  // a local player exists (else: loading, front end)
    bool paused = false;   // the game is paused
    bool cursor = false;   // the mouse cursor is shown (menus, inventories)
    uint32_t view_id = 0;  // the active gameplay view (0 = none: menus, cutscenes, spectator)
    // The view view_id stands in for, when the engine switches between several rotations of one camera (Unreal's
    // Match=Camera views): the default view (1) for its alternatives, so the sensitivity profiles apply to them too.
    // 0 = view_id itself.
    uint32_t base_view_id = 0;
    std::string target;    // what the camera looks at (e.g. its class), for the log
    // The view's field of view as rendered this frame (horizontal degrees; 0 = unknown): zoom compensation (zoom.hpp).
    double fov = 0;
    // The engine has no hook for this game's camera at all (no camera model fits it): the gyro then always goes to the
    // game as right-stick input (the stick fallback, blind: no angles to learn from, no camera writes). Report the
    // default view (view_id 1, in_game) with it, so the gyro's gate (focus, pause, cursor) and GyroLib's view still
    // apply.
    bool no_camera = false;
};

// View ids of the core's sensitivity profiles (ADS, ALT_ADS, OTHER): an engine must not give them to its own views.
inline constexpr uint32_t kSensitivityViewIds[] = {1001, 1002, 1003};

class Engine {
public:
    virtual ~Engine() = default;
    virtual const char* name() const = 0;  // "Unreal"; also the value of Engine= in Gyro4All.ini
    // DllMain: the game was made with this engine (cheap checks on the exe and its folder only).
    virtual bool detect(const std::wstring& exe_path) const = 0;
    // DllMain: file-name prefix of the game's own DLLs (modular builds), hooked like the exe ("" = the exe only).
    virtual std::wstring module_prefix(const std::wstring& /*exe_stem*/) const { return {}; }
    // DllMain: the same game's exe stem in other stores' builds (e.g. Unreal's -Win64- / -WinGDK-), whose profile is
    // used when profiles\<exe stem>.ini doesn't exist.
    virtual std::vector<std::wstring> profile_aliases(const std::wstring& /*exe_stem*/) const { return {}; }
    // DllMain: the engine's games are protected exes whose imports the protector resolves itself, in tables the
    // import scan doesn't recognize (e.g. Frostbite with Denuvo/EA's OOA): hook system DLL exports inline instead.
    virtual bool inline_hooks() const { return false; }
    // Window class of the game's windows, which get WM_DEVICECHANGE when the virtual Xbox controller comes or goes
    // (nullptr = every top-level window of the game thread).
    virtual const wchar_t* window_class() const { return nullptr; }
    // Game thread, right after that WM_DEVICECHANGE: for games that look for controllers only at other times (e.g. when
    // their viewport is created), make the game look again now.
    virtual void controllers_changed() {}
    // The engine's own commented [Gyro4All] settings, written first into a new Gyro4All.ini ("\r\n" line ends).
    virtual const char* default_settings() const { return ""; }
    // DllMain, after settings::configure and settings::ensure_file: read the profile.
    virtual void configure() = 0;
    // The gameplay views (at least the default one, id 1), registered with GyroLib on the first frame.
    virtual std::vector<ViewInfo> views() const = 0;
    // DllMain: true when the engine calls frame() itself, once per frame and always on the same thread, instead of the
    // core's PeekMessageW hook on the main thread. For engines whose main thread only waits for window messages (RE
    // Engine: GetMessageW, frame on its game update thread) or stops pumping them (Frostbite: frame on the module's own
    // thread, woken by the game's Present). Everything the "game thread" notes above say then holds for that thread.
    using FrameFn = void (*)();
    virtual bool frame_source(FrameFn /*frame*/) { return false; }
    // DllMain: the engine's DllMain thread may never pump window messages (DOOM 2016): until the first gyro frame, the
    // thread that gets the visible game window's messages becomes the game thread instead. Off by default (a splash
    // screen's thread could pump first).
    virtual bool adopt_pump_thread() const { return false; }
    // DllMain: the game thread is the one that pumps the messages of a top-level window of this class, whichever thread
    // that is and however late it starts; no gyro frame runs before it pumps (Space Marine 2: the DllMain thread pumps
    // only the splash screen's window, then stops, and another thread pumps the game window). nullptr = the DllMain thread
    // (or adopt_pump_thread's choice).
    virtual const wchar_t* pump_window_class() const { return nullptr; }
    // DllMain: this exe of the game's folder is a launcher (DOOM: The Dark Ages' idTechLauncher.exe, an Electron app that
    // pumps messages and starts the game later): Gyro4All stays a plain system-DLL proxy there and leaves the session to
    // the game it starts.
    virtual bool is_launcher(const std::wstring& /*exe_path*/) const { return false; }

    // Game thread, once per frame: read the scene and pick the view.
    virtual EngineFrame read_frame() = 0;
    // Game thread, after read_frame: views() changed since the last call (e.g. the profile file was edited, or the
    // camera probe added a view). The core then registers the new views with GyroLib and withdraws removed ones.
    virtual bool views_changed() { return false; }
    // The gyro must not turn the camera now, although the view allows it (e.g. while a camera probe captures).
    virtual bool hold_gyro() const { return false; }
    // Before apply: trims the turn (degrees, the gyro's senses) to what the active view can still take, e.g. up to the
    // game's pitch limit, so a turn stopped there isn't judged as a write the game undid (the stick fallback).
    virtual void limit_turn(double& /*pitch*/, double& /*yaw*/) {}
    // Turn the active view by GyroLib's output (degrees, already integrated). True when it was written.
    virtual bool apply(double pitch, double yaw) = 0;
    // Camera recenter (GyroLib's camera.recenter_button) in the active gameplay view: Yes = it can level this view's
    // pitch; NotYet = not now (e.g. its rotation can't be read yet), asked again next frame; No = never in this view.
    // detail = the view and the reason or level, for the log.
    enum class Recenter { Yes, NotYet, No };
    virtual Recenter recenter_support(std::string& detail) {
        detail = "not supported by this engine";
        return Recenter::No;
    }
    // Removes `fraction` (0..1) of the remaining pitch to the active view's level, through the same writes as apply.
    // True when it was written (or there was nothing to do).
    virtual bool recenter(double /*fraction*/) { return false; }
    // The active gameplay view's current angles, read back from the game (degrees, in the sense apply turns them: pitch
    // up, yaw right; any zero), and which camera they belong to (any value that changes with it). For the stick fallback
    // (stick_fallback.hpp): false = this view can't be read, no fallback for it.
    virtual bool view_angles(double& /*pitch*/, double& /*yaw*/, uintptr_t& /*camera*/) { return false; }
    // The engine reports menus and pauses (EngineFrame in_game/paused/cursor/view_id). False: GyroLib's "menus only"
    // automatic calibration would never run, so the core makes it "any time".
    virtual bool sees_menus() const { return true; }
    // After apply: gyro_deg = |yaw| + |pitch| actually written this frame (0 if none).
    virtual void after_frame(double /*gyro_deg*/) {}
    // Periodic status: the scene part of the status line ("view=... pc=..."), then any detail lines of its own.
    virtual std::string status_scene() const = 0;
    virtual void status_details() {}
};

// Every engine module linked in (src/engine_registry.cpp), in detection order.
std::span<Engine* const> all_engines();

// DllMain: Engine= in Gyro4All.ini (or the game's profile) forces an engine by name; else the first one whose detect()
// matches; else the first one. Logs the choice. Needs settings::configure first.
Engine& select_engine(const std::wstring& exe_path);

}  // namespace gyro4all
