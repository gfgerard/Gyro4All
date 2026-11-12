#include "unity.hpp"

#include "fov.hpp"
#include "host.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "hooks.hpp"
#include "notify.hpp"
#include "settings.hpp"
#include "unity_aim.hpp"
#include "unity_candidates.hpp"
#include "unity_legacy_keys.hpp"
#include "unity_path.hpp"
#include "unity_probe.hpp"
#include "unity_profile.hpp"
#include "unity_quat.hpp"
#include "unity_runtime.hpp"
#include "util.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <format>
#include <string_view>
#include <unordered_map>

namespace gyro4all::unity {
namespace {

Runtime g_runtime;

enum class ApiState { Waiting, Ready, Failed };
ApiState g_api_state = ApiState::Waiting;
ULONGLONG g_api_first = 0, g_api_next = 0;

struct Api {
    void* main = nullptr;
    void* transform = nullptr;
    void* get_rotation = nullptr;
    void* set_rotation = nullptr;
    void* get_parent = nullptr;  // YawParent=
    void* rigidbody = nullptr;        // the Rigidbody class, and Physics.SyncTransforms: a yaw body with physics (body_rigidbody)
    void* sync_transforms = nullptr;
    void* game_object_transform = nullptr;  // PitchObject=/YawObject= naming a GameObject
    // Optional: managed code stripping can remove what a game doesn't call itself, and a pause or FOV signal less is no
    // reason to lose the camera.
    void* get_fov = nullptr;
    void* orthographic = nullptr;
    void* time_scale = nullptr;
    void* cursor_visible = nullptr;
    void* cursor_lock = nullptr;
} g_api;

UnityProfile g_profile;

// Assemblies may still be loading at the first frames, so a missing method is tried again for a while.
bool resolve_api() {
    void* camera = g_runtime.find_class("UnityEngine", "Camera");
    void* component = g_runtime.find_class("UnityEngine", "Component");
    void* transform = g_runtime.find_class("UnityEngine", "Transform");
    void* time = g_runtime.find_class("UnityEngine", "Time");
    void* cursor = g_runtime.find_class("UnityEngine", "Cursor");
    g_api.main = g_runtime.find_method(camera, "get_main", 0);
    g_api.transform = g_runtime.find_method(component, "get_transform", 0);
    g_api.get_rotation = g_runtime.find_method(transform, "get_rotation", 0);
    g_api.set_rotation = g_runtime.find_method(transform, "set_rotation", 1);
    g_api.get_parent = g_runtime.find_method(transform, "get_parent", 0);
    g_api.rigidbody = g_runtime.find_class("UnityEngine", "Rigidbody");
    g_api.sync_transforms = g_runtime.find_method(g_runtime.find_class("UnityEngine", "Physics"), "SyncTransforms", 0);
    g_api.game_object_transform = g_runtime.find_method(g_runtime.find_class("UnityEngine", "GameObject"), "get_transform", 0);
    g_api.get_fov = g_runtime.find_method(camera, "get_fieldOfView", 0);
    g_api.orthographic = g_runtime.find_method(camera, "get_orthographic", 0);
    g_api.time_scale = g_runtime.find_method(time, "get_timeScale", 0);
    g_api.cursor_visible = g_runtime.find_method(cursor, "get_visible", 0);
    g_api.cursor_lock = g_runtime.find_method(cursor, "get_lockState", 0);
    return (g_api.main || g_profile.enabled()) && g_api.transform && g_api.get_rotation && g_api.set_rotation;
}

// Unity's own joystick input (Input.GetKey*(KeyCode.JoystickButtonN)) reads HID devices, so it never sees the controller
// shown as XInput, while a library that polls XInput (Rewired) does: The Forest's menus move with the stick (Rewired) and
// confirm with JoystickButton0 (NGUI). Its native functions answer these keys from that controller too. Mono only.
using GetKey_t = int (*)(int);
GetKey_t g_get_key = nullptr, g_get_key_down = nullptr, g_get_key_up = nullptr;
uint16_t g_keys_now = 0, g_keys_before = 0;  // game thread, like the calls: this frame's buttons and the last one's

int hk_get_key(int key) {
    return (g_get_key(key) & 0xFF) != 0 || (g_keys_now & legacy_key_button(key)) != 0;
}
int hk_get_key_down(int key) {
    const uint16_t button = legacy_key_button(key);
    return (g_get_key_down(key) & 0xFF) != 0 || ((g_keys_now & button) != 0 && (g_keys_before & button) == 0);
}
int hk_get_key_up(int key) {
    const uint16_t button = legacy_key_button(key);
    return (g_get_key_up(key) & 0xFF) != 0 || ((g_keys_now & button) == 0 && (g_keys_before & button) != 0);
}

void hook_legacy_keys() {
    if (g_runtime.backend() != Backend::Mono) return;
    void* input = g_runtime.find_class("UnityEngine", "Input");
    void* key = g_runtime.internal_call(g_runtime.find_method(input, "GetKeyInt", 1));
    void* down = g_runtime.internal_call(g_runtime.find_method(input, "GetKeyDownInt", 1));
    void* up = g_runtime.internal_call(g_runtime.find_method(input, "GetKeyUpInt", 1));
    const bool hooked = key && down && up &&
                        hooks::hook_function(key, reinterpret_cast<void*>(&hk_get_key), reinterpret_cast<void**>(&g_get_key)) &&
                        hooks::hook_function(down, reinterpret_cast<void*>(&hk_get_key_down), reinterpret_cast<void**>(&g_get_key_down)) &&
                        hooks::hook_function(up, reinterpret_cast<void*>(&hk_get_key_up), reinterpret_cast<void**>(&g_get_key_up));
    log("unity: Input.GetKey's joystick buttons %s", hooked ? "also read the controller shown as XInput" : "can't be answered (native functions not found)");
}

void update_legacy_keys() {
    uint16_t buttons = 0;
    xinput::shown_buttons(buttons);
    g_keys_before = g_keys_now;
    g_keys_now = buttons;
}

void update_api() {
    if (g_api_state != ApiState::Waiting) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_api_first) g_api_first = now;
    if (now < g_api_next) return;
    g_api_next = now + 1000;
    if (resolve_api()) {
        g_api_state = ApiState::Ready;
        if (xinput::translates()) hook_legacy_keys();
        log("unity: API ready (fov %d, orthographic %d, timeScale %d, cursor %d, lock state %d)", g_api.get_fov != nullptr,
            g_api.orthographic != nullptr, g_api.time_scale != nullptr, g_api.cursor_visible != nullptr,
            g_api.cursor_lock != nullptr);
    } else if (now - g_api_first >= 30000) {
        g_api_state = ApiState::Failed;
        log("unity: Camera/Transform methods not found (main %d, transform %d, get_rotation %d, set_rotation %d): gyro as right stick",
            g_api.main != nullptr, g_api.transform != nullptr, g_api.get_rotation != nullptr, g_api.set_rotation != nullptr);
        notify::show(L"Gyro4All: this game's Unity camera can't be reached, so the gyro goes to the right stick.\n"
                     L"Please report it with Gyro4All\\Gyro4All.log.",
                     15);
    }
}

void* main_camera() {
    void* camera = g_runtime.invoke(g_api.main, nullptr);
    return g_runtime.alive(camera) ? camera : nullptr;
}

bool rotation(void* camera, Quaternion& q) {
    void* transform = g_runtime.invoke(g_api.transform, camera);
    NativeQuaternion native{};
    if (!transform || !g_runtime.returned(g_runtime.invoke(g_api.get_rotation, transform), native)) return false;
    q = from_native(native);
    return finite(q);
}

bool write_rotation(void* camera, const Quaternion& q) {
    void* transform = g_runtime.invoke(g_api.transform, camera);
    if (!transform || !finite(q)) return false;
    NativeQuaternion value = to_native(q);
    void* arguments[] = {&value};
    g_runtime.invoke(g_api.set_rotation, transform, arguments);
    return !g_runtime.threw();
}

// YawParent=: the Transform that many levels above the camera.
void* ancestor(void* camera, int levels) {
    void* transform = g_runtime.invoke(g_api.transform, camera);
    for (int i = 0; i < levels && transform; ++i) transform = g_runtime.invoke(g_api.get_parent, transform);
    return transform;
}

// Whether a yaw body has a Rigidbody, looked up once per body. Physics reads a Transform written outside it only at the
// next simulation step, and that late sync drops a MoveRotation the game asked for in FixedUpdate from the Rigidbody's old
// pose (Outer Wilds in zero-g with the suit: the right stick's rotational thrusters did nothing while the gyro wrote).
bool body_has_rigidbody(void* body) {
    static void* last_body = nullptr;
    static bool found = false;
    if (body != last_body) {
        last_body = body, found = false;
        void* transform = body && g_api.sync_transforms ? g_runtime.invoke(g_api.transform, body) : nullptr;
        void* array = transform && g_api.rigidbody ? g_runtime.find_objects_of_type(g_api.rigidbody) : nullptr;
        const size_t count = g_runtime.array_length(array);
        for (size_t i = 0; i < count && !found; ++i) {
            void** element = static_cast<void**>(g_runtime.array_element(array, i));
            void* rigidbody = element ? *element : nullptr;
            found = rigidbody && g_runtime.alive(rigidbody) && g_runtime.invoke(g_api.transform, rigidbody) == transform;
        }
        if (found) log("unity: the yaw body has a Rigidbody: physics takes the gyro's yaw at once (Physics.SyncTransforms)");
    }
    return found;
}

// The up a yaw body turns around: its parent's, or its own (unity_quat.hpp, "The up of a body's yaw").
unity::Vector3 body_up(void* body, const Quaternion& own) {
    void* transform = g_api.get_parent ? g_runtime.invoke(g_api.transform, body) : nullptr;
    void* parent = transform ? g_runtime.invoke(g_api.get_parent, transform) : nullptr;
    Quaternion frame;
    const bool from_parent = parent && rotation(parent, frame) && unity::yaw_up_from_parent(own, frame);
    static int logged = -1;  // said when it changes
    if (int(from_parent) != logged) {
        logged = from_parent;
        log("unity: the yaw body turns around %s", from_parent ? "its parent's up"
                                                   : parent    ? "its own up (it leans from its parent by more than a pitch)"
                                                               : "its own up (no parent)");
    }
    return unity::up_of(from_parent ? frame : own);
}

bool number(void* method, void* object, float& value) {
    return g_runtime.returned(g_runtime.invoke(method, object), value) && std::isfinite(value);
}

// Pause and cursor. A cursor locked to the centre doesn't show, whatever Cursor.visible says.
bool g_mouse_free = false;  // this frame: Cursor.lockState says the game hasn't captured the mouse for looking around

void read_gate(EngineFrame& f) {
    float time_scale = 1;
    if (number(g_api.time_scale, nullptr, time_scale)) f.paused = time_scale < 0.001;
    uint8_t visible = 0;
    int lock = 0;  // CursorLockMode: None 0, Locked 1, Confined 2
    const bool got_visible = g_runtime.returned(g_runtime.invoke(g_api.cursor_visible, nullptr), visible);
    const bool got_lock = g_runtime.returned(g_runtime.invoke(g_api.cursor_lock, nullptr), lock);
    if (got_visible) f.cursor = visible != 0 && !(got_lock && lock == 1);
    // Managed code stripping may keep only Cursor.visible's setter (MOUSE: P.I. For Hire, Unity 6): the lock state alone
    // says then. Only None counts as a menu's cursor; a confined one stays as before (no mouse look, generic view only).
    else if (!g_api.cursor_visible && got_lock) f.cursor = lock == 0;
    g_mouse_free = got_lock && lock != 1;
    static int last = -1, lines = 0;
    const int state = (got_visible ? visible != 0 : 2) * 10 + (got_lock ? lock : 9);
    if (state != last && lines < 60) {
        last = state;
        ++lines;
        log("unity: Cursor.visible=%d lockState=%d%s", got_visible ? visible != 0 : -1, got_lock ? lock : -1,
            lines == 60 ? " (no more of these lines)" : "");
    }
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// --- The profile's view ---

ManagedPath g_pair_path, g_pitch_path, g_yaw_path, g_camera_path, g_pitch_object_path, g_yaw_object_path, g_zoom_path;
std::vector<ManagedPath> g_look_paths;

void once(std::string& told, const std::string& text);

// The Transform of a component or GameObject (PitchObject=/YawObject=). Which of the two a class is, is kept per class.
void* transform_of(void* object) {
    if (!object || !g_runtime.alive(object)) return nullptr;
    static std::unordered_map<void*, int> kinds;  // 1 component, 2 GameObject, 0 neither
    void* klass = g_runtime.object_class(object);
    auto [it, added] = kinds.try_emplace(klass, 0);
    if (added) {
        for (void* c = klass; c && !it->second; c = g_runtime.class_parent(c)) {
            if (std::string_view(g_runtime.class_namespace(c)) != "UnityEngine") continue;
            const std::string_view name = g_runtime.class_name(c);
            it->second = name == "Component" ? 1 : name == "GameObject" ? 2 : 0;
        }
    }
    if (it->second == 1) return g_runtime.invoke(g_api.transform, object);
    if (it->second == 2) return g_runtime.invoke(g_api.game_object_transform, object);
    return nullptr;
}

void* object_transform(ManagedPath& path, void* root) {
    static std::string told;
    void* object = path.read_object(g_runtime, root);
    void* transform = transform_of(object);
    if (object && !transform) once(told, "PitchObject and YawObject must name a component or GameObject");
    return transform;
}

// An object a profile's paths start from. Held by a weak GC handle: nothing else in Gyro4All would tell the collector
// about it, and a destroyed object's wrapper keeps answering field reads while its Unity calls throw.
struct ProfileObject {
    std::string type;
    void* klass = nullptr;
    uintptr_t handle = 0;
    void* pointer = nullptr;  // only when the handle exports are missing
    ULONGLONG next_search = 0;
    bool told = false;

    void reset(const std::string& name) {
        g_runtime.free_handle(handle);
        *this = {};
        type = name;
    }

    template <typename Valid>
    void* get(Valid valid) {
        void* cached = handle ? g_runtime.handle_target(handle) : pointer;
        if (cached && g_runtime.alive(cached) && valid(cached)) return cached;
        const ULONGLONG now = GetTickCount64();
        if (now < next_search) return nullptr;
        next_search = now + 1000;
        if (!klass) {
            const size_t dot = type.find_last_of('.');
            klass = dot == std::string::npos ? g_runtime.find_class("", type.c_str())
                                             : g_runtime.find_class(type.substr(0, dot).c_str(), type.substr(dot + 1).c_str());
            if (!klass) return nullptr;
        }
        void* array = g_runtime.find_objects_of_type(klass);
        const size_t count = g_runtime.array_length(array);
        for (size_t i = 0; i < count; ++i) {
            void** element = static_cast<void**>(g_runtime.array_element(array, i));
            void* object = element ? *element : nullptr;
            if (!object || !g_runtime.alive(object) || !valid(object)) continue;
            g_runtime.free_handle(handle);
            handle = g_runtime.new_handle(object, true);
            pointer = handle ? nullptr : object;
            log("unity: profile object %s found (%zu instance(s))", type.c_str(), count);
            return object;
        }
        return nullptr;
    }
} g_main_object, g_yaw_object;

// Where the pitch and yaw live this frame.
struct Axes {
    uintptr_t pitch = 0, yaw = 0;
};

void* g_pair_class = nullptr;
uint32_t g_pair_pitch = 0, g_pair_yaw = 0;

void once(std::string& told, const std::string& text) {
    if (told == text) return;
    told = text;
    log("unity: profile: %s", text.c_str());
}

bool float_address(ManagedPath& path, void* root, uintptr_t& address) {
    static std::string told;
    FieldValue value;
    if (!path.resolve(g_runtime, root, value)) return false;
    if (value.type != kTypeR4) {
        once(told, "PitchPath and YawPath must end at a float field");
        return false;
    }
    if (value.copy) {
        once(told, "a path reaches its float through a method returning a value type: writes wouldn't reach the game");
        return false;
    }
    address = value.address;
    return true;
}

bool pair_addresses(void* root, Axes& axes) {
    static std::string told;
    FieldValue value;
    if (!g_pair_path.resolve(g_runtime, root, value)) return false;
    if (value.type != kTypeValueType || value.copy) {
        once(told, "PitchYawPath must end at a value type field of an object (not of a method's returned copy)");
        return false;
    }
    if (value.klass != g_pair_class) {
        void* pitch = g_runtime.find_field(value.klass, "pitch");
        void* yaw = g_runtime.find_field(value.klass, "yaw");
        if (g_runtime.field_type(pitch) != kTypeR4 || g_runtime.field_type(yaw) != kTypeR4) {
            once(told, "PitchYawPath's type " + g_runtime.class_full_name(value.klass) + " has no float pitch and yaw fields");
            return false;
        }
        g_pair_class = value.klass;
        g_pair_pitch = g_runtime.field_offset(pitch);
        g_pair_yaw = g_runtime.field_offset(yaw);
    }
    axes.pitch = value.address + g_pair_pitch - kObjectHeader;
    axes.yaw = value.address + g_pair_yaw - kObjectHeader;
    return true;
}

bool read_axis(uintptr_t address, double per_degree, double& degrees) {
    float value = 0;
    if (!address || !mem::rd(address, value) || !std::isfinite(value)) return false;
    degrees = value / per_degree;
    return true;
}

bool write_axis(uintptr_t address, double per_degree, double degrees) {
    const float value = float(degrees * per_degree);
    return address && std::isfinite(value) && mem::guarded_store(address, &value, sizeof(value));
}

void* profile_camera(void* root) {
    if (g_camera_path.empty()) return main_camera();
    void* camera = g_camera_path.read_object(g_runtime, root);
    return g_runtime.alive(camera) ? camera : nullptr;
}

// A candidate for ObjectType: its paths reach their values, and its camera exists if the profile names one.
bool main_object_valid(void* object) {
    Axes axes;
    if (!g_profile.path.empty()) {
        if (!pair_addresses(object, axes)) return false;
    } else {
        const bool pitch = g_profile.pitch_on_object() ? object_transform(g_pitch_object_path, object) != nullptr
                                                       : float_address(g_pitch_path, object, axes.pitch);
        if (!pitch) return false;
        if (g_profile.yaw_parent == 0 && g_profile.yaw_object_type.empty()) {
            const bool yaw = g_profile.yaw_on_object() ? object_transform(g_yaw_object_path, object) != nullptr
                                                       : float_address(g_yaw_path, object, axes.yaw);
            if (!yaw) return false;
        }
    }
    return g_camera_path.empty() || profile_camera(object);
}

bool yaw_object_valid(void* object) {
    if (g_profile.yaw_on_object()) return object_transform(g_yaw_object_path, object) != nullptr;
    uintptr_t address = 0;
    return float_address(g_yaw_path, object, address);
}

// Look=: the first condition that doesn't hold, or "" when they all do (or none is set). One that can't be read is false.
std::string look_blocked(void* root) {
    for (size_t i = 0; i < g_look_paths.size(); ++i) {
        bool value = false;
        const LookCondition& c = g_profile.look[i];
        const bool read = g_look_paths[i].read_bool(g_runtime, root, value);
        if (read && value != c.negate) continue;
        std::string text = c.negate ? "!" : "";
        for (const std::string& part : c.path) text += (text.size() > (c.negate ? 1u : 0u) ? "." : "") + part;
        return text + (read ? " is false" : " can't be read");
    }
    return {};
}

// --- The checks on the generic view ---

struct FollowCheck {
    bool ignored = false;  // the game rewrites this camera's Transform
    bool warned = false;   // the profile's camera didn't follow: told once
    bool have_last = false;
    double last_pitch = 0, last_yaw = 0;
    double frame_yaw = 0;       // this frame's yaw change, with the mod's last write in it
    double frame_residual = 0;  // this frame's change the mod didn't write itself
    // The current 2 s window, over gameplay frames only.
    ULONGLONG window_end = 0, last_tick = 0, look_ms = 0, idle_ms = 0;
    double moved = 0, applied = 0, looked = 0, idle = 0;
    double idle_rate = 0;  // deg/s the camera turns without look input, from the last window that measured it
    bool look = false;
    int ignored_windows = 0;
    // While ignored, a small write every so often to see whether the game keeps it again.
    int still_frames = 0, retest_wait = 0, kept = 0, retest_sign = 1;
    bool retest_pending = false;
    double retest_yaw = 0;
} g_follow;

AimCheck g_aim;
PitchClamp g_clamp;

void reset_follow() {
    g_follow = {};
    g_aim.reset();
    g_clamp = {};
}

void clear_window() {
    FollowCheck& w = g_follow;
    w.window_end = w.look_ms = w.idle_ms = 0;
    w.moved = w.applied = w.looked = w.idle = 0;
    w.look = false;
}

class UnityEngine final : public Engine {
public:
    const char* name() const override { return "Unity"; }

    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\UnityPlayer.dll");
    }

    std::wstring module_prefix(const std::wstring& /*exe_stem*/) const override { return L"Unity"; }

    bool is_launcher(const std::wstring& exe_path) const override {
        std::wstring stem = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        for (wchar_t& c : stem) c = wchar_t(towlower(c));
        return stem.starts_with(L"unitycrashhandler");
    }

    bool managed_input() const override { return true; }
    // Unity 6's player reads gamepads through Windows.Gaming.Input and loads XInput when its classes fail (MODLOG 2026-11-11).
    bool wgi_falls_back_to_xinput() const override { return true; }
    const char* default_settings() const override { return probe_default_settings(); }

    void configure() override {
        use_profile(load_unity_profile(settings::profile_path()));
        probe_configure(g_profile.probe);
        load_zoom_reference();
    }

    // Saving the settings (the panel does it often) forgets nothing unless the profile's [Unity] section changed.
    void settings_changed() override {
        UnityProfile profile = load_unity_profile(settings::profile_path());
        const bool changed = !(profile == g_profile);
        if (changed) {
            log("unity: the profile's [Unity] section changed: read again");
            use_profile(std::move(profile));
        }
        probe_configure(g_profile.probe);
        load_zoom_reference();
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        gameplay_ = false;
        discover_ok_ = false;
        const Backend backend = g_runtime.initialize();
        if (backend == Backend::Failed) {
            tell_once(told_runtime_, "unity: the runtime's exports can't be used: gyro as right stick",
                      L"Gyro4All: this game's Unity runtime can't be read, so the gyro goes to the right stick.\n"
                      L"Please report it with Gyro4All\\Gyro4All.log.");
            return blind(f, "no Unity runtime");
        }
        if (backend == Backend::Waiting) {
            watch_runtime();
            return f;
        }
        update_api();
        update_legacy_keys();
        if (g_api_state == ApiState::Waiting) return f;
        read_gate(f);
        gameplay_ = !f.paused && !f.cursor && !host::panel_open() && host::game_has_focus();
        if (g_api_state == ApiState::Failed) return blind(f, "no Unity camera methods");

        const bool profile = g_profile.enabled();
        void* camera = nullptr;
        pitch_body_ = yaw_object_body_ = nullptr;
        look_off_ = false;
        if (profile) {
            // The yaw's own root: a float or an object of YawObjectType; YawParent hangs from the camera instead.
            const bool yaw_float = g_profile.yaw_parent == 0 && !g_profile.yaw_on_object();
            root_ = g_main_object.get(main_object_valid);
            yaw_root_ = g_profile.yaw_parent > 0 || g_profile.yaw_object_type.empty() ? root_ : g_yaw_object.get(yaw_object_valid);
            axes_ = {};
            pitch_body_ = yaw_object_body_ = nullptr;
            bool found = root_ && yaw_root_;
            if (found && !g_profile.path.empty()) {
                found = pair_addresses(root_, axes_);
            } else if (found) {
                found = g_profile.pitch_on_object() ? (pitch_body_ = object_transform(g_pitch_object_path, root_)) != nullptr
                                                    : float_address(g_pitch_path, root_, axes_.pitch);
                if (found && yaw_float) found = float_address(g_yaw_path, yaw_root_, axes_.yaw);
                if (found && g_profile.yaw_on_object())
                    found = (yaw_object_body_ = object_transform(g_yaw_object_path, yaw_root_)) != nullptr;
            }
            state_pitch_ = state_yaw_ = 0;
            if (!found || !read_pitch(state_pitch_) || (yaw_float && !read_axis(axes_.yaw, g_profile.yaw_per_degree, state_yaw_))) {
                lose_camera();
                return f;
            }
            camera = profile_camera(root_);
        } else {
            camera = main_camera();
        }
        Quaternion q;
        const bool readable = camera && rotation(camera, q);
        if (!readable && !profile) {
            lose_camera();
            return f;
        }
        yaw_body_ = nullptr;
        if (g_profile.yaw_parent > 0 || yaw_object_body_) {
            Quaternion body;
            yaw_body_ = yaw_object_body_ ? yaw_object_body_ : camera ? ancestor(camera, g_profile.yaw_parent) : nullptr;
            if (!yaw_body_ || !rotation(yaw_body_, body)) {
                lose_camera();
                return f;
            }
            // Its yaw around its up (body_up), as the gyro turns it, added up frame to frame: a world yaw says little about a body
            // standing on a planet's side (Outer Wilds), and made its turns look undone.
            yaw_up_ = body_up(yaw_body_, body);
            const double turned_since = yaw_body_ == body_last_object_ ? unity::yaw_between_about(body_last_, body, yaw_up_) : 0;
            body_yaw_ = yaw_body_ == body_last_object_ ? wrap180(body_yaw_ + turned_since) : unity::view_angles(body).yaw;
            state_yaw_ = body_yaw_;
            body_last_object_ = yaw_body_, body_last_ = body;
        }
        missing_since_ = 0;
        camera_ = readable ? camera : nullptr;
        if (readable) {
            const bool same_camera = camera == last_camera_;
            last_camera_ = camera;
            follow_frame(q, same_camera);
            watch_auto_view(q, same_camera);
            rotation_ = q;
            // The look input moved the view through the last frame: whatever the gyro wrote, the game set the camera then.
            const bool look = look_now();
            if (g_profile.yaw_parent > 0 && !profile)
                probe_pitch_state(g_runtime, camera, g_profile.yaw_parent, same_camera && gameplay_ && look && looked_,
                                  unity::view_angles(q).pitch);
            looked_ = look;
        }
        f.in_game = true;
        f.view_id = 1;
        f.target = profile ? std::format("{} ({})", g_profile.object_type, backend_name(backend))
                           : std::format("Camera.main ({})", backend_name(backend));
        if (!profile) {
            // A camera that can't be aimed is no gameplay view: the gyro stays off it, the stick fallback too, and the
            // core treats it as a menu (calibration included). The windows keep running, to see it answer later.
            uint8_t ortho = 0;
            g_runtime.returned(g_runtime.invoke(g_api.orthographic, camera), ortho);
            // Hidden cursor that isn't locked: nothing in the game looks around with this mouse, so no aim camera either.
            no_mouse_look_ = ortho == 0 && g_mouse_free && !f.cursor;
            if (ortho != 0 || aim_off()) {
                f.view_id = 0;
                f.target = ortho != 0      ? "orthographic camera"
                           : no_mouse_look_ ? "camera without mouse look (cursor not locked)"
                                            : "fixed scene camera";
                return f;
            }
            discover_ok_ = true;
            if (probe_menu_on_screen()) {
                f.view_id = 0;
                f.target = "menu on screen (UI focus)";
                return f;
            }
            if (g_follow.ignored) return blind(f, "camera rewrites Transform (gyro as right stick)");
        } else {
            // Look=: the game's own "can look" state (menus, dialogue, cutscenes), on top of pause and cursor.
            const std::string blocked = look_blocked(root_);
            look_off_ = !blocked.empty();
            if (blocked != told_look_) {
                log("unity: gameplay view %s", blocked.empty() ? "on (Look= holds)" : ("off: " + blocked).c_str());
                told_look_ = blocked;
            }
            if (look_off_) {
                f.view_id = 0;
                f.target = "look off (" + blocked + ")";
                return f;
            }
        }
        // ZoomReference=: the game's own unzoomed FOV, in ZoomReferenceAxis= (or the axis the camera shows it in); else
        // ZoomReferenceFOV= (horizontal).
        const bool own_reference = profile && (g_profile.zoom_reference_value > 0 || !g_zoom_path.empty());
        float current = 0;
        if (camera_ && (own_reference || zoom_reference_ > 0) && number(g_api.get_fov, camera_, current)) {
            double reference = 0;
            if (!own_reference) reference = fov::vertical_from_horizontal(zoom_reference_, fov::game_window_aspect());
            else reference = own_zoom_reference(current);
            if (fov::is_valid(current) && fov::is_valid(reference)) f.zoom_fov = ZoomFov{current, reference};
        }
        return f;
    }

    // Before the first search the gyro waits, so it can't bend the camera away from what the game's state says. A game whose
    // camera can't be reached (blind, to the stick) will never be searched: nothing waits for it.
    bool hold_gyro() const override {
        return probe_holds_gyro() ||
               (g_api_state == ApiState::Ready && !g_profile.has_view() && probe_discovery_pending() && !probe_menu_on_screen());
    }

    // Writing the Transform bypasses the game's own pitch clamp, so the turn stops at the clamp here.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        const double limit = g_profile.pitch_limit;
        if (g_profile.enabled()) {
            // Without a limit of its own: a game often clamps only what its input adds, and past 90 the view turns over.
            const double bound = limit > 0 ? limit : 89;
            double current = 0;
            if (read_pitch(current)) pitch = limit_pitch_turn(wrap180(current), pitch, -bound, bound);
        } else if (camera_) {
            pitch = limit_pitch_turn(unity::view_angles(rotation_).pitch, pitch, limit > 0 ? -limit : g_clamp.down,
                                     limit > 0 ? limit : g_clamp.up);
        }
    }

    bool apply(double pitch, double yaw) override {
        if (g_profile.enabled()) {
            if (look_off_) return false;
            double current_pitch = 0, current_yaw = 0;
            if (!pitch_body_ && (!read_axis(axes_.pitch, g_profile.pitch_per_degree, current_pitch) ||
                                 !write_axis(axes_.pitch, g_profile.pitch_per_degree, current_pitch + pitch)))
                return false;
            if (yaw_body_) {
                if (!turn_body(yaw)) return false;
            } else if (!read_axis(axes_.yaw, g_profile.yaw_per_degree, current_yaw) ||
                       !write_axis(axes_.yaw, g_profile.yaw_per_degree, current_yaw + yaw)) {
                return false;
            }
            // PitchObject=: after the yaw, which it may hang from (it is read again, so its world pitch turns locally).
            if (pitch_body_ && !turn_object_pitch(pitch)) return false;
            if (!turn_pitch_transform(pitch)) return false;
            last_write_yaw_ = yaw;
            turn_pitch_ = pitch, turn_yaw_ = yaw;
            return true;
        }
        if (!camera_ || g_follow.ignored || aim_off()) return false;
        // YawParent=: the body takes the yaw, and the camera, hanging from it, only its own pitch on top of that.
        Quaternion from = rotation_;
        if (yaw_body_ && (!turn_body(yaw) || !rotation(camera_, from))) return false;
        const Quaternion turned = add_turn(from, pitch, yaw_body_ ? 0 : yaw);
        if (!write_rotation(camera_, turned)) return false;
        if (pitch != 0) {
            wrote_pitch_ = unity::view_angles(turned).pitch;
            wrote_pitch_valid_ = true;
        }
        rotation_ = turned;
        last_write_yaw_ = yaw;
        turn_pitch_ = pitch, turn_yaw_ = yaw;
        return true;
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (g_profile.enabled()) {
            if (look_off_ || !read_pitch(pitch)) return false;
            if (yaw_body_) yaw = state_yaw_;
            else if (g_profile.yaw_from_camera) {
                if (!camera_) return false;
                yaw = unity::view_angles(rotation_).yaw;
            }
            else if (!read_axis(axes_.yaw, g_profile.yaw_per_degree, yaw)) return false;
            camera = reinterpret_cast<uintptr_t>(root_);
            return true;
        }
        if (!camera_ || g_follow.ignored || aim_off()) return false;
        const ViewAngles angles = unity::view_angles(rotation_);
        pitch = angles.pitch, yaw = angles.yaw;
        camera = reinterpret_cast<uintptr_t>(camera_);
        return true;
    }

    Recenter recenter_support(std::string& detail) override {
        if (g_profile.enabled()) {
            if (!root_) {
                detail = "(no profile object)";
                return Recenter::NotYet;
            }
            detail = g_profile.object_type + " (level pitch)";
            return Recenter::Yes;
        }
        if (!camera_ || g_follow.ignored || aim_off()) {
            detail = !camera_ ? "(no Camera.main)" : "(the gyro isn't on this camera)";
            return Recenter::NotYet;
        }
        detail = "Camera.main (level world horizon)";
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        if (g_profile.enabled()) {
            double pitch = 0;
            if (look_off_ || !read_pitch(pitch)) return false;
            if (pitch_body_) return turn_object_pitch(-pitch * fraction);
            return write_axis(axes_.pitch, g_profile.pitch_per_degree, pitch * (1 - fraction)) && turn_pitch_transform(-pitch * fraction);
        }
        if (!camera_ || g_follow.ignored || aim_off()) return false;
        const Quaternion turned = add_turn(rotation_, -unity::view_angles(rotation_).pitch * fraction, 0);
        if (!write_rotation(camera_, turned)) return false;
        rotation_ = turned;
        return true;
    }

    void after_frame(double gyro_deg) override {
        const double gyro_yaw = gyro_deg > 0 ? std::abs(last_write_yaw_) : 0;
        last_write_yaw_ = 0;
        const bool judge = gameplay_ && (!probe_active() || probe_watching_quietly());
        const bool profile = g_profile.enabled();
        if (camera_ && (!profile || !g_profile.camera_field.empty())) follow_result(gyro_yaw, judge);
        if (camera_ && !profile) retest_ignored(judge);
        if (!profile) {
            watch_stick_fallback();
            watch_no_mouse_look(judge);
        }
        watch_missing();
        if (!g_profile.has_view()) probe_discover(g_runtime, camera_, discover_ok_ && gameplay_ && camera_);
        probe_frame(g_runtime, gameplay_);
    }

    std::string status_scene() const override {
        return std::format("unity={} camera={}{}{}{}", backend_name(g_runtime.backend()), camera_ != nullptr,
                           g_follow.ignored ? " ignored" : "", g_aim.unresponsive() ? " scene-camera" : "",
                           no_mouse_look_ ? " no-mouse-look" : "");
    }

private:
    void use_profile(UnityProfile profile) {
        g_profile = std::move(profile);
        g_pair_path = ManagedPath(g_profile.path);
        g_pitch_path = ManagedPath(g_profile.pitch_path);
        g_yaw_path = ManagedPath(g_profile.yaw_path);
        g_camera_path = ManagedPath(split(g_profile.camera_field, '.'));
        g_pitch_object_path = ManagedPath(g_profile.pitch_object);
        g_yaw_object_path = ManagedPath(g_profile.yaw_object);
        g_zoom_path = ManagedPath(g_profile.zoom_reference);
        g_look_paths.clear();
        for (const LookCondition& c : g_profile.look) g_look_paths.emplace_back(c.path);
        told_look_.clear();
        zoom_vote_.reset();
        told_zoom_axis_ = false;
        if (g_profile.enabled() && (g_profile.zoom_reference_value > 0 || !g_profile.zoom_reference.empty()))
            log("unity: profile: ZoomReference axis %s", g_profile.zoom_axis == FovAxis::Auto
                                                             ? "told from the camera's FOV (no zoom compensation until then)"
                                                             : fov_axis_name(g_profile.zoom_axis));
        pitch_body_ = yaw_object_body_ = nullptr;
        look_off_ = false;
        if (g_profile.enabled() && (g_profile.pitch_on_object() || g_profile.yaw_on_object() || !g_profile.look.empty()))
            log("unity: profile: pitch %s, yaw %s, %zu Look= condition(s)",
                g_profile.pitch_on_object() ? "on PitchObject's Transform" : "a float",
                g_profile.yaw_on_object() ? "on YawObject's Transform" : g_profile.yaw_parent > 0 ? "on YawParent" : "a float",
                g_profile.look.size());
        g_pair_class = nullptr;
        g_main_object.reset(g_profile.object_type);
        g_yaw_object.reset(g_profile.yaw_object_type);
        root_ = yaw_root_ = nullptr;
        axes_ = {};
        camera_ = last_camera_ = nullptr;
        yaw_body_ = nullptr;
        auto_yaw_ = {}, auto_pitch_ = {};
        have_state_ = false;
        missing_since_ = 0;
        told_missing_ = false;
        wrote_pitch_valid_ = false;
        reset_follow();
        if (g_api_state == ApiState::Failed) g_api_state = ApiState::Waiting, g_api_first = 0;  // a profile needs less
    }

    void load_zoom_reference() { zoom_reference_ = fov::reference_setting(); }

    // ZoomReference='s value as a vertical FOV, 0 while its axis isn't known yet. The axis the camera shows it in is told
    // either way: it decides an Auto one, and a ZoomReferenceAxis= it contradicts is logged.
    double own_zoom_reference(double current) {
        double raw = g_profile.zoom_reference_value;
        if (raw <= 0 && (!root_ || !g_zoom_path.read_number(g_runtime, root_, raw))) return 0;
        const double aspect = fov::game_window_aspect();
        const FovAxis seen = zoom_vote_.add(raw, current, aspect);
        FovAxis axis = g_profile.zoom_axis;
        if (seen != FovAxis::Auto && !told_zoom_axis_) {
            told_zoom_axis_ = true;
            if (axis == FovAxis::Auto)
                log("unity: ZoomReference %.1f is %s (the camera's unzoomed %.1f vertical at aspect %.2f); ZoomReferenceAxis= "
                    "sets it",
                    raw, seen == FovAxis::Horizontal ? "horizontal" : "vertical", current, aspect);
            else if (axis != seen)
                log("unity: ZoomReferenceAxis=%s, but ZoomReference %.1f matches the camera's %.1f vertical as %s at aspect "
                    "%.2f: check the profile",
                    fov_axis_name(axis), raw, current, seen == FovAxis::Horizontal ? "horizontal" : "vertical", aspect);
        }
        if (axis == FovAxis::Auto) axis = seen;
        if (axis == FovAxis::Horizontal) return fov::vertical_from_horizontal(raw, aspect);
        return axis == FovAxis::Vertical ? raw : 0;
    }

    static void tell_once(bool& told, const char* line, const wchar_t* notice) {
        if (told) return;
        told = true;
        log("%s", line);
        notify::show(notice, 15);
    }

    // The gyro goes to the right stick. Pause and cursor were read when they could be, so the gate still applies.
    EngineFrame& blind(EngineFrame& f, const char* why) {
        f.no_camera = true, f.in_game = true, f.view_id = 1;
        f.target = why;
        return f;
    }

    void lose_camera() {
        camera_ = nullptr;
        g_follow.have_last = false;
        g_follow.frame_yaw = g_follow.frame_residual = 0;
        wrote_pitch_valid_ = false;
        if (!missing_since_) missing_since_ = GetTickCount64();
    }

    // This frame's rotation against last frame's, before and after the mod's write.
    void follow_frame(const Quaternion& q, bool same_camera) {
        FollowCheck& w = g_follow;
        if (!same_camera) {
            reset_follow();
            wrote_pitch_valid_ = false;
        }
        const ViewAngles now = unity::view_angles(q);
        w.frame_yaw = w.frame_residual = 0;
        if (w.have_last) {
            const ViewAngles after = unity::view_angles(rotation_);
            w.frame_yaw = std::abs(wrap180(now.yaw - w.last_yaw));
            w.frame_residual = std::abs(wrap180(now.yaw - after.yaw)) + std::abs(now.pitch - after.pitch);
        }
        w.last_pitch = now.pitch, w.last_yaw = now.yaw;
        w.have_last = true;
        if (wrote_pitch_valid_) {
            wrote_pitch_valid_ = false;
            if (!look_now() && g_clamp.observe(wrote_pitch_, now.pitch))
                log("unity: learnt the game's pitch clamp: %.1f..%.1f deg", g_clamp.down, g_clamp.up);
        }
        if (w.retest_pending) {
            w.retest_pending = false;
            w.kept = std::abs(wrap180(now.yaw - w.retest_yaw)) < 0.1 ? w.kept + 1 : 0;
            if (w.kept >= 2) {
                reset_verdict();
                log("unity: the camera keeps Transform writes again: the gyro turns it");
            }
        }
    }

    // This frame's pitch, up, in degrees: the float's, or PitchObject's Transform's world pitch.
    bool read_pitch(double& degrees) {
        if (!pitch_body_) return read_axis(axes_.pitch, g_profile.pitch_per_degree, degrees);
        Quaternion q;
        if (!rotation(pitch_body_, q)) return false;
        degrees = unity::view_angles(q).pitch;
        return true;
    }

    bool turn_object_pitch(double pitch) {
        Quaternion q;
        return pitch == 0 || (rotation(pitch_body_, q) && write_rotation(pitch_body_, add_turn(q, pitch, 0)));
    }

    bool turn_body(double yaw) {
        Quaternion body;
        if (yaw == 0) return true;
        if (!rotation(yaw_body_, body)) return false;
        if (!write_rotation(yaw_body_, add_yaw_about(body, yaw_up_, yaw))) return false;
        if (body_has_rigidbody(yaw_body_)) g_runtime.invoke(g_api.sync_transforms, nullptr);
        return true;
    }

    // PitchTransform=1: the pitch written to the float, shown on the camera now (after the body's yaw, which it hangs from).
    bool turn_pitch_transform(double pitch) {
        if (!g_profile.pitch_transform || pitch_body_ || pitch == 0) return true;
        Quaternion q;
        return camera_ && rotation(camera_, q) && write_rotation(camera_, add_turn(q, pitch, 0));
    }

    // Source=auto: the player's own turns say whether the state found is the real one. Frames where the gyro wrote are left
    // out, so a smoothed camera catching up with the gyro can't count as the game turning it.
    void watch_auto_view(const Quaternion& q, bool same_camera) {
        const double turn_pitch = turn_pitch_, turn_yaw = turn_yaw_;
        turn_pitch_ = turn_yaw_ = 0;
        if (!g_profile.auto_found) return;
        const ViewAngles now = unity::view_angles(q);
        const bool have = have_state_ && same_camera;
        const double camera_yaw = wrap180(now.yaw - last_view_.yaw) - turn_yaw, camera_pitch = now.pitch - last_view_.pitch - turn_pitch;
        const double state_yaw = wrap180(state_yaw_ - last_state_yaw_) - turn_yaw;
        const double state_pitch = wrap180(state_pitch_ - last_state_pitch_) - turn_pitch;
        last_view_ = now, last_state_yaw_ = state_yaw_, last_state_pitch_ = state_pitch_;
        have_state_ = true;
        if (!have || !gameplay_ || !look_now() || std::abs(turn_yaw) > 0.05 || std::abs(turn_pitch) > 0.05) return;
        const bool has_pitch = g_profile.enabled();
        // YawFromCamera=1: the yaw float is relative to a body that catches up with it, so its change never equals the
        // camera's; the live test that found it was its check.
        const auto yaw = g_profile.yaw_from_camera ? LookStateCheck::Verdict::None : auto_yaw_.add(camera_yaw, state_yaw);
        const auto pitch = has_pitch ? auto_pitch_.add(camera_pitch, state_pitch) : LookStateCheck::Verdict::None;
        if (yaw == LookStateCheck::Verdict::Follows) log("unity: the stick or mouse turns the yaw that was found: confirmed");
        if (pitch == LookStateCheck::Verdict::Follows) log("unity: the stick or mouse turns the pitch that was found: confirmed");
        if (yaw == LookStateCheck::Verdict::Rejected) return reject_auto_view(false);
        if (pitch == LookStateCheck::Verdict::Rejected) return reject_auto_view(true);
    }

    // The profile's automatic result goes, and is remembered so the next search doesn't find it again.
    void reject_auto_view(bool pitch_axis) {
        const std::string yaw_class = g_profile.yaw_object_type.empty() ? g_profile.object_type : g_profile.yaw_object_type;
        const std::string what = pitch_axis ? (g_profile.pitch_on_object() ? object_rejection(g_profile.object_type, join_path(g_profile.pitch_object))
                                                                           : g_profile.object_type + " " + join_path(g_profile.pitch_path))
                                 : g_profile.yaw_parent > 0  ? std::format("parent {}", g_profile.yaw_parent)
                                 : g_profile.yaw_on_object() ? object_rejection(yaw_class, join_path(g_profile.yaw_object))
                                                             : yaw_class + " " + join_path(g_profile.yaw_path);
        log("unity: the camera turned with the stick or mouse but %s didn't: it isn't the look state, looking again", what.c_str());
        const std::wstring& path = settings::profile_path();
        for (const wchar_t* key : {L"ObjectType", L"YawObjectType", L"CameraField", L"PitchPath", L"YawPath", L"PitchPerDegree",
                                   L"YawPerDegree", L"YawParent", L"PitchTransform", L"PitchObject", L"YawObject", L"YawFromCamera",
                                   L"Source"})
            WritePrivateProfileStringW(L"Unity", key, nullptr, path.c_str());
        std::string list = settings::profile_string(L"Unity", L"Rejected");
        list += (list.empty() ? "" : ";") + what;
        WritePrivateProfileStringW(L"Unity", L"Rejected", std::wstring(list.begin(), list.end()).c_str(), path.c_str());
        use_profile(load_unity_profile(path));
        probe_discovery_again();
    }

    static std::string join_path(const std::vector<std::string>& parts) {
        std::string text;
        for (const std::string& part : parts) text += (text.empty() ? "" : ".") + part;
        return text;
    }

    static bool look_now() { return host::mouse_moving() || (host::right_stick_moving() && !host::stick_hidden()); }

    bool aim_off() const { return g_aim.unresponsive() || no_mouse_look_; }

    void reset_verdict() {
        FollowCheck& w = g_follow;
        w.ignored = false;
        w.ignored_windows = w.kept = 0;
    }

    // gyro_yaw: the gyro's yaw written this frame. judged only over gameplay: a closed gate ends the window, whole.
    void follow_result(double gyro_yaw, bool gameplay) {
        FollowCheck& w = g_follow;
        if (!gameplay) return clear_window();
        const ULONGLONG now = GetTickCount64();
        if (!w.window_end) {
            w.window_end = now + 2000;
            w.last_tick = now;
        }
        // As in Unreal's follow check, yaw only: at the pitch limit the camera ignores pitch writes, which isn't the game
        // ignoring the gyro.
        if (gyro_yaw > 0) {
            w.applied += gyro_yaw;
            w.moved += w.frame_yaw;
        }
        if (host::right_stick_moving() || host::mouse_moving()) w.look = true;
        if (look_now()) {
            w.looked += w.frame_residual;
            w.look_ms += now - w.last_tick;
        } else {
            w.idle += w.frame_residual;
            w.idle_ms += now - w.last_tick;
        }
        w.last_tick = now;
        if (now < w.window_end) return;
        int verdict = 0;
        if (w.applied >= 10 && !w.look) {
            const double ratio = w.moved / w.applied;
            if (ratio < 0.2) verdict = -1;
            else if (ratio > 0.5) verdict = 1;
        }
        w.ignored_windows = verdict == -1 ? w.ignored_windows + 1 : verdict == 1 ? 0 : w.ignored_windows;
        const double applied = w.applied, moved = w.moved;
        if (g_profile.enabled()) {
            if (w.ignored_windows >= 3 && !w.warned) {
                w.warned = true;
                log("unity: the gyro turned the profile's values by %.0f degrees but its camera moved only %.0f: the profile "
                    "may be outdated",
                    applied, moved);
                notify::show(L"Gyro4All: the camera doesn't follow this game's profile, which may be outdated.\nTo find the "
                             L"camera again, press the probe key (Ctrl+F7) or set Probe=1 in Gyro4All\\Gyro4All.ini.",
                             15);
            }
        } else {
            if (w.ignored_windows >= 3 && !w.ignored) {
                w.ignored = true;
                log("unity: the gyro turned Camera.main by %.0f degrees but it stayed %.0f: the game rewrites its Transform; gyro "
                    "as right stick",
                    applied, moved);
                if (!probe_request_auto(g_runtime, "the game rewrites the camera's Transform") && !probe_watching_quietly())
                    notify::show(L"Gyro4All: the game keeps rebuilding this camera, so the gyro goes to the right stick.\nTo "
                                 L"find the real camera, press the probe key (Ctrl+F7) or set Probe=1 in Gyro4All\\Gyro4All.ini.",
                                 15);
            }
            // Look input the game can see, and the game doesn't turn the camera for it: a fixed scene camera.
            if (w.idle_ms >= 200) w.idle_rate = w.idle / (double(w.idle_ms) / 1000);
            const bool was = g_aim.unresponsive();
            const bool look = w.look_ms >= AimCheck::kLookMs;
            const double answered = look_motion(w.looked, w.look_ms, w.idle_rate);
            g_aim.window(answered, look);
            // Evidence against the cursor rule, for the log: it never turns the gyro on by itself.
            if (no_mouse_look_ && look && answered >= AimCheck::kStill && !told_turns_unlocked_) {
                told_turns_unlocked_ = true;
                log("unity: the cursor isn't locked, yet the camera turned with look input (%.1f degrees in %llu ms, %.1f deg/s "
                    "without it): if this is the aim camera, the cursor rule is wrong for this game",
                    w.looked, static_cast<unsigned long long>(w.look_ms), w.idle_rate);
            }
            if (g_aim.unresponsive() != was) {
                log("unity: %s", g_aim.unresponsive() ? "the camera doesn't answer look input (a fixed scene camera): the gyro stays off it"
                                                      : "the camera answers look input again: the gyro turns it");
                if (notices_left_ > 0) {
                    --notices_left_;
                    notify::show(g_aim.unresponsive()
                                     ? L"Gyro4All: this camera doesn't turn with the stick or mouse (a fixed scene camera), so "
                                       L"the gyro stays off it.\nTurning the camera with the stick or mouse brings the gyro back."
                                     : L"Gyro4All: the camera answers the stick or mouse again: the gyro turns it.",
                                 g_aim.unresponsive() ? 10 : 5);
                }
            }
        }
        clear_window();
    }

    // The cursor rule for a few seconds of play: say why the gyro does nothing, once.
    void watch_no_mouse_look(bool gameplay) {
        if (!no_mouse_look_ || !camera_ || !gameplay) {
            no_mouse_look_since_ = 0;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (!no_mouse_look_since_) no_mouse_look_since_ = now;
        if (told_no_mouse_look_ || now - no_mouse_look_since_ < 5000) return;
        told_no_mouse_look_ = true;
        log("unity: the cursor is hidden but not locked: the game has no mouse look here, so the gyro stays off this camera");
        notify::show(L"Gyro4All: this game doesn't capture the mouse here, so the gyro stays off this camera.\nIf the camera "
                     L"should turn, press the probe key (Ctrl+F7) to find it.",
                     10);
    }

    // The camera is written only while it is still and the player isn't turning it, so the blind stick's turn can't look
    // like a kept write; a game that rewrites the Transform undoes this before it is ever shown.
    void retest_ignored(bool gameplay) {
        FollowCheck& w = g_follow;
        if (!w.ignored || !gameplay || look_now()) {
            w.still_frames = 0;
            return;
        }
        w.still_frames = w.frame_residual < 0.01 ? w.still_frames + 1 : 0;
        if (w.still_frames < 10 || ++w.retest_wait < 90) return;
        w.retest_wait = 0;
        const Quaternion turned = add_turn(rotation_, 0, 0.5 * w.retest_sign);
        w.retest_sign = -w.retest_sign;  // back and forth: no drift if it stays
        if (!write_rotation(camera_, turned)) return;
        rotation_ = turned;
        w.retest_yaw = unity::view_angles(turned).yaw;
        w.retest_pending = true;
    }

    void watch_runtime() {
        const ULONGLONG now = GetTickCount64();
        if (!first_frame_) first_frame_ = now;
        if (now - first_frame_ >= 60000)
            tell_once(told_runtime_, "unity: no Mono or IL2CPP runtime after 60 s: the gyro can't turn the camera. Please report the game with this log.",
                      L"Gyro4All: this game's Unity runtime wasn't found, so the gyro can't turn the camera.\n"
                      L"Please report it with Gyro4All\\Gyro4All.log.");
    }

    // No camera (or no profile object) for a long time, the game focused: loading screens don't last this long.
    void watch_missing() {
        if (!missing_since_ || told_missing_ || probe_active() || !host::game_has_focus()) return;
        if (GetTickCount64() - missing_since_ < 90000) return;
        told_missing_ = true;
        if (g_profile.enabled()) {
            log("unity: the profile's object %s hasn't appeared in 90 s: the gyro can't turn the camera until it does", g_profile.object_type.c_str());
            notify::show(L"Gyro4All: this game's profile hasn't found its camera yet (ignore this in menus).\nIf you are "
                         L"playing, it may be outdated: press the probe key (Ctrl+F7) to find the camera again.",
                         15);
        } else {
            log("unity: no Camera.main for 90 s: the gyro can't turn the camera until there is one");
            notify::show(L"Gyro4All: no main camera found yet (ignore this in menus).\nIf you are playing, press the probe "
                         L"key (Ctrl+F7) to look for the camera.",
                         15);
        }
    }

    // The core's stick fallback took over (the game undoes the writes): the same as a camera that ignores them, once
    // it lasts: short ones are scripted moments.
    void watch_stick_fallback() {
        if (!host::stick_fallback_active()) {
            fallback_since_ = 0;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (!fallback_since_) fallback_since_ = now;
        if (now - fallback_since_ >= 3000 && gameplay_) probe_request_auto(g_runtime, "the gyro's camera writes are undone");
    }

    void* camera_ = nullptr;       // this frame's, null when there is none
    void* last_camera_ = nullptr;  // the last one seen (only compared): the same one back keeps its verdicts
    void* root_ = nullptr;
    void* yaw_root_ = nullptr;
    void* yaw_body_ = nullptr;  // YawParent= or YawObject=: this frame's Transform
    void* yaw_object_body_ = nullptr;  // YawObject=: this frame's Transform
    void* pitch_body_ = nullptr;       // PitchObject=: this frame's Transform
    bool look_off_ = false;            // this frame: a Look= condition doesn't hold
    std::string told_look_;
    FovAxisVote zoom_vote_;
    bool told_zoom_axis_ = false;
    Axes axes_;
    double state_pitch_ = 0, state_yaw_ = 0;  // this frame's look state, in degrees
    void* body_last_object_ = nullptr;        // the yaw body of the last frame, and its rotation then
    Quaternion body_last_;
    unity::Vector3 yaw_up_;  // this frame's up of the yaw body's turns (body_up)
    double body_yaw_ = 0;    // the yaw body's turns added up (state_yaw_ is cleared at the start of each profile frame)
    double last_state_pitch_ = 0, last_state_yaw_ = 0;
    double turn_pitch_ = 0, turn_yaw_ = 0;  // what the mod turned last frame
    ViewAngles last_view_{};
    bool have_state_ = false;
    bool looked_ = false;  // look input last frame (probe_pitch_state)
    LookStateCheck auto_yaw_, auto_pitch_;
    bool discover_ok_ = false;  // this frame's camera could be searched
    Quaternion rotation_{};  // the camera's rotation, with this frame's write
    double zoom_reference_ = 0;
    double last_write_yaw_ = 0, wrote_pitch_ = 0;
    bool wrote_pitch_valid_ = false;
    bool gameplay_ = false;
    ULONGLONG first_frame_ = 0, missing_since_ = 0, fallback_since_ = 0;
    bool told_runtime_ = false, told_missing_ = false, told_no_mouse_look_ = false, told_turns_unlocked_ = false;
    bool no_mouse_look_ = false;  // this frame: the cursor rule keeps the gyro off the generic camera
    ULONGLONG no_mouse_look_since_ = 0;
    int notices_left_ = 4;
};

}  // namespace

Engine& engine() {
    static UnityEngine instance;
    return instance;
}

}  // namespace gyro4all::unity
