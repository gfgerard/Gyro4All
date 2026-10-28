#include "host.hpp"

#include "deps.hpp"
#include "engine.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "longpress.hpp"
#include "dinputpad.hpp"
#include "stick_camera.hpp"
#include "stick_fallback.hpp"
#include "nativepad.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "perf.hpp"
#include "settings.hpp"
#include "util.hpp"
#include "writewatch.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.hpp>
#include <gyrolib/overlay.h>
#include <gyrolib/steam.h>
#include <gyrolib/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The engine-neutral frame: GyroLib (settings, SDL reader, panel), the controller shown to the game (XInput, native
// pad), the gate that decides when the gyro may turn the camera, and the status line. The engine module (Engine, see
// engine.hpp) reads the scene and turns the view.
namespace gyro4all {
namespace {

Engine* g_engine = nullptr;

std::optional<gyrolib::Context> g_gyro;
std::optional<gyrolib::SdlInput> g_input;
// GyroLib's bridge to the game's own Steam Input (steam_api64.dll, already initialized and updated by the game): the
// gyro of a controller Steam Input hides from SDL, when the game reads Steam Input itself and Steam makes no virtual
// pad for GyroLib's isolated reader (Monster Hunter Wilds). Created once the game has loaded steam_api64.dll.
gl_steam_runtime* g_steam = nullptr;
uint64_t g_steam_frame = 0;
ULONGLONG g_next_steam_check = 0;
bool g_failed = false;

ULONGLONG g_next_status = 0, g_next_ini_check = 0;
FILETIME g_ini_time{};
double g_applied_yaw = 0, g_applied_pitch = 0;
StickFallback g_stick_fallback;  // the gyro as right-stick input while the game undoes its camera writes
bool g_applied_this_frame = false;
bool g_stick_suppression = false;  // GL_HOST_NATIVE_STICK_SUPPRESSION declared (xinput.cpp or nativepad.cpp can do it)
bool g_fallback_test = false;      // StickFallbackTest=1: the stick fallback forced on every other minute
bool g_native_flick = false;       // NativeFlickStick=1: flick stick through the game's own PlayStation reports too
// Stick camera (StickCamera=1, stick_camera.hpp): the right stick's turn written like the gyro's, while the game reads a
// centred right stick. Its Stick* values are read again when Gyro4All.ini or the game's profile is saved.
bool g_stick_camera_on = false;
StickCamera g_stick_camera;
std::string g_stick_camera_state;  // this frame's state for the status line ("" = off)
FILETIME g_mod_ini_time{}, g_profile_ini_time{};
// StickCameraPassButtons: held longer than kPassMs, the right stick goes to the game (a weapon wheel on R1).
std::vector<int> g_pass_buttons;
constexpr uint64_t kPassMs = 200;
bool g_long_press = false;         // GL_HOST_LONG_PRESS_BLOCKING declared (longpress.cpp, applied by xinput/nativepad)
std::vector<uint32_t> g_registered;  // engine view ids registered with GyroLib (tabs in the panel)
// Mouse look (Raw Input reaching the game's main thread): counts like the right stick for the engine's camera checks.
std::atomic<ULONGLONG> g_mouse_moved_at{0};

// Camera recenter (GyroLib's camera.recenter_button, Off until the player binds a button in the panel). Offered while
// the engine can level the active view (Engine::recenter_support); withdrawn while a view it can't level is active,
// which also hides the setting in the panel. GyroLib calls the step callback inside gl_update; the fractions are
// combined and applied after the gyro delta. Custom cameras are best effort (their level may not be 0).
bool g_recenter_supported = false;
bool g_recenter_final = false;   // the engine said No for g_recenter_view: not asked again until the view changes
uint32_t g_recenter_view = 0;    // view the support was last decided for
double g_recenter_fraction = 0;  // fraction of the remaining pitch to remove this frame

void GL_CALL on_recenter_step(void*, double fraction) {
    g_recenter_fraction = 1 - (1 - g_recenter_fraction) * (1 - std::clamp(fraction, 0.0, 1.0));
}

// Sensitivity profiles (ADS, ALT_ADS, OTHER): extra GyroLib views (tabs in the panel, inheriting the default view's
// settings) that replace the default view's settings while their button is held (Hold) or after it is pressed (Toggle).
// Only in the default view (id 1): other camera views (vehicles) keep their own tab. The engine still turns the default
// view. A Toggle profile turns off when gameplay is interrupted (no default view, pause, cursor, panel, focus), since
// Gyro4All can't read the game's own aim state. All are off by default: a profile needs both its button and its mode in
// Gyro4All.ini; until then its tab is labelled "<label> - disabled" and says how to enable it.
struct Profile {
    uint32_t id = 0;
    const char* key;  // Gyro4All.ini prefix: <key>ProfileButton, <key>ProfileMode
    const char* label;
    uint32_t parent;  // settings inheritance (ALT_ADS from ADS)
    int button = -1;  // SDL_GamepadButton, or kLeftTrigger / kRightTrigger; -1 = disabled
    std::string button_name;
    bool registered = false;  // has its tab in the panel
    bool toggle = false;
    bool down = false, on = false;
    uint64_t since = 0;  // activation order: the latest active profile wins
};
constexpr int kLeftTrigger = 100, kRightTrigger = 101;
// ALT_ADS: a second aim (e.g. a scope or an alternative weapon mode) in games that have two.
Profile g_profiles[] = {{1001, "Ads", "ADS", 1}, {1002, "AltAds", "ALT_ADS", 1001}, {1003, "Other", "OTHER", 1}};
uint32_t g_last_profile = 0;  // sensitivity profile reported last frame (0 = none)

// A profile button name: Xbox or PlayStation names (face buttons by position), case-insensitive. -1 = off/unknown.
int parse_button(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    static const struct {
        const char* name;
        int button;
    } names[] = {
        {"LT", kLeftTrigger}, {"L2", kLeftTrigger}, {"RT", kRightTrigger}, {"R2", kRightTrigger},
        {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"A", SDL_GAMEPAD_BUTTON_SOUTH}, {"CROSS", SDL_GAMEPAD_BUTTON_SOUTH},
        {"B", SDL_GAMEPAD_BUTTON_EAST}, {"CIRCLE", SDL_GAMEPAD_BUTTON_EAST},
        {"X", SDL_GAMEPAD_BUTTON_WEST}, {"SQUARE", SDL_GAMEPAD_BUTTON_WEST},
        {"Y", SDL_GAMEPAD_BUTTON_NORTH}, {"TRIANGLE", SDL_GAMEPAD_BUTTON_NORTH},
        {"BACK", SDL_GAMEPAD_BUTTON_BACK}, {"VIEW", SDL_GAMEPAD_BUTTON_BACK}, {"CREATE", SDL_GAMEPAD_BUTTON_BACK},
        {"START", SDL_GAMEPAD_BUTTON_START}, {"MENU", SDL_GAMEPAD_BUTTON_START}, {"OPTIONS", SDL_GAMEPAD_BUTTON_START},
        {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"TOUCHPAD", SDL_GAMEPAD_BUTTON_TOUCHPAD}, {"MISC", SDL_GAMEPAD_BUTTON_MISC1},
        {"RP1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"LP1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
        {"RP2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"LP2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    };
    for (const auto& n : names)
        if (name == n.name) return n.button;
    return -1;
}

void load_sensitivity_profiles() {
    for (Profile& p : g_profiles) {
        const std::wstring key(p.key, p.key + std::strlen(p.key));
        const std::string button = settings::get_string((key + L"ProfileButton").c_str());
        std::string mode = settings::get_string((key + L"ProfileMode").c_str());
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool mode_ok = mode == "hold" || mode == "toggle";
        p.button = mode_ok ? parse_button(button) : -1;
        p.button_name = button;
        p.toggle = mode == "toggle";
        if (!button.empty() && parse_button(button) < 0)
            log("settings: %sProfileButton=%s not recognised", p.key, button.c_str());
        if (!mode.empty() && !mode_ok) log("settings: %sProfileMode=%s not recognised (Hold or Toggle)", p.key, mode.c_str());
        if (p.button < 0) log("settings: profile '%s' (view %u) disabled: needs %sProfileButton= and %sProfileMode=", p.label, p.id, p.key, p.key);
        else log("settings: profile '%s' (view %u) button=%s mode=%s", p.label, p.id, button.c_str(), p.toggle ? "Toggle" : "Hold");
    }
}

// Every profile gets its tab, so the player sees it exists; a disabled one says how to enable it.
void register_sensitivity_profiles(gl_context* ctx, const std::vector<ViewInfo>& views) {
    for (Profile& p : g_profiles) {
        if (std::any_of(views.begin(), views.end(), [&](const ViewInfo& v) { return v.id == p.id; })) {
            log("profile: view Id=%u is reserved for the '%s' sensitivity profile, profile off", p.id, p.label);
            p.button = -1;
            continue;
        }
        const std::string label = p.button < 0 ? std::string(p.label) + " - disabled" : p.label;
        const std::string description =
            p.button < 0 ? std::format("Disabled. To use it, set {0}ProfileButton= (e.g. LT) and {0}ProfileMode= (Hold or "
                                       "Toggle) in Gyro4All\\Gyro4All.ini, then restart the game.", p.key)
            : p.toggle   ? std::format("Pressing {} turns these settings on, pressing it again turns them off.", p.button_name)
                         : std::format("These settings are used while {} is held.", p.button_name);
        g_gyro->register_view(p.id, label.c_str(), GL_OUTPUT_CAMERA, 0, description.c_str());
        p.registered = true;
        const bool parent_registered = std::any_of(std::begin(g_profiles), std::end(g_profiles),
                                                   [&](const Profile& o) { return o.id == p.parent && o.registered; });
        gl_set_context_parent(ctx, p.id, parent_registered ? p.parent : 1);
    }
}

bool profile_button_down(SDL_Gamepad* pad, int button) {
    if (button == kLeftTrigger || button == kRightTrigger)
        return SDL_GetGamepadAxis(pad, button == kLeftTrigger ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    return SDL_GetGamepadButton(pad, SDL_GamepadButton(button));
}

// Updates the profiles from the selected controller after gl_update: the result is reported on the next frame.
// gameplay = the default view may turn the camera now.
void update_sensitivity_profiles(SDL_Gamepad* pad, bool gameplay) {
    static uint64_t order = 0;
    for (Profile& p : g_profiles) {
        const bool down = gameplay && pad && p.button >= 0 && profile_button_down(pad, p.button);
        const bool pressed = down && !p.down;
        p.down = down;
        if (!gameplay) p.on = false;
        else if (!p.toggle) p.on = down;
        else if (pressed) p.on = !p.on;
        if (pressed && p.on) {
            p.since = ++order;
            if (p.toggle)  // a newly toggled profile replaces the other toggled one
                for (Profile& other : g_profiles)
                    if (&other != &p && other.toggle) other.on = false;
        }
    }
}

uint32_t sensitivity_profile() {
    const Profile* best = nullptr;
    for (const Profile& p : g_profiles)
        if (p.on && (!best || p.since > best->since)) best = &p;
    return best ? best->id : 0;
}

// Motion delivery, for the status line. Samples and distinct arrival times come from GyroLib's sample observer; the
// camera updates count how often the gyro turned the camera, and how often it did so without a new sample. The Switch
// Pro over Bluetooth measures a sample every ~5 ms but its reports reach the PC 1-4 at a time with gaps of 3-68 ms;
// since 1.3.1 GyroLib spreads such bursts over the following updates itself ("burst delivery"), which shows here as
// rotation on updates without new samples. (GyroUE 0.6.14-0.6.19 had its own pacer for this, Pacing=1.)
struct Delivery {
    uint64_t samples = 0, arrivals = 0, last_arrival_ns = 0;
    uint64_t updates = 0, turned = 0, spread = 0;  // camera updates; with rotation; with rotation but no new sample
    uint64_t since_ns = 0;

    void on_sample(const gl_sample& sample) {
        ++samples;
        if (sample.arrival_ns != last_arrival_ns) ++arrivals, last_arrival_ns = sample.arrival_ns;
    }
    void on_update(bool camera, uint32_t new_samples, bool rotation) {
        if (!camera) return;
        ++updates;
        if (rotation) ++turned, spread += new_samples == 0;
    }
} g_delivery;

// Motion trace (MotionTrace=1 in Gyro4All.ini; a number over 1: that many seconds instead of 30): per-sample and
// per-frame timing in Gyro4All\motion-trace.csv,
// to see where motion gets bunched (controller, transport, frame) and how GyroLib's burst delivery spreads it.
//   S,sensor_ns,arrival_ns,gyro_x,gyro_y,gyro_z         every accepted sample (GyroLib sample observer)
//   F,now_ns,new_samples,raw_yaw,raw_pitch,yaw,pitch     every frame while the gyro may turn the camera (raw = GyroLib
//                                                        output, yaw/pitch = applied; equal since Gyro4All has no pacer)
struct Trace {
    bool wanted = false, done = false;
    int seconds = 30;
    FILE* file = nullptr;
    uint64_t start_ns = 0;
} g_trace;

void GL_CALL on_sample(void*, uint64_t, const gl_sample* sample) {
    if (!sample) return;
    g_delivery.on_sample(*sample);
    if (g_trace.file)
        std::fprintf(g_trace.file, "S,%llu,%llu,%.3f,%.3f,%.3f\n", (unsigned long long)sample->sensor_ns,
                     (unsigned long long)sample->arrival_ns, sample->gyro_dps.x, sample->gyro_dps.y, sample->gyro_dps.z);
}

void trace_frame(uint64_t now_ns, bool camera, uint32_t new_samples, const gl_output& out, double pitch, double yaw) {
    if (!g_trace.wanted || g_trace.done) return;
    if (!g_trace.file) {
        if (!camera) return;  // start with the first gameplay frame (with StickFallbackTest: with the fallback on)
        if (g_fallback_test && !g_stick_fallback.active()) return;
        const std::wstring path = settings::mod_dir() + L"\\motion-trace.csv";
        if (_wfopen_s(&g_trace.file, path.c_str(), L"wb") != 0 || !g_trace.file) {
            log("trace: cannot create %s", narrow(path).c_str());
            g_trace.done = true;
            return;
        }
        std::fprintf(g_trace.file, "kind,a,b,c,d,e,f\n");
        g_trace.start_ns = now_ns;
        log("trace: recording %d s of motion timing to %s", g_trace.seconds, narrow(path).c_str());
    }
    if (camera)
        std::fprintf(g_trace.file, "F,%llu,%u,%.5f,%.5f,%.5f,%.5f\n", (unsigned long long)now_ns, new_samples,
                     out.yaw_degrees, out.pitch_degrees, yaw, pitch);
    if (now_ns - g_trace.start_ns >= uint64_t(g_trace.seconds) * 1'000'000'000ull) {
        std::fclose(g_trace.file);
        g_trace.file = nullptr;
        g_trace.done = true;
        log("trace: done (motion-trace.csv)");
    }
}

// The stick fallback's deflection this frame and what it learnt from (after the frame's F line), while it is on.
void trace_stick() {
    if (!g_trace.file) return;
    if (const std::string line = g_stick_fallback.trace(); !line.empty()) std::fprintf(g_trace.file, "%s\n", line.c_str());
}

// The game's stick as the stick fallback learnt it, saved to the game's profile so the next session starts with it.
void save_stick_model() {
    std::string text;
    if (!g_stick_fallback.take_save(text)) return;
    CreateDirectoryW((settings::mod_dir() + L"\\profiles").c_str(), nullptr);
    if (GetFileAttributesW(settings::profile_path().c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::string header = "; Gyro4All profile for " + narrow(settings::exe_stem()) + ", written by Gyro4All.\r\n\r\n";
        if (FILE* file = nullptr; _wfopen_s(&file, settings::profile_path().c_str(), L"wb") == 0 && file) {
            std::fwrite(header.data(), 1, header.size(), file);
            std::fclose(file);
        }
    }
    const bool ok = WritePrivateProfileStringW(settings::kSection, L"StickFallbackLearned", std::wstring(text.begin(), text.end()).c_str(),
                                               settings::profile_path().c_str());
    log("stick fallback: StickFallbackLearned=%s %s %s", text.c_str(), ok ? "saved to" : "could not be saved to",
        narrow(settings::profile_path()).c_str());
}

// Gives every engine view its GyroLib view (a tab in the panel, inheriting its parent's settings) and withdraws the
// ones the engine no longer has: at start, and after the engine's views changed during play (Engine::views_changed).
// A view can't take a sensitivity profile's id once that profile has its tab (at start the view wins, see
// register_sensitivity_profiles).
void register_views() {
    gl_context* ctx = g_gyro->get();
    const std::vector<ViewInfo> views = g_engine->views();
    for (const ViewInfo& v : views) {
        if (std::find(g_registered.begin(), g_registered.end(), v.id) != g_registered.end()) continue;
        if (std::any_of(std::begin(g_profiles), std::end(g_profiles), [&](const Profile& p) { return p.id == v.id && p.registered; })) {
            log("profile: view %u '%s' ignored, its id belongs to a sensitivity profile", v.id, v.label.c_str());
            continue;
        }
        try {
            g_gyro->register_view(v.id, v.label.c_str());
        } catch (const std::exception& e) {
            log("profile: view %u '%s' not registered with GyroLib: %s", v.id, v.label.c_str(), e.what());
            continue;
        }
        if (v.parent) gl_set_context_parent(ctx, v.id, v.parent);
        g_registered.push_back(v.id);
    }
    for (auto it = g_registered.begin(); it != g_registered.end();) {
        const uint32_t id = *it;
        if (id == 1 || std::any_of(views.begin(), views.end(), [&](const ViewInfo& v) { return v.id == id; })) {
            ++it;
            continue;
        }
        gl_unregister_gameplay_context(ctx, id);
        log("profile: view %u withdrawn (no longer in the engine's views)", id);
        it = g_registered.erase(it);
    }
}

bool start_gyro() {
    const std::wstring dll = settings::install_dir() + L"\\gyrolib.dll";
    if (!deps::check_gyrolib(dll)) return false;  // the game's own runtime DLLs would crash GyroLib
    if (!LoadLibraryW(dll.c_str())) {
        log("gyro: cannot load %s (error %lu)", narrow(dll).c_str(), GetLastError());
        return false;
    }
    log("gyro: GyroLib ABI %u (built against SDK %s)", gl_abi_version(), GL_VERSION_STRING);
    g_gyro.emplace();
    gl_context* ctx = g_gyro->get();
    register_views();
    register_sensitivity_profiles(ctx, g_engine->views());
    gl_set_host_capabilities(ctx, GL_HOST_MENU_STATE);

    // Mod defaults (an existing gyrolib.ini overrides them): always on, calibrate in menus, no flick stick.
    gl_setting_set(ctx, "gyro.activation", 0);
    gl_setting_set(ctx, "calibration.automatic", 1);
    gl_setting_set(ctx, "flick.mode", 0);
    const std::string dir = narrow(settings::mod_dir());
    g_gyro->initialize_settings(dir.c_str());
    log("gyro: settings %s (menu key F%u, gamepad shortcut %u)", gl_get_settings_path(ctx), gl_get_menu_key(ctx),
        gl_get_gamepad_menu_shortcut(ctx));
    double calibration = 0;
    if (!g_engine->sees_menus() && gl_setting_get(ctx, "calibration.automatic", &calibration) == GL_OK &&
        calibration == double(GL_CAL_MENUS)) {
        gl_setting_set(ctx, "calibration.automatic", GL_CAL_ANYTIME);
        log("gyro: calibration.automatic menus only -> any time (the %s module can't see the game's menus)",
            g_engine->name());
    }

    // Recenter is offered from the start, so the panel shows it in the main menu too; a gameplay view that can't
    // support it withdraws it (update_recenter_support).
    gl_set_recenter_step_callback(ctx, &on_recenter_step, nullptr);
    g_recenter_supported = true;

    g_input.emplace(*g_gyro);
    gl_set_sample_observer(ctx, &on_sample, nullptr);  // delivery status (and the motion trace) need every sample
    log("gyro: SDL reader started");
    return true;
}

// A Gyro4All.ini number that may have decimals (settings::get reads integers only).
double setting_number(const wchar_t* key, double fallback) {
    const std::string text = settings::get_string(key, L"");
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end != text.c_str() ? value : fallback;
}

void load_stick_camera(const char* why) {
    StickCamera::Settings s;
    s.speed = setting_number(L"StickSpeed", s.speed);
    s.speed_pitch = setting_number(L"StickSpeedPitch", s.speed_pitch);
    s.deadzone = setting_number(L"StickDeadzone", s.deadzone * 100) / 100;
    s.curve = setting_number(L"StickCurve", s.curve);
    s.edge = setting_number(L"StickEdge", s.edge * 100) / 100;
    s.edge_speed = setting_number(L"StickEdgeSpeed", s.edge_speed);
    s.edge_ramp_ms = setting_number(L"StickEdgeRamp", s.edge_ramp_ms);
    s.invert_y = settings::get(L"StickInvertY", 0) != 0;
    g_stick_camera.configure(s);
    g_pass_buttons.clear();
    std::string pass;
    for (const std::string& name : split(settings::get_string(L"StickCameraPassButtons"), ','))
        if (const int b = parse_button(name); b >= 0) {
            g_pass_buttons.push_back(b);
            pass += (pass.empty() ? "" : ",") + name;
        } else {
            log("settings: StickCameraPassButtons: unknown button '%s'", name.c_str());
        }
    if (!pass.empty()) log("settings: StickCameraPassButtons=%s (held over %llu ms: the right stick goes to the game)",
                           pass.c_str(), (unsigned long long)kPassMs);
    const StickCamera::Settings& c = g_stick_camera.settings();
    log("settings: StickCamera=1 (%s): yaw %.0f deg/s, pitch %.0f deg/s, deadzone %.0f%%, curve %.2f, edge %.0f%% -> "
        "%.0f deg/s in %.0f ms%s",
        why, c.speed, c.speed_pitch, c.deadzone * 100, c.curve, c.edge * 100, c.edge_speed, c.edge_ramp_ms,
        c.invert_y ? ", Y inverted" : "");
}

// Whether a file's last write time differs from the one seen (which is then updated); a missing file has none.
bool file_changed(const std::wstring& path, FILETIME& seen) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const FILETIME time = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime : FILETIME{};
    const bool changed = CompareFileTime(&time, &seen) != 0;
    seen = time;
    return changed;
}

void check_settings_file() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_ini_check) return;
    g_next_ini_check = now + 1000;
    if (g_stick_camera_on) {
        const bool mod = file_changed(settings::settings_path(), g_mod_ini_time);
        const bool profile = file_changed(settings::profile_path(), g_profile_ini_time);
        if (mod || profile) load_stick_camera(mod ? "Gyro4All.ini saved" : "the game's profile saved");
    }
    const char* path = gl_get_settings_path(g_gyro->get());
    if (!path || !*path) return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return;
    if (g_ini_time.dwLowDateTime == 0 && g_ini_time.dwHighDateTime == 0) {
        g_ini_time = data.ftLastWriteTime;
        return;
    }
    if (CompareFileTime(&data.ftLastWriteTime, &g_ini_time) != 0) {
        g_ini_time = data.ftLastWriteTime;
        const int32_t r = gl_load_settings(g_gyro->get(), path);
        log("gyro: gyrolib.ini changed, reloaded (result %d)", r);
    }
}

// The game's own PlayStation support closed the DualSense unread (it only knows the DualShock 4): remember it in the
// game's profile, so from the next start Gyro4All shows the DualSense to it as a DualShock 4 (PlayStation prompts, rumble,
// light bar). Until then the auto mode shows it as an Xbox controller. Once per session; never over a value the profile
// already has (shipped, set by the user, or taken back by unlearn_dualsense).
void learn_dualsense() {
    static bool done = false;
    if (done || !nativepad::dualsense_ignored() || !xinput::sony_library()) return;
    done = true;
    if (!settings::profile_setting(L"DualSenseAsDualShock4").empty()) return;
    CreateDirectoryW((settings::mod_dir() + L"\\profiles").c_str(), nullptr);
    const bool created = GetFileAttributesW(settings::profile_path().c_str()) == INVALID_FILE_ATTRIBUTES;
    if (created) {
        const std::string header = "; Gyro4All profile for " + narrow(settings::exe_stem()) + ", written by Gyro4All.\r\n"
                                   "; The game's own PlayStation support only knows the DualShock 4: the DualSense is shown to it as one.\r\n\r\n";
        if (FILE* file = nullptr; _wfopen_s(&file, settings::profile_path().c_str(), L"wb") == 0 && file) {
            std::fwrite(header.data(), 1, header.size(), file);
            std::fclose(file);
        }
    }
    const bool ok = WritePrivateProfileStringW(settings::kSection, L"DualSenseAsDualShock4", L"1", settings::profile_path().c_str()) &&
                    WritePrivateProfileStringW(settings::kSection, L"DualSenseAsDualShock4Learned", L"1", settings::profile_path().c_str());
    log("dualsense: the game's own PlayStation support closed the DualSense unread (it only knows the DualShock 4): %s %s",
        ok ? (created ? "profile created," : "profile updated,") : "could not write", narrow(settings::profile_path()).c_str());
    if (ok)
        notify::show(L"This game's PlayStation support doesn't know the DualSense: it works as an Xbox controller for now.\n"
                     L"Restart the game to use it as a PlayStation controller (DualShock 4).", 15);
}

// The DualShock 4 identity learn_dualsense wrote was rejected too (the game opened the DualSense shown as a DualShock 4
// and closed it unread): the game doesn't use PlayStation controllers this way at all. xinput.cpp already shows the
// DualSense as an Xbox controller; the profile gets DualSenseAsDualShock4=0, which also keeps it from being learned again.
void unlearn_dualsense() {
    static bool done = false;
    if (done || !nativepad::learned_ds4_rejected()) return;
    done = true;
    const wchar_t* path = settings::profile_path().c_str();
    const bool ok = WritePrivateProfileStringW(settings::kSection, L"DualSenseAsDualShock4", L"0", path) &&
                    WritePrivateProfileStringW(settings::kSection, L"DualSenseAsDualShock4Learned", nullptr, path);
    log("dualsense: the game rejected the DualSense shown as a DualShock 4 too (opened, closed unread): shown as an Xbox "
        "controller, DualSenseAsDualShock4=0 %s %s",
        ok ? "written to" : "could not be written to", narrow(settings::profile_path()).c_str());
    notify::show(L"This game doesn't take the DualSense as a DualShock 4 either: it works as an Xbox controller.", 10);
}

void status() {
    gl_gyro_state gs{};
    gl_get_gyro_state(g_gyro->get(), &gs);
    gl_diagnostics d{};
    gl_get_diagnostics(g_gyro->get(), &d);
    log("status: %s | gyro enabled=%u motion=%u calib=%u samples=%llu still=%u bias=(%.2f %.2f %.2f) | applied "
        "yaw=%.1f pitch=%.1f",
        g_engine->status_scene().c_str(), gs.enabled, gs.motion_available, d.calibration_state,
        (unsigned long long)d.accepted_samples, d.stationary, d.bias.x, d.bias.y, d.bias.z, g_applied_yaw, g_applied_pitch);
    if (const std::string fallback = g_stick_fallback.status(); !fallback.empty()) log("status: %s", fallback.c_str());
    if (g_stick_camera_on) log("status: stick camera %s", g_stick_camera_state.c_str());
    g_engine->status_details();
    Delivery& m = g_delivery;
    const uint64_t now_ns = SDL_GetTicksNS();
    const double span = m.since_ns ? double(now_ns - m.since_ns) * 1e-9 : 0;
    if (span > 0)
        log("status: motion %.0f samples/s in %.0f arrivals/s | camera updates %.0f/s, %.0f%% turned, %.0f%% of "
            "those without a new sample (GyroLib burst delivery)",
            m.samples / span, m.arrivals / span, m.updates / span, m.updates ? 100.0 * m.turned / m.updates : 0.0,
            m.turned ? 100.0 * m.spread / m.turned : 0.0);
    m.samples = m.arrivals = m.updates = m.turned = m.spread = 0;
    m.since_ns = now_ns;
}

// GyroLib's panel is driven with the controller while it is open, so the game reads an idle controller then (XInput
// and its own PlayStation support), and after the panel closes until every button is released and the triggers are
// up: the press that closed it doesn't reach the game either.
bool panel_has_pad(gl_context* ctx, SDL_Gamepad* pad) {
    static bool held = false;
    if (gl_panel_open(ctx)) return held = true;
    if (!held || !pad) return held = false;
    bool busy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 ||
                SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !busy; ++b) busy = SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
    return held = busy;
}

// Offers (or withdraws) the recenter button for the active view: decided when the view changes, and retried while the
// engine can't tell yet (no withdrawal on a transient read failure, which would cancel a transition). It starts
// offered (start_gyro). No gameplay view keeps the current state: GyroLib discards requests in menus anyway, and
// withdrawing there would hide the setting in the panel opened from a menu.
void update_recenter_support(uint32_t view_id) {
    if (!view_id) return;
    if (view_id == g_recenter_view && (g_recenter_supported || g_recenter_final)) return;
    std::string detail;
    const Engine::Recenter r = g_engine->recenter_support(detail);
    const bool supported = r == Engine::Recenter::Yes;
    const bool changed = supported != g_recenter_supported || view_id != g_recenter_view;
    g_recenter_view = view_id;
    g_recenter_final = r == Engine::Recenter::No;
    if (!changed) return;
    if (supported != g_recenter_supported)
        gl_set_recenter_step_callback(g_gyro->get(), supported ? &on_recenter_step : nullptr, nullptr);
    g_recenter_supported = supported;
    // Views that take turns (Unreal's Match=Camera views and their base view): each view's support is logged once,
    // then only changes.
    static std::vector<std::pair<uint32_t, bool>> logged;
    const std::pair<uint32_t, bool> entry{view_id, supported};
    if (std::find(logged.begin(), logged.end(), entry) != logged.end()) return;
    logged.push_back(entry);
    log("recenter: %s in view %u %s", supported ? "available" : "unavailable", view_id, detail.c_str());
}

// A state logged when it changes and the new value has held for a while, so a hold-to-aim button or a pad being
// picked up doesn't flood the log. The first value is logged at once.
struct StateLog {
    int logged = -1, pending = -1;
    ULONGLONG since = 0;
    bool changed(int value, ULONGLONG now, ULONGLONG hold_ms) {
        if (value == logged) return pending = -1, false;
        if (value != pending) pending = value, since = now;
        if (logged >= 0 && now - since < hold_ms) return false;
        logged = value, pending = -1;
        return true;
    }
};

// The window in front when the game isn't: its process file name and window class.
std::string foreground_window() {
    const HWND w = GetForegroundWindow();
    if (!w) return "none";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[128] = {}, path[MAX_PATH] = {};
    GetClassNameW(w, cls, 128);
    DWORD size = MAX_PATH;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        if (!QueryFullProcessImageNameW(p, 0, path, &size)) path[0] = 0;
        CloseHandle(p);
    }
    const wchar_t* file = std::wcsrchr(path, L'\\');
    return std::format("{} (window class {})", narrow(file ? file + 1 : path), narrow(cls));
}

// What the "gyro on/off" line can't see: focus, GyroLib's own permission (activation mode, focus, pause, menus,
// calibration) and the sensor stream. A gyro that stops while Gyro4All still says "gyro on" shows up here.
void log_gyro_changes(bool focused, const gl_gyro_state& gs) {
    static StateLog focus, enabled, motion, still, source;
    static uint64_t last_samples = 0;
    gl_diagnostics d{};
    gl_get_diagnostics(g_gyro->get(), &d);
    const ULONGLONG now = GetTickCount64();
    if (focus.changed(focused, now, 500))
        log("focus: %s", focused ? "the game window is in front" : ("lost to " + foreground_window()).c_str());
    if (enabled.changed(gs.enabled != 0, now, 1000))
        log("gyro: GyroLib %s", gs.enabled ? "enables the gyro"
                                           : "suspends the gyro (activation mode, focus, pause, menus or calibration)");
    if (motion.changed(gs.motion_available != 0, now, 1000))
        log("gyro: motion data %s", gs.motion_available ? "available" : "lost (no controller with a gyro)");
    if (source.changed(int(d.source), now, 0)) log("gyro: motion source %u", d.source);
    if (still.changed(d.stationary != 0, now, 3000))
        log("gyro: the controller %s", d.stationary ? "reads as lying still" : "moves");
    if (d.accepted_samples < last_samples)
        log("gyro: GyroLib's sample count restarted (%llu -> %llu): the controller was reopened",
            (unsigned long long)last_samples, (unsigned long long)d.accepted_samples);
    last_samples = d.accepted_samples;
}

// Game thread, after the SDL poll and before gl_update, once per frame (the game updates Steam once per frame too).
void poll_steam(uint64_t now_ns) {
    if (!g_steam) {
        const ULONGLONG now = GetTickCount64();
        if (now < g_next_steam_check) return;
        g_next_steam_check = now + 1000;
        if (!GetModuleHandleW(L"steam_api64.dll")) return;
        g_steam = gl_steam_runtime_create(g_gyro->get());
        log("gyro: Steam Input reader %s (the game's steam_api64.dll)", g_steam ? "started" : "unavailable");
        if (!g_steam) {
            g_next_steam_check = ~0ull;
            return;
        }
    }
    const int32_t r = gl_steam_runtime_poll(g_steam, now_ns, ++g_steam_frame);
    static int32_t last = 1;
    static std::string last_error;
    const char* error = gl_steam_runtime_error(g_steam);
    const std::string text = error ? error : "";
    if (r != last || text != last_error) {
        last = r;
        last_error = text;
        log("gyro: Steam Input reader: %s%s%s", r == GL_OK ? "ok" : r == GL_UNAVAILABLE ? "unavailable" : "error",
            text.empty() ? "" : ": ", text.c_str());
    }
}

// GyroLib's flick stick uses the right stick in this GyroLib view: any mode but Off and touchpad only (the effective
// value, so a controller without a right stick reads as Off).
bool flick_uses_stick(uint32_t view) {
    if (!view) return false;
    char id[48];
    std::snprintf(id, sizeof id, "context.%u.flick.mode", view);
    double mode = GL_FLICK_OFF;
    if (gl_setting_get_effective(g_gyro->get(), id, &mode) != GL_OK) return false;
    return int(mode) != GL_FLICK_OFF && int(mode) != GL_FLICK_TOUCHPAD;
}

// The stick camera's state: kept for the status line, logged when it changes (not for idle, which follows the gyro's
// gate, logged already).
void log_stick_camera(const char* state) {
    if (g_stick_camera_state == state) return;
    g_stick_camera_state = state;
    static std::string logged;
    if (!*state || std::strncmp(state, "idle", 4) == 0 || logged == state) return;
    logged = state;
    log("stick camera: %s", state);
}

void frame() {
    if (!g_gyro && !start_gyro()) {
        g_failed = true;
        return;
    }
    check_settings_file();

    // Settings panel: attach once the game's swapchain (or D3D9 device) is known. Without a renderer, never
    // leave the (invisible) panel open, or it would hold the gyro suspended.
    overlay::attach(g_gyro->get());
    overlay::poll_menu_key(g_gyro->get(), host::game_has_focus());
    overlay::process();
    if (!overlay::available() && gl_panel_open(g_gyro->get())) gl_set_panel_open(g_gyro->get(), 0);

    const EngineFrame f = g_engine->read_frame();
    if (g_engine->views_changed()) {
        register_views();
        g_recenter_view = 0;  // decide recenter support again
        g_recenter_final = false;
    }
    const bool view = f.view_id != 0;

    gl_host_state host{};
    host.focused = host::game_has_focus();
    host.paused = f.paused;
    // Menus: some games (Cronos) show no cursor in gamepad menus, so pause and "no gameplay view" count too
    // (automatic "menus only" calibration needs it).
    host.menu_open = !f.in_game || f.cursor || f.paused || !view;
    host.camera_allowed = view && !f.cursor;
    // Log every change of whether the gyro may turn the camera, with the reason, so a gyro that "suddenly stops"
    // can be matched to a cursor, a pause or a lost view at that exact moment.
    {
        const char* gate = !view ? "off: no gameplay view" : f.cursor ? "off: mouse cursor shown" : f.paused ? "off: game paused"
                                                                                                 : "on";
        static std::string last_gate;
        if (last_gate != gate) {
            last_gate = gate;
            log("gyro %s (target %s)", gate, f.target.c_str());
        }
    }
    const uint64_t now_ns = SDL_GetTicksNS();
    update_recenter_support(f.view_id);
    g_recenter_fraction = 0;
    // The default view (and the views that stand in for it, EngineFrame::base_view_id) reports the active sensitivity
    // profile instead (decided from last frame's buttons).
    const bool default_view = (f.base_view_id ? f.base_view_id : f.view_id) == 1;
    const uint32_t profile = default_view ? sensitivity_profile() : 0;
    if (profile != g_last_profile) {
        g_last_profile = profile;
        std::string label = "none";
        for (const ViewInfo& v : g_engine->views())
            if (v.id == f.view_id) label = v.label;
        for (const Profile& p : g_profiles)
            if (p.id == profile) label = p.label;
        log("profile: %s", label.c_str());
    }
    g_input->poll(now_ns);
    poll_steam(now_ns);
    const gl_output out = g_gyro->update(now_ns, host, profile ? profile : f.view_id);
    // GyroLib's flick stick needs the game's own right-stick rotation suppressed: possible for the controller shown to
    // the game as XInput, or when the game reads the controller itself through a checked report layout.
    SDL_Gamepad* pad = xinput::selected_gamepad(g_gyro->get(), g_input->get(), out);
    overlay::draw(pad);  // D3D11/D3D10/D3D9 games: GyroLib's static panel, built after gl_update
    const bool neutral = panel_has_pad(g_gyro->get(), pad);
    update_sensitivity_profiles(pad, default_view && host.camera_allowed && host.focused && !f.paused && !neutral);
    xinput::set_neutral(neutral);
    nativepad::set_neutral(neutral);
    dinputpad::frame(neutral, pad);
    const unsigned panel_capture = overlay::capture();
    dinputpad::set_capture((panel_capture & GL_OVERLAY_CAPTURE_MOUSE) != 0, (panel_capture & GL_OVERLAY_CAPTURE_KEYBOARD) != 0);
    // Long-press blocking: the activation button's press is held back from the game, or given to it as a short tap,
    // wherever the game reads that controller through a path Gyro4All can change.
    const longpress::Mask buttons = longpress::frame(g_gyro->get(), pad, now_ns, host.focused && !neutral, g_long_press);
    const bool real_buttons = xinput::set_buttons(pad, buttons.hide, buttons.tap);
    const bool native_buttons = nativepad::set_buttons(buttons.hide, buttons.tap);
    // Stick camera: in gameplay (the gyro's camera gate, focus, no pause, no panel), unless GyroLib's flick stick uses the
    // right stick (it keeps priority) or the stick fallback gives the gyro's turn to the game's own stick.
    const bool stick_gameplay = g_stick_camera_on && pad && view && host.camera_allowed && host.focused && !f.paused &&
                                !neutral && !g_engine->hold_gyro();
    const bool flick = stick_gameplay && (out.suppress_native_right_stick || flick_uses_stick(profile ? profile : f.view_id));
    // A pass button held over kPassMs: the right stick goes to the game until every pass button is released.
    static uint64_t pass_since = 0;
    bool pass_held = false;
    for (int b : g_pass_buttons) pass_held = pass_held || (pad && profile_button_down(pad, b));
    if (!pass_held) pass_since = 0;
    else if (!pass_since) pass_since = now_ns;
    const bool passing = pass_held && now_ns - pass_since >= kPassMs * 1'000'000;
    const bool stick_want = stick_gameplay && !flick && !passing && !g_stick_fallback.active();
    const bool xinput_shown = xinput::frame(g_gyro->get(), g_input->get(), out, stick_want);
    const bool native = nativepad::frame(pad, (out.suppress_native_right_stick != 0 || stick_want) && !xinput_shown);
    learn_dualsense();
    unlearn_dualsense();
    // The stick camera turns only where the game reads the centred stick (otherwise both would turn the camera).
    const bool stick_active = stick_want && (xinput_shown || native);
    double stick_pitch = 0, stick_yaw = 0;
    if (stick_active)
        g_stick_camera.turn(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f,
                            -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f, now_ns, stick_pitch, stick_yaw);
    else g_stick_camera.reset();
    log_stick_camera(!g_stick_camera_on ? ""
                     : stick_active      ? (xinput_shown ? "on (the controller shown as XInput)"
                                                         : "on (the game's own controller reports)")
                     : flick             ? "off while GyroLib's flick stick uses the right stick"
                     : stick_gameplay && passing ? "idle (a StickCameraPassButtons button held: the stick goes to the game)"
                     : stick_gameplay && g_stick_fallback.active() ? "off while the stick fallback turns the camera"
                     : stick_want ? "unavailable: Gyro4All can't hide the right stick from the game here (Steam Input, a "
                                    "real Xbox controller, or the game's own controller reports not checked yet: move "
                                    "the right stick)"
                                  : "idle (no gameplay)");
    // Flick stick through the game's own reports only with NativeFlickStick=1 (StickCamera=1 arms them too).
    const bool flick_suppression = xinput_shown || (native && g_native_flick);
    const bool long_press = xinput_shown || real_buttons || native_buttons;
    if (flick_suppression != g_stick_suppression || long_press != g_long_press) {
        g_stick_suppression = flick_suppression;
        if (long_press != g_long_press)
            log("long-press blocking %s", long_press ? (xinput_shown ? "available (the controller shown as XInput)"
                                                       : real_buttons ? "available (the real XInput controller)"
                                                                      : "available (the game's own controller reports)")
                                                     : "unavailable: the game reads the controller in a way Gyro4All can't change");
        g_long_press = long_press;
        gl_set_host_capabilities(g_gyro->get(), GL_HOST_MENU_STATE |
                                                    (g_stick_suppression ? GL_HOST_NATIVE_STICK_SUPPRESSION : 0u) |
                                                    (g_long_press ? GL_HOST_LONG_PRESS_BLOCKING : 0u));
    }

    double gyro_deg = 0;  // gyro rotation actually written this frame
    const double pitch = out.pitch_degrees, yaw = out.yaw_degrees;
    {
        gl_gyro_state gs{};
        gl_get_gyro_state(g_gyro->get(), &gs);
        const bool camera = view && host.camera_allowed && gs.enabled;
        log_gyro_changes(host.focused, gs);
        g_delivery.on_update(camera, gs.new_samples, yaw != 0 || pitch != 0);
        trace_frame(now_ns, camera, gs.new_samples, out, pitch, yaw);
    }
    // Stick fallback: when the game undoes the gyro's camera writes, its turn goes to the game as right-stick input;
    // when the engine has no hook for the game's camera at all, always (blind).
    const bool turning = view && host.camera_allowed && !g_engine->hold_gyro();
    if (f.no_camera) {
        g_stick_fallback.frame_blind(turning && host.focused && !f.paused && !neutral, now_ns);
    } else {
        double view_pitch = 0, view_yaw = 0;
        uintptr_t camera = 0;
        const bool known = turning && !neutral && g_engine->view_angles(view_pitch, view_yaw, camera);
        // The stick camera's turn counts as the player's stick: no verdict on the gyro's writes while it turns (a turn
        // against the game's pitch limit is no undone write, and isn't owed to the game's stick).
        const bool player_stick = (stick_active && (stick_pitch != 0 || stick_yaw != 0)) ||
                                  (pad && (std::abs(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX)) > 6000 ||
                                           std::abs(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)) > 6000));
        if (g_fallback_test) {
            // Forced in the first minute of gameplay and every other one after, detection only in between (to compare
            // both in any game).
            static const ULONGLONG since = GetTickCount64();
            static int phase = -1;
            const int now_phase = int((GetTickCount64() - since) / 60000 % 2);
            if (now_phase != phase) {
                phase = now_phase;
                g_stick_fallback.force(phase == 0);
                log("stick fallback test: %s for a minute", phase == 0 ? "forced on" : "detection only");
                notify::show(phase == 0 ? L"Stick fallback test: the gyro turns the game's right stick for a minute"
                                        : L"Stick fallback test: direct gyro writes for a minute",
                             4);
            }
        }
        g_stick_fallback.frame(known, view_pitch, view_yaw, camera, player_stick, now_ns);
    }
    // The stick camera's turn goes with the gyro's (one write; the engine's follow check sees both); the stick fallback
    // judges and owes the gyro's share only.
    double turn_pitch = pitch + stick_pitch, turn_yaw = yaw + stick_yaw;
    double gyro_pitch = pitch, gyro_yaw = yaw;  // the gyro's share of the turn
    if (turning && (turn_yaw != 0 || turn_pitch != 0)) {
        // A turn the view can't take (past the game's pitch limit) is left out, the gyro's share in proportion.
        const double asked_pitch = turn_pitch, asked_yaw = turn_yaw;
        g_engine->limit_turn(turn_pitch, turn_yaw);
        if (turn_pitch != asked_pitch) gyro_pitch *= asked_pitch != 0 ? turn_pitch / asked_pitch : 0;
        if (turn_yaw != asked_yaw) gyro_yaw *= asked_yaw != 0 ? turn_yaw / asked_yaw : 0;
    }
    if (turning && (turn_yaw != 0 || turn_pitch != 0)) {
        const bool direct = g_stick_fallback.direct();
        const bool written = direct && g_engine->apply(turn_pitch, turn_yaw);
        if (written) {
            g_applied_this_frame = true;
            g_applied_yaw += turn_yaw;
            g_applied_pitch += turn_pitch;
            gyro_deg = std::abs(turn_yaw) + std::abs(turn_pitch);
        }
        if (gyro_yaw != 0 || gyro_pitch != 0) g_stick_fallback.turn(gyro_pitch, gyro_yaw, written);
    }
    float stick_x = 0, stick_y = 0;
    if (!g_stick_fallback.stick(stick_x, stick_y) || neutral) stick_x = stick_y = 0;
    trace_stick();
    save_stick_model();
    dinputpad::set_stick(stick_x, stick_y);
    xinput::set_stick(stick_x, stick_y);
    nativepad::set_stick(stick_x, stick_y);
    // Recenter after the gyro delta (GyroLib's order); it isn't gyro motion, so the follow check doesn't count it.
    if (g_recenter_fraction > 0 && view && f.view_id == g_recenter_view && g_recenter_supported && host.camera_allowed &&
        !g_engine->hold_gyro()) {
        if (g_engine->recenter(g_recenter_fraction)) g_applied_this_frame = true;
        else log("recenter: view %u rotation not written, request dropped", f.view_id);
    }
    g_engine->after_frame(gyro_deg);

    const ULONGLONG now = GetTickCount64();
    if (now >= g_next_status) {
        g_next_status = now + 10000;
        status();
        perf::report();
    }
}

}  // namespace

namespace host {

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool right_stick_moving(bool* available) {
    gl_flick_input fi{};
    if (!g_gyro || gl_get_flick_input(g_gyro->get(), &fi) != GL_OK || !(fi.available & GL_FLICK_INPUT_STICK)) return false;
    if (available) *available = true;
    return std::hypot(fi.stick_x, fi.stick_y) > 0.3f;
}

bool mouse_moving() { return GetTickCount64() - g_mouse_moved_at.load() < 150; }

}  // namespace host

void host_raw_input(void* raw_input_handle) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    // A HID report doesn't fit (or isn't a mouse): that read fails and is ignored.
    if (GetRawInputData(static_cast<HRAWINPUT>(raw_input_handle), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return;
    if (raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    if (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0) g_mouse_moved_at = GetTickCount64();
}

void host_configure(Engine& engine) {
    g_engine = &engine;
    settings::ensure_file(engine.default_settings());
    const int trace = settings::get(L"MotionTrace", 0);
    g_trace.wanted = trace != 0;
    g_trace.seconds = trace > 1 ? trace : 30;
    if (g_trace.wanted) log("settings: MotionTrace=%d (%d s from the first gameplay frame)", trace, g_trace.seconds);
    // On-screen notices (camera check, probe progress); Notices=0 keeps them in the log only.
    const bool notices = settings::get(L"Notices", 1) != 0;
    log("settings: Notices=%d (Gyro4All.ini, the game's profile overrides)", notices ? 1 : 0);
    notify::set_enabled(notices);
    engine.configure();
    load_sensitivity_profiles();
    g_native_flick = settings::get(L"NativeFlickStick", 0) != 0;
    g_fallback_test = settings::get(L"StickFallbackTest", 0) != 0;
    if (g_fallback_test) log("settings: StickFallbackTest=1 (the stick fallback forced on every other minute)");
    // The game's stick as the stick fallback learnt it in an earlier session (written to the profile by save_stick_model).
    if (const std::string saved = settings::get_string(L"StickFallbackLearned"); !saved.empty()) {
        const std::string what = g_stick_fallback.restore(saved) ? "(the game's stick: " + g_stick_fallback.model().describe() + ")"
                                                                 : std::string("(unreadable: ignored, learnt again)");
        log("settings: StickFallbackLearned=%s %s", saved.c_str(), what.c_str());
    }
    g_stick_camera_on = settings::get(L"StickCamera", 0) != 0;
    if (g_stick_camera_on) {
        load_stick_camera("Gyro4All.ini, the game's profile overrides");
        file_changed(settings::settings_path(), g_mod_ini_time);
        file_changed(settings::profile_path(), g_profile_ini_time);
    }
    // The game's own PlayStation reports: right-stick suppression for flick stick and the stick camera.
    nativepad::install(g_native_flick || g_stick_camera_on, settings::get(L"DualSenseAsDualShock4", 0) != 0,
                       settings::profile_setting(L"DualSenseAsDualShock4") == "1" &&
                           settings::profile_setting(L"DualSenseAsDualShock4Learned") == "1");
    xinput::Settings xs;
    xs.mode = settings::get(L"XInput", xinput::Auto);
    xs.nintendo_labels = settings::get(L"XInputNintendoLabels", 0) != 0;
    xs.touchpad_back = settings::get(L"XInputTouchpadBack", 1) != 0;
    xs.trace = settings::get(L"XInputTrace", 0) != 0;
    xs.window_class = engine.window_class();
    xs.controllers_changed = [] { g_engine->controllers_changed(); };
    xinput::install(xs);
    // The game's DirectInput controllers: idle while GyroLib's panel has the controller (every XInput mode), and
    // XInput auto mode's sign (registered by xinput::install).
    dinputpad::install();
    // Diagnostics: which code writes an address (finding a camera's source); off unless WriteWatch is set.
    if (!settings::get_string(L"WriteWatch").empty()) writewatch::start();
}

bool host_frame() {
    if (g_failed) return false;
    g_applied_this_frame = false;
    try {
        frame();
    } catch (const std::exception& e) {
        log("fatal: %s (gyro disabled for this session)", e.what());
        g_failed = true;
    } catch (...) {
        log("fatal: unknown exception (gyro disabled for this session)");
        g_failed = true;
    }
    return g_applied_this_frame;
}

}  // namespace gyro4all
