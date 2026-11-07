#include "unity_probe.hpp"

#include "host.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "notify.hpp"
#include "probe_key.hpp"
#include "settings.hpp"
#include "unity_aim.hpp"
#include "unity_candidates.hpp"
#include "unity_path.hpp"
#include "unity_quat.hpp"
#include "unity_runtime.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The camera probe, like Unreal's: Unity has no ControlRotation, so where a game's look input lives differs per game,
// but it is nearly always a float in one of its scripts. Started by Probe=1, the probe key or AutoProbe:
// 1. Scan: the floats of every active MonoBehaviour outside Unity's own (through the fields of value types, and one
//    object reference deep), found with the runtime's reflection exports, which managed code stripping doesn't touch.
// 2. Pick: which camera turns with the right stick or mouse. None after a while: a fixed scene camera, told as such.
// 3. Capture: while the player turns it, every float's change is correlated with the camera's yaw and pitch, which also
//    gives each float's units per degree. Turns without the stick or mouse (cutscenes) don't count.
// 4. Test, while the player doesn't touch the controller: the best candidates of each axis get a small turn written, as
//    a profile would. A candidate passes if the camera turns by that much, keeps it, and comes back when it is undone.
//    An output (the game rewrites it) or a copy the camera doesn't read fails.
// 5. Both axes verified: written to the game's [Unity] profile, which applies at once. A game that has a [Unity] section
//    gets <exe>.suggested.ini instead, never overwritten. Nothing unverified is ever written.
// F7 alone dumps the cameras and the look-named floats to the probe log.
//
// With no input (probe_discover), on any game without a profile, as soon as gameplay has a camera and the mouse is captured:
// 1. Nudge: the look-named floats, likeliest first and a batch at a time, get a tiny change for a moment. A batch that moves a
//    camera is split until one float is left; its effect on the camera gives its axis and its units per degree.
// 2. Those go through the same live test as above, and are written the same way, marked Source=auto.
// 3. A yaw with no float: the classic script turns its body's Transform. The nearest ancestor of the camera with a
//    CharacterController or Rigidbody gets the same test (YawParent=). With no pitch float either, pitch stays on the camera,
//    and probe_pitch_state looks for the game's own pitch float during play (PitchTransform=).
// 4. Nothing found: Scan, Pick, Capture and Test go on quietly while the player plays, with no notices and the gyro on.
// It starts on a camera nobody is turning (a cutscene pans) and belongs to that camera: a new camera ends it, and a menu ends it
// and starts it over a moment after the menu closes. Until a camera has had its search the gyro waits: its turns would keep the
// camera from settling, on a Camera.main that is only the last resort. Nothing
// says for sure that a scene is gameplay, so a search that found nothing is cheap and comes back: on each new camera, once more
// after a while (the scene may have loaded before the player could look), and when the player turns the camera, which is proof.
// The engine confirms a Source=auto view with the first real stick or mouse turn, and takes it back if it doesn't follow.
namespace gyro4all::unity {
namespace {

constexpr int kTurnsForTest = 3, kMaxTurns = 8, kMaxCandidates = 12, kMaxAutoStarts = 3, kMaxDumpLines = 300;
constexpr size_t kMaxSlots = 30000;
constexpr int kMaxInstancesPerClass = 16, kMaxCameras = 16;
constexpr double kTurnDegrees = 30, kMinPitchDegrees = 15, kPickDegrees = 15, kTestTurn = 3;
constexpr ULONGLONG kSampleMs = 100, kLookHoldMs = 500, kRestMs = 300, kNoCameraLookMs = 15000, kAutoGiveUpMs = 45000;
// The search with no input. Long enough for a real-time smoothed camera; short enough for the gyro to wait.
constexpr ULONGLONG kNudgeSettleMs = 150, kNudgeWatchMs = 100, kNudgeMaxMs = 600, kNudgeBudgetMs = 15000, kDiscoverStableMs = 1500, kDiscoverAfterMenuMs = 3000,
                    kDiscoverPatienceMs = 20000, kDiscoverRetryMs = 30000, kQuietGiveUpMs = 300000;
constexpr float kNudgeStep = 0.25f;  // a quarter of a degree, or 14 degrees for a moment on a radian value
constexpr double kNudgeSeen = 0.05, kNudgeStill = 0.02;
constexpr size_t kNudgeBatch = 16, kNudgeMaxSlots = 600;
constexpr int kMaxDiscoveries = 12;
// The automatic searches (by itself, after a rejected or undone view) only run in the first minutes of play; later the
// probe key is the way. Only time in gameplay counts (not menus, loading or alt-tab).
constexpr ULONGLONG kAutoWindowMs = 120000;
constexpr ULONGLONG kMenuCheckMs = 1000, kMenuPatienceMs = 60000;
constexpr int kDeepObjects = 32;  // per container, and each class once

const char kSettings[] =
    "; 1 = find the camera of a game whose camera ignores the gyro: in gameplay, turn the camera with the\r\n"
    ";     RIGHT STICK or MOUSE (more than 30 degrees, also up and down) and stop, 3 times (the gyro is paused meanwhile),\r\n"
    ";     then don't touch the controller while it tests what it found. A verified camera goes into the game's profile\r\n"
    ";     (Gyro4All\\profiles\\<game exe>.ini) and applies at once. Gyro4All then sets this back to 0 by itself.\r\n"
    ";     Results: Gyro4All\\Gyro4All-probe.log. F7 writes the scene's cameras and look-named values there at any time.\r\n"
    "Probe=0\r\n"
    "; The same during the game, without restarting:\r\n"
    ";   ProbeKey: starts the probe right where you are, and cancels it. F1..F24, optionally with Ctrl+, Shift+ or Alt+;\r\n"
    ";     empty = no key.\r\n"
    ";   AutoProbe=1: a game with no profile gets its camera looked for by itself, in gameplay: the gyro waits a few seconds\r\n"
    ";     at most, once per game, and what is found goes into its profile. If that fails it goes on watching while you play,\r\n"
    ";     and when the camera stops answering the gyro the probe starts by itself. 0 = off.\r\n"
    "ProbeKey=Ctrl+F7\r\n"
    "AutoProbe=1\r\n";

const char kHeader[] =
    "Gyro4All " GYRO4ALL_VERSION " Unity camera probe (recreated every game session)\n"
    "\n"
    "  cam  <hierarchy> ...          a camera of the scene, from its own object up to its root\n"
    "  val  <class>: <field>=<value> a script's float whose name looks like a view angle; (camera rig) = on a camera's object\n"
    "                                or one of its parents\n"
    "  yaw/pitch <class> <path> ...  a float that followed the camera: r = correlation, per degree = its units for one\n"
    "                                degree. These become ObjectType=, PitchPath=/YawPath= and Pitch/YawPerDegree= once a\n"
    "                                live test verifies them (ADVANCED.md, \"Unity camera profiles\").\n"
    "\n";

struct Api {
    void* camera_class = nullptr;
    void* get_main = nullptr;
    void* get_game_object = nullptr;
    void* get_transform = nullptr;
    void* get_parent = nullptr;
    void* get_rotation = nullptr;
    void* set_rotation = nullptr;
    void* get_orthographic = nullptr;
    void* get_name = nullptr;
    bool ready = false;
} g_api;

struct Owner {
    void* object = nullptr;
    uintptr_t handle = 0;  // strong: its memory stays valid while the probe holds addresses in it
    std::string klass;
};

struct Slot {
    uintptr_t address = 0;
    uint32_t owner = 0, path = 0;
    float last = 0;
    SlotStats stats;
    double nudged_yaw = 0, nudged_pitch = 0;  // units per degree from a nudge (0 = none): stands in for the correlation's
    double tested_yaw = 0, tested_pitch = 0;  // from the live test: what the value's own turn did, better than either
};

struct Camera {
    void* object = nullptr;
    uintptr_t handle = 0;
    double yaw = 0, moved = 0;
    double pitch = 0;  // with yaw: where the nudges measure from
};

struct NudgeItem {
    uint32_t slot = 0;
    float base = 0, written = 0;
};

struct Nudge {
    enum Stage { Settle, Watch } stage = Settle;
    std::vector<std::vector<uint32_t>> groups;  // what is left, the next at the back
    std::vector<NudgeItem> live;                // what is written now
    std::vector<uint32_t> group;                // the same slots, to split it
    std::vector<uint32_t> yaw, pitch;           // what turned the camera, not tested yet
    int found_yaw = -1, found_pitch = -1;       // already verified: the search goes on only for the other one
    ULONGLONG deadline = 0, at = 0, still_since = 0, moved_at = 0;
    int frames = 0;
    double last_seen = 0;
};

// A Transform's turn tested live, for a look state with no float.
struct BodyTest {
    enum Stage { Settle, Measure, Back } stage = Settle;
    bool pitch_stage = false;  // the camera's own pitch, after the body's yaw, when no float has it
    bool need_pitch = false;
    int pitch_slot = -1;
    int parent = 0;
    double base_yaw = 0, base_pitch = 0, asked = 0, last_yaw = 0, last_pitch = 0;
    bool passed = false;
    ULONGLONG at = 0, ok_since = 0, still_since = 0;
};

struct Test {
    enum Stage { Settle, Measure, Back } stage = Settle;
    bool pitch_axis = false;
    size_t index = 0;
    std::vector<uint32_t> yaw, pitch;
    int found_yaw = -1, found_pitch = -1;
    double base_yaw = 0, base_pitch = 0, asked = 0, per_degree = 0;
    double last_yaw = 0, last_pitch = 0;
    float base_value = 0, written = 0;
    bool passed = false;
    ULONGLONG at = 0, ok_since = 0, still_since = 0, next_notice = 0;
};

struct State {
    enum Phase { Off, Scan, Nudging, Pick, Capture, Testing, Body } phase = Off;
    bool automatic = false;
    bool discovery = false;  // the search with no input (Nudging)
    bool quiet = false;      // no notices and no asking the player for anything
    ULONGLONG started_at = 0, next_try = 0, next_sample = 0, last_look = 0, still_since = 0, look_ms = 0, last_tick = 0;
    std::vector<Owner> owners;
    std::vector<std::string> paths;
    std::vector<Slot> slots;
    std::vector<Camera> cameras;
    void* camera = nullptr;
    double yaw = 0, pitch = 0, turned = 0, pitch_total = 0, yaw2 = 0, pitch2 = 0;
    int turns = 0, scan_tries = 0;
    Test test;
    Nudge nudge;
    BodyTest body;
} g;

bool g_was_wanted = false, g_start_wanted = false;
bool g_auto_setting = true;
int g_auto_starts = 0;
ULONGLONG g_next_auto = 0;
ULONGLONG g_play_ms = 0, g_play_last = 0;  // gameplay time seen by probe_discover
bool auto_window_open() { return g_play_ms < kAutoWindowMs; }
ProbeKey g_key;
bool g_key_down = false, g_f7_down = false;
// probe_discover.
void* g_discover_camera = nullptr;  // the camera being watched
void* g_discover_tried = nullptr;   // the last one searched
void* g_discover_retried = nullptr;  // the last one searched again after a while: only once
void* g_discover_turned = nullptr;   // the last one the player was seen turning: searched again, only once
ViewStillness g_discover_view;
ULONGLONG g_discover_since = 0, g_discover_next = 0;
int g_discoveries = 0;
bool g_discovery_done = false;
bool g_menu_on_screen = false;
ULONGLONG g_menu_next = 0, g_menu_since = 0, g_menu_left_at = 0;
void* g_menu_camera = nullptr;  // the camera whose patience ran out: it has the gyro now

bool resolve_api(Runtime& rt) {
    if (g_api.ready) return true;
    void* component = rt.find_class("UnityEngine", "Component");
    void* transform = rt.find_class("UnityEngine", "Transform");
    g_api.camera_class = rt.find_class("UnityEngine", "Camera");
    g_api.get_main = rt.find_method(g_api.camera_class, "get_main", 0);
    g_api.get_game_object = rt.find_method(component, "get_gameObject", 0);
    g_api.get_transform = rt.find_method(component, "get_transform", 0);
    g_api.get_parent = rt.find_method(transform, "get_parent", 0);
    g_api.get_rotation = rt.find_method(transform, "get_rotation", 0);
    g_api.set_rotation = rt.find_method(transform, "set_rotation", 1);
    g_api.get_orthographic = rt.find_method(g_api.camera_class, "get_orthographic", 0);
    g_api.get_name = rt.find_method(rt.find_class("UnityEngine", "Object"), "get_name", 0);
    g_api.ready = g_api.camera_class && g_api.get_transform && g_api.get_rotation;
    return g_api.ready;
}

bool open_log() {
    static bool opened = false, tried = false;
    if (tried) return opened;
    tried = true;
    const std::wstring path = settings::mod_dir() + L"\\Gyro4All-probe.log";
    opened = probe_log_open(path, kHeader);
    log("probe: results in %s", opened ? narrow(path).c_str() : "Gyro4All.log (probe file failed)");
    return opened;
}

bool look_input() { return host::mouse_moving() || (host::right_stick_moving() && !host::stick_hidden()); }

bool read_angles(Runtime& rt, void* camera, ViewAngles& angles) {
    if (!rt.alive(camera)) return false;
    void* transform = rt.invoke(g_api.get_transform, camera);
    NativeQuaternion native{};
    if (!transform || !rt.returned(rt.invoke(g_api.get_rotation, transform), native)) return false;
    const Quaternion q = from_native(native);
    if (!finite(q)) return false;
    angles = view_angles(q);
    return true;
}

std::string narrow_string(Runtime& rt, void* text) {
    int length = 0;
    const wchar_t* chars = rt.string_chars(text, length);
    return chars ? narrow(std::wstring(chars, size_t(length))) : std::string();
}

// "<game object> < <parent> < <grandparent>" of a component (its rig, what really owns the camera).
std::string hierarchy(Runtime& rt, void* component, std::unordered_set<void*>* chain = nullptr) {
    std::string text = narrow_string(rt, rt.invoke(g_api.get_name, rt.invoke(g_api.get_game_object, component)));
    void* transform = rt.invoke(g_api.get_transform, component);
    for (int depth = 0; transform && depth < 16; ++depth) {
        if (chain) chain->insert(transform);
        transform = g_api.get_parent ? rt.invoke(g_api.get_parent, transform) : nullptr;
        if (transform && depth < 8)
            text += " < " + narrow_string(rt, rt.invoke(g_api.get_name, rt.invoke(g_api.get_game_object, transform)));
    }
    return text;
}

// --- Scan ---

// Unity's and its usual libraries' own scripts: nothing there is a game's look state. Cinemachine stays.
bool skipped_namespace(const std::string& name_space) {
    for (const char* prefix : {"UnityEngine", "TMPro", "Rewired", "System", "Mono."})
        if (name_space.starts_with(prefix)) return true;
    return false;
}

bool is_unity_object(Runtime& rt, void* klass) {
    for (void* c = klass; c; c = rt.class_parent(c))
        if (std::string(rt.class_name(c)) == "Object" && std::string(rt.class_namespace(c)) == "UnityEngine") return true;
    return false;
}

struct Scanner {
    std::vector<std::string>* paths = &g.paths;  // where the floats found go: the probe's, or the pitch watch's
    std::vector<Slot>* slots = &g.slots;
    std::unordered_map<std::string, uint32_t> path_ids;
    std::unordered_set<uintptr_t> seen;
    std::unordered_set<uintptr_t> seen_objects;
};

// Transforms of the cameras and everything they hang from.
std::unordered_set<void*> camera_rig(Runtime& rt) {
    std::unordered_set<void*> rig;
    for (const Camera& camera : g.cameras) {
        void* transform = rt.invoke(g_api.get_transform, camera.object);
        for (int depth = 0; transform && depth < 16; ++depth) {
            rig.insert(transform);
            transform = rt.invoke(g_api.get_parent, transform);
        }
    }
    return rig;
}

bool is_rotation_block(Runtime& rt, void* klass) {
    if (!klass || std::string(rt.class_namespace(klass)) != "UnityEngine") return false;
    const std::string name = rt.class_name(klass);
    return name == "Quaternion" || name == "Matrix4x4";
}

// How far from its script a float is looked for: one object reference for most; a camera's own scripts keep state further
// away (Risk of Rain 2: rig -> camera mode -> dictionary -> instance data -> pair of floats), and through containers.
constexpr int kNearDepth = 1, kDeepDepth = 3, kValueDepth = 2;

void add_fields(Runtime& rt, Scanner& scan, uint32_t owner, uintptr_t base, void* klass, const std::string& prefix, int depth, int values,
                bool deep) {
    for (void* c = klass; c; c = rt.class_parent(c)) {
        const std::string name_space = rt.class_namespace(c);
        if (name_space.starts_with("System") ||
            (name_space == "UnityEngine" && std::string(rt.class_name(c)) == "MonoBehaviour"))
            return;
        void* iterator = nullptr;
        while (void* field = rt.next_field(c, &iterator)) {
            if (scan.slots->size() >= kMaxSlots) return;
            if (rt.field_is_static(field)) continue;
            const uintptr_t address = base + rt.field_offset(field);
            const std::string path = prefix + rt.field_name(field);
            const int type = rt.field_type(field);
            if (type == kTypeR4) {
                float value = 0;
                if (!mem::rd(address, value) || !std::isfinite(value) || !scan.seen.insert(address).second) continue;
                const auto id = scan.path_ids.try_emplace(path, uint32_t(scan.paths->size()));
                if (id.second) scan.paths->push_back(path);
                scan.slots->push_back({address, owner, id.first->second, value, {}});
            } else if (type == kTypeValueType && values < kValueDepth) {
                // A rotation's four numbers aren't four view axes: writing one of them does nothing that lasts.
                void* value_class = rt.field_class(field);
                if (!is_rotation_block(rt, value_class))
                    add_fields(rt, scan, owner, address - kObjectHeader, value_class, path + ".", depth, values + 1, deep);
            } else if (depth < (deep ? kDeepDepth : kNearDepth) && holds_object(rt, type, rt.field_class(field))) {
                const uintptr_t object = mem::rdp(address);
                void* value_class = object ? rt.object_class(reinterpret_cast<void*>(object)) : nullptr;
                if (!value_class || !scan.seen_objects.insert(object).second) continue;
                if (is_container(rt, value_class)) {
                    if (!deep) continue;
                    std::unordered_set<void*> classes;  // the first of each class: what name[Class] will find again
                    for (void* element : container_objects(rt, reinterpret_cast<void*>(object), size_t(kDeepObjects))) {
                        void* element_class = rt.object_class(element);
                        if (!classes.insert(element_class).second || is_unity_object(rt, element_class) ||
                            skipped_namespace(rt.class_namespace(element_class)))
                            continue;
                        if (scan.seen_objects.insert(reinterpret_cast<uintptr_t>(element)).second)
                            add_fields(rt, scan, owner, reinterpret_cast<uintptr_t>(element), element_class,
                                       path + "[" + rt.class_name(element_class) + "].", depth + 1, 0, deep);
                    }
                } else if (!is_unity_object(rt, value_class) && !skipped_namespace(rt.class_namespace(value_class))) {
                    add_fields(rt, scan, owner, object, value_class, path + ".", depth + 1, 0, deep);
                }
            }
        }
    }
}

void clear_scene() {
    g.owners.clear();
    g.paths.clear();
    g.slots.clear();
    g.cameras.clear();
    g.camera = nullptr;
}

void release_scene(Runtime& rt) {
    for (const Owner& owner : g.owners) rt.free_handle(owner.handle);
    for (const Camera& camera : g.cameras) rt.free_handle(camera.handle);
    clear_scene();
}

void list_cameras(Runtime& rt) {
    void* array = rt.find_objects_of_type(g_api.camera_class);
    const size_t count = rt.array_length(array);
    for (size_t i = 0; i < count && g.cameras.size() < size_t(kMaxCameras); ++i) {
        void** element = static_cast<void**>(rt.array_element(array, i));
        void* camera = element ? *element : nullptr;
        if (!camera || !rt.alive(camera)) continue;
        uint8_t ortho = 0;
        if (g_api.get_orthographic) rt.returned(rt.invoke(g_api.get_orthographic, camera), ortho);
        if (ortho) continue;
        ViewAngles angles;
        if (read_angles(rt, camera, angles)) g.cameras.push_back({camera, rt.new_handle(camera, false), angles.yaw, 0});
    }
}

void scan_scripts(Runtime& rt) {
    Scanner scan;
    std::unordered_map<void*, int> instances;
    const std::unordered_set<void*> rig = camera_rig(rt);
    void* array = rt.find_objects_of_type(rt.find_class("UnityEngine", "MonoBehaviour"));
    const size_t count = rt.array_length(array);
    for (size_t i = 0; i < count && g.slots.size() < kMaxSlots; ++i) {
        void** element = static_cast<void**>(rt.array_element(array, i));
        void* object = element ? *element : nullptr;
        void* klass = object ? rt.object_class(object) : nullptr;
        if (!klass || skipped_namespace(rt.class_namespace(klass)) || !rt.alive(object)) continue;
        if (++instances[klass] > kMaxInstancesPerClass) continue;
        g.owners.push_back({object, rt.new_handle(object, false), rt.class_full_name(klass)});
        const bool deep = camera_class_name(g.owners.back().klass) || rig.count(rt.invoke(g_api.get_transform, object)) != 0;
        add_fields(rt, scan, uint32_t(g.owners.size() - 1), reinterpret_cast<uintptr_t>(object), klass, "", 0, 0, deep);
    }
    log("probe: %zu active scripts, %zu of them scanned: %zu floats", count, g.owners.size(), g.slots.size());
    probe_log("probe: %zu active scripts, %zu of them scanned: %zu floats", count, g.owners.size(), g.slots.size());
}

bool look_named(const std::string& path) {
    std::string lower(path.size(), '\0');
    std::transform(path.begin(), path.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* needle : {"pitch", "yaw", "look", "aim", "tilt", "pan", "rot", "angle", "euler", "axis", "camera", "view"})
        if (lower.find(needle) != std::string::npos) return true;
    return false;
}

void dump(Runtime& rt) {
    open_log();
    std::unordered_set<void*> rig;
    void* main = g_api.get_main ? rt.invoke(g_api.get_main, nullptr) : nullptr;
    void* array = rt.find_objects_of_type(g_api.camera_class);
    const size_t count = rt.array_length(array);
    probe_log("probe: %zu camera(s)%s", count, main ? "" : ", Camera.main is null right now");
    for (size_t i = 0; i < count; ++i) {
        void** element = static_cast<void**>(rt.array_element(array, i));
        void* camera = element ? *element : nullptr;
        if (!camera || !rt.alive(camera)) continue;
        ViewAngles angles;
        read_angles(rt, camera, angles);
        uint8_t ortho = 0;
        if (g_api.get_orthographic) rt.returned(rt.invoke(g_api.get_orthographic, camera), ortho);
        const std::string name = g_api.get_game_object && g_api.get_name ? hierarchy(rt, camera, &rig) : "";
        probe_log("cam  %s | main=%d ortho=%d view=(pitch %.1f, yaw %.1f)", name.c_str(), camera == main, int(ortho),
                  angles.pitch, angles.yaw);
    }
    std::vector<std::string> lines(g.owners.size());
    for (const Slot& slot : g.slots) {
        if (!look_named(g.paths[slot.path])) continue;
        float value = 0;
        if (mem::rd(slot.address, value)) lines[slot.owner] += std::format(" {}={:.3f}", g.paths[slot.path], value);
    }
    int logged = 0;
    for (int on_rig = 1; on_rig >= 0; --on_rig)
        for (size_t i = 0; i < g.owners.size() && logged < kMaxDumpLines; ++i) {
            if (lines[i].empty() || !rt.alive(g.owners[i].object)) continue;
            const bool on = rig.count(rt.invoke(g_api.get_transform, g.owners[i].object)) != 0;
            if (on != (on_rig != 0)) continue;
            probe_log("val  %s%s:%s", g.owners[i].klass.c_str(), on ? " (camera rig)" : "", lines[i].c_str());
            ++logged;
        }
    if (logged >= kMaxDumpLines) probe_log("probe: dump capped at %d lines", kMaxDumpLines);
    log("probe: the scene's cameras and look-named values written to the probe log");
}

// --- Capture ---

void sample_slots(double dyaw, double dpitch, bool count) {
    for (Slot& slot : g.slots) {
        float value = 0;
        if (!mem::rd(slot.address, value) || !std::isfinite(value)) continue;
        double change = double(value) - slot.last;
        if (std::abs(change) > 180) change = wrap180(change);  // an angle kept as 0..360
        slot.last = value;
        if (count) slot.stats.add(change, dyaw, dpitch);
    }
}

std::vector<uint32_t> rank(bool pitch_axis) {
    struct Scored {
        uint32_t slot;
        double score;
    };
    std::vector<Scored> scored;
    for (size_t i = 0; i < g.slots.size(); ++i) {
        const SlotStats& s = g.slots[i].stats;
        const AxisFit yaw = fit_axis(s.value_yaw, s.value2, g.yaw2), pitch = fit_axis(s.value_pitch, s.value2, g.pitch2);
        const double score = candidate_score(pitch_axis ? pitch : yaw, pitch_axis ? yaw : pitch, s.moved);
        if (score > 0) scored.push_back({uint32_t(i), score});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    if (scored.size() > size_t(kMaxCandidates)) scored.resize(kMaxCandidates);
    std::vector<uint32_t> out;
    for (const Scored& s : scored) {
        out.push_back(s.slot);
        const SlotStats& stats = g.slots[s.slot].stats;
        const AxisFit fit = fit_axis(pitch_axis ? stats.value_pitch : stats.value_yaw, stats.value2, pitch_axis ? g.pitch2 : g.yaw2);
        probe_log("%-5s %s %s r=%.3f per degree %.5f", pitch_axis ? "pitch" : "yaw", g.owners[g.slots[s.slot].owner].klass.c_str(),
                  g.paths[g.slots[s.slot].path].c_str(), fit.correlation, fit.per_degree);
    }
    return out;
}

double slot_per_degree(uint32_t index, bool pitch_axis) {
    const double tested = pitch_axis ? g.slots[index].tested_pitch : g.slots[index].tested_yaw;
    if (tested != 0) return tested;
    const double nudged = pitch_axis ? g.slots[index].nudged_pitch : g.slots[index].nudged_yaw;
    if (nudged != 0) return nudged;
    const SlotStats& s = g.slots[index].stats;
    return fit_axis(pitch_axis ? s.value_pitch : s.value_yaw, s.value2, pitch_axis ? g.pitch2 : g.yaw2).per_degree;
}

// --- End ---

void end(Runtime& rt, const char* why) {
    release_scene(rt);
    log("probe: ended (%s)", why);
    probe_log("probe: ended (%s)", why);
    g.phase = State::Off;
    g_next_auto = GetTickCount64() + 30000;
}

// The field of the object holding this camera, for CameraField=. Its own fields only.
std::string camera_field(Runtime& rt, void* object, void* camera) {
    for (void* c = rt.object_class(object); c; c = rt.class_parent(c)) {
        if (std::string(rt.class_namespace(c)).starts_with("UnityEngine")) break;
        void* iterator = nullptr;
        while (void* field = rt.next_field(c, &iterator))
            if (!rt.field_is_static(field) && rt.field_type(field) == kTypeClass &&
                mem::rdp(reinterpret_cast<uintptr_t>(object) + rt.field_offset(field)) == reinterpret_cast<uintptr_t>(camera))
                return rt.field_name(field);
    }
    return {};
}

std::wstring widen(const std::string& text) {
    std::wstring wide(text.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), int(wide.size()));
    wide.resize(n > 0 ? size_t(n - 1) : 0);
    return wide;
}

// Probe=0 where it is on: the probe's job is done.
void probe_off() {
    std::wstring value;
    if (settings::read(settings::settings_path(), L"Probe", value))
        WritePrivateProfileStringW(settings::kSection, L"Probe", L"0", settings::settings_path().c_str());
    if (settings::read(settings::profile_path(), L"Probe", value))
        WritePrivateProfileStringW(settings::kSection, L"Probe", L"0", settings::profile_path().c_str());
    if (!settings::profile_string(L"Unity", L"Probe").empty())
        WritePrivateProfileStringW(L"Unity", L"Probe", L"0", settings::profile_path().c_str());
}

bool start(Runtime& rt, bool automatic, const char* why);
void restart_scan(Runtime& rt, const char* why);

void* parent_transform(Runtime& rt, void* camera, int levels) {
    void* transform = rt.invoke(g_api.get_transform, camera);
    for (int i = 0; i < levels && transform; ++i) transform = rt.invoke(g_api.get_parent, transform);
    return transform;
}

// What the camera hangs from that the game moves as the player's body, levels above it. 0 = nothing.
int find_body_parent(Runtime& rt, void* camera) {
    std::unordered_set<void*> bodies;
    for (const char* type : {"CharacterController", "Rigidbody"}) {
        void* array = rt.find_objects_of_type(rt.find_class("UnityEngine", type));
        const size_t count = rt.array_length(array);
        for (size_t i = 0; i < count; ++i) {
            void** element = static_cast<void**>(rt.array_element(array, i));
            void* object = element ? *element : nullptr;
            if (object && rt.alive(object)) bodies.insert(rt.invoke(g_api.get_transform, object));
        }
    }
    void* transform = rt.invoke(g_api.get_transform, camera);
    for (int level = 1; level <= 16 && transform; ++level) {
        transform = rt.invoke(g_api.get_parent, transform);
        if (transform && bodies.count(transform)) return level;
    }
    return 0;
}

// Rejected= in the profile: what the player's own turns showed isn't the look state.
std::string rejected_list() { return ";" + settings::profile_string(L"Unity", L"Rejected") + ";"; }

bool in_list(const std::string& list, const std::string& what) { return list.find(";" + what + ";") != std::string::npos; }

void write_result(Runtime& rt, ProbeResult result) {
    // A profile with its own [Unity] section is never overwritten.
    const bool suggested = !settings::profile_string(L"Unity", L"ObjectType").empty();
    const std::wstring dir = settings::mod_dir() + L"\\profiles";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = suggested ? dir + L"\\" + settings::exe_stem() + L".suggested.ini" : settings::profile_path();
    bool written = true;
    for (const auto& [key, value] : profile_keys(result)) {
        written = WritePrivateProfileStringW(L"Unity", widen(key).c_str(), widen(value).c_str(), path.c_str()) != 0 && written;
        probe_log("probe: %s=%s", key.c_str(), value.c_str());
    }
    log("probe: camera VERIFIED: pitch %s, yaw %s -> %s",
        result.pitch_class.empty() ? "on the camera's Transform" : (result.pitch_class + "." + result.pitch_path).c_str(),
        result.yaw_parent > 0 ? std::format("parent {}", result.yaw_parent).c_str() : (result.yaw_class + "." + result.yaw_path).c_str(),
        written ? narrow(path).c_str() : "the profile couldn't be written");
    if (written) probe_off();
    notify::show(!written    ? L"Gyro4All probe: camera found, but the profile couldn't be written.\nSee Gyro4All\\Gyro4All.log."
                 : suggested ? L"Gyro4All probe: camera found and VERIFIED.\nThis game already has a Unity profile, so it went to "
                               L"Gyro4All\\profiles\\" + settings::exe_stem() + L".suggested.ini."
                             : L"Gyro4All: camera found and VERIFIED, added to this game's profile.\nThe gyro turns it from now on.",
                 15);
    end(rt, "camera verified");
    if (written) g_discovery_done = true;
}

// Nothing to write. The search with no input goes on watching the player's turns, quietly.
void give_up(Runtime& rt, const char* what) {
    log("probe: no camera: %s (see Gyro4All-probe.log)", what);
    probe_log("probe: no camera: %s. The game may turn the camera from native state.", what);
    const bool discovery = g.discovery, quiet = g.quiet;
    if (discovery) g_discover_tried = g_discover_camera;
    if (!quiet)
        notify::show(L"Gyro4All probe: no value in this game's scripts turns its camera, so no profile was written.\n"
                     L"See Gyro4All\\Gyro4All-probe.log.",
                     15);
    end(rt, what);
    if (discovery && start(rt, true, "watching the player's turns")) g.quiet = true;
}

bool start_body(Runtime& rt, ULONGLONG now, bool need_pitch);

void finish(Runtime& rt) {
    const Test& t = g.test;
    const ULONGLONG tick = GetTickCount64();
    if (g.discovery && (t.found_yaw < 0 || t.found_pitch < 0) && !g.nudge.groups.empty() && tick < g.nudge.deadline) {
        g.nudge.found_yaw = t.found_yaw, g.nudge.found_pitch = t.found_pitch;
        g.nudge.stage = Nudge::Settle;
        g.nudge.still_since = tick;
        g.phase = State::Nudging;
        return;
    }
    if (t.found_yaw < 0) {
        if (start_body(rt, GetTickCount64(), t.found_pitch < 0)) return;
        return give_up(rt, t.found_pitch >= 0 ? "pitch was found, but nothing turns the yaw" : "nothing turned the camera");
    }
    if (t.found_pitch < 0) return give_up(rt, "yaw was found, but no float turns the pitch");
    const Slot& yaw = g.slots[size_t(t.found_yaw)];
    const Slot& pitch = g.slots[size_t(t.found_pitch)];
    ProbeResult result;
    result.pitch_class = g.owners[pitch.owner].klass;
    result.pitch_path = g.paths[pitch.path];
    result.yaw_class = g.owners[yaw.owner].klass;
    result.yaw_path = g.paths[yaw.path];
    result.pitch_per_degree = slot_per_degree(uint32_t(t.found_pitch), true);
    result.yaw_per_degree = slot_per_degree(uint32_t(t.found_yaw), false);
    result.camera_field = camera_field(rt, g.owners[pitch.owner].object, g.camera);
    result.automatic = g.quiet;
    write_result(rt, std::move(result));
}

// --- Live test ---

bool slot_usable(Runtime& rt, const Slot& slot) { return rt.alive(g.owners[slot.owner].object); }

void restore_value(const Slot& slot) {
    float value = 0;
    // Left alone if the game rewrote it meanwhile: the original would be stale.
    if (mem::rd(slot.address, value) && std::abs(double(value) - g.test.written) <= std::abs(g.test.per_degree) * 1.5)
        mem::guarded_store(slot.address, &g.test.base_value, sizeof(float));
}

void next_candidate(Runtime& rt) {
    Test& t = g.test;
    ++t.index;
    t.stage = Test::Settle;
    if (t.index >= (t.pitch_axis ? t.pitch.size() : t.yaw.size())) {
        if (t.pitch_axis) return finish(rt);
        t.pitch_axis = true;
        t.index = 0;
        if (t.pitch.empty()) finish(rt);
    }
}

void test_step(Runtime& rt, ULONGLONG now) {
    Test& t = g.test;
    ViewAngles angles;
    if (!read_angles(rt, g.camera, angles)) return end(rt, "the camera went away during the live test");
    if (std::abs(wrap180(angles.yaw - t.last_yaw)) + std::abs(angles.pitch - t.last_pitch) > 0.02) t.still_since = now;
    t.last_yaw = angles.yaw, t.last_pitch = angles.pitch;
    const double moved_yaw = wrap180(angles.yaw - t.base_yaw), moved_pitch = angles.pitch - t.base_pitch;
    const double moved = t.pitch_axis ? moved_pitch : moved_yaw, other = t.pitch_axis ? moved_yaw : moved_pitch;
    const bool touched = look_input();
    const std::vector<uint32_t>& list = t.pitch_axis ? t.pitch : t.yaw;
    if (t.index >= list.size()) return next_candidate(rt);
    const uint32_t index = list[t.index];
    const Slot& slot = g.slots[index];
    switch (t.stage) {
    case Test::Settle: {
        if (touched) {
            t.still_since = now;
            if (now >= t.next_notice && !g.quiet) {
                t.next_notice = now + 5000;
                notify::show(L"Gyro4All probe: testing. DON'T TOUCH THE CONTROLLER OR THE MOUSE.", 4);
            }
            return;
        }
        if (now - t.still_since < kRestMs) return;
        float value = 0;
        if (!slot_usable(rt, slot) || !mem::rd(slot.address, value) || !std::isfinite(value)) return next_candidate(rt);
        t.base_yaw = angles.yaw, t.base_pitch = angles.pitch;
        t.asked = t.pitch_axis && angles.pitch > 0 ? -kTestTurn : kTestTurn;  // toward the horizon: away from the limits
        t.per_degree = slot_per_degree(index, t.pitch_axis);
        t.base_value = value;
        t.written = float(double(value) + t.per_degree * t.asked);
        mem::guarded_store(slot.address, &t.written, sizeof(float));
        t.stage = Test::Measure;
        t.at = now, t.ok_since = 0, t.passed = false;
        return;
    }
    case Test::Measure: {
        if (touched) {
            restore_value(slot);
            t.stage = Test::Settle;
            t.still_since = now;
            return;
        }
        float value = 0;
        // The game keeps the value (an input accumulator it resets would turn the camera once and be gone).
        const bool kept = mem::rd(slot.address, value) && std::abs(double(value) - t.written) <= std::abs(t.per_degree);
        if (kept && turn_followed(t.asked, moved, other)) {
            if (!t.ok_since) t.ok_since = now;
            if (now - t.ok_since >= 200) t.passed = true;
        } else {
            t.ok_since = 0;
        }
        // The correlation's scale runs off with a camera that lags the value (Viewfinder's yaw: 0.78 for a value in degrees,
        // which passed the test and made every yaw 22% short). What this write did is the measure.
        if (t.passed && std::abs(moved) > 0.5)
            (t.pitch_axis ? g.slots[index].tested_pitch : g.slots[index].tested_yaw) = (double(t.written) - t.base_value) / moved;
        if (t.passed || now - t.at >= 1000) {
            probe_log("probe: test %s %s %s: asked %.1f, the camera moved %.1f (other axis %.1f), value %s: %s",
                      t.pitch_axis ? "pitch" : "yaw", g.owners[slot.owner].klass.c_str(), g.paths[slot.path].c_str(), t.asked, moved,
                      other, kept ? "kept" : "rewritten by the game", t.passed ? "turned" : "failed");
            restore_value(slot);
            t.stage = Test::Back;
            t.at = now;
        }
        return;
    }
    case Test::Back:
        if (now - t.at < kRestMs) return;
        if (!t.passed) return next_candidate(rt);
        if (turn_followed(0, moved, other)) {
            probe_log("probe: verified %s: %s %s", t.pitch_axis ? "pitch" : "yaw", g.owners[slot.owner].klass.c_str(),
                      g.paths[slot.path].c_str());
            (t.pitch_axis ? t.found_pitch : t.found_yaw) = int(index);
            if (t.pitch_axis) return finish(rt);
            t.pitch_axis = true;
            t.index = 0;
            t.stage = Test::Settle;
            if (t.pitch.empty()) finish(rt);
            return;
        }
        if (now - t.at >= 1000) next_candidate(rt);
        return;
    }
}

void begin_test(Runtime& rt, ULONGLONG now, std::vector<uint32_t> yaw, std::vector<uint32_t> pitch) {
    Test t;
    t.yaw = std::move(yaw);
    t.pitch = std::move(pitch);
    t.still_since = now;
    g.test = std::move(t);
    g.phase = State::Testing;
    ViewAngles angles;
    if (read_angles(rt, g.camera, angles)) g.test.last_yaw = angles.yaw, g.test.last_pitch = angles.pitch;
    log("probe: %zu yaw and %zu pitch candidates: testing them live", g.test.yaw.size(), g.test.pitch.size());
    if (!g.quiet)
        notify::show(L"Gyro4All probe: testing what it found.\nDON'T TOUCH THE CONTROLLER OR THE MOUSE for a few seconds.", 8);
}

// A yaw with no float (a body's Transform) can still be found once pitch is.
bool start_test(Runtime& rt, ULONGLONG now) {
    std::vector<uint32_t> yaw = rank(false), pitch = rank(true);
    if (pitch.empty()) return false;
    begin_test(rt, now, std::move(yaw), std::move(pitch));
    return true;
}

// --- The search with no input ---

std::vector<std::vector<uint32_t>> nudge_groups(Runtime& rt) {
    const std::unordered_set<void*> rig = camera_rig(rt);
    std::vector<char> on_rig(g.owners.size());
    for (size_t i = 0; i < g.owners.size(); ++i)
        on_rig[i] = rt.alive(g.owners[i].object) && rig.count(rt.invoke(g_api.get_transform, g.owners[i].object)) != 0;
    struct Scored {
        uint32_t slot;
        int score;
    };
    std::vector<Scored> scored;
    const std::string not_these = rejected_list();
    for (size_t i = 0; i < g.slots.size(); ++i) {
        const Slot& slot = g.slots[i];
        const std::string& klass = g.owners[slot.owner].klass;
        const std::string& path = g.paths[slot.path];
        const int score = discovery_score(klass, path, on_rig[slot.owner] != 0);
        if (score > 0 && !in_list(not_these, klass + " " + path)) scored.push_back({uint32_t(i), score});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    if (scored.size() > kNudgeMaxSlots) scored.resize(kNudgeMaxSlots);
    log("probe: %zu of the floats look like a view angle: nudging them", scored.size());
    probe_log("probe: %zu of the floats look like a view angle: nudging them", scored.size());
    std::vector<std::vector<uint32_t>> groups;
    for (size_t i = 0; i < scored.size(); i += kNudgeBatch) {
        std::vector<uint32_t> group;
        for (size_t j = i; j < std::min(i + kNudgeBatch, scored.size()); ++j) group.push_back(scored[j].slot);
        groups.push_back(std::move(group));
    }
    std::reverse(groups.begin(), groups.end());
    return groups;
}

void nudge_restore() {
    Nudge& n = g.nudge;
    for (const NudgeItem& item : n.live) {
        float value = 0;
        // Left alone if the game rewrote it meanwhile: the original would be stale.
        if (mem::rd(g.slots[item.slot].address, value) && std::abs(double(value) - item.written) < 1e-3)
            mem::guarded_store(g.slots[item.slot].address, &item.base, sizeof(float));
    }
    n.live.clear();
}

void nudge_done(Runtime& rt, ULONGLONG now) {
    nudge_restore();
    Nudge& n = g.nudge;
    probe_log("probe: nudges: %zu value(s) to test for the yaw, %zu for the pitch", n.yaw.size(), n.pitch.size());
    if (n.yaw.empty() && n.pitch.empty()) {
        if (!g.camera) {
            g.camera = rt.invoke(g_api.get_main, nullptr);
            if (!g.camera || !rt.alive(g.camera)) return give_up(rt, "nothing turned the camera");
        }
        g.test = {};
        g.test.found_yaw = n.found_yaw, g.test.found_pitch = n.found_pitch;
        return finish(rt);
    }
    std::vector<uint32_t> yaw = std::move(n.yaw), pitch = std::move(n.pitch);
    n.yaw.clear(), n.pitch.clear();
    begin_test(rt, now, std::move(yaw), std::move(pitch));
    g.test.found_yaw = n.found_yaw, g.test.found_pitch = n.found_pitch;
}

// The largest turn of any camera since it was last at rest.
struct CameraMoved {
    Camera* camera = nullptr;
    double yaw = 0, pitch = 0, size = 0;
};

bool cameras_moved(Runtime& rt, CameraMoved& out, bool rebase) {
    for (Camera& camera : g.cameras) {
        ViewAngles angles;
        if (!read_angles(rt, camera.object, angles)) return false;
        const double yaw = wrap180(angles.yaw - camera.yaw), pitch = angles.pitch - camera.pitch;
        const double size = std::abs(yaw) + std::abs(pitch);
        if (!out.camera || size > out.size) out = {&camera, yaw, pitch, size};
        if (rebase) camera.yaw = angles.yaw, camera.pitch = angles.pitch;
    }
    return !g.cameras.empty();
}

void nudge_step(Runtime& rt, ULONGLONG now) {
    Nudge& n = g.nudge;
    // Something for each axis is enough to test: each further nudge may turn the player. A failed test comes back here.
    const bool enough = (n.found_yaw >= 0 || !n.yaw.empty()) && (n.found_pitch >= 0 || !n.pitch.empty());
    if (n.stage == Nudge::Settle && (enough || n.groups.empty() || now >= n.deadline)) return nudge_done(rt, now);
    const bool touched = look_input();
    CameraMoved moved;
    if (!cameras_moved(rt, moved, false)) {
        nudge_restore();
        return restart_scan(rt, "a camera went away");
    }
    if (n.stage == Nudge::Settle) {
        if (touched || moved.size > kNudgeStill) {
            CameraMoved ignored;
            cameras_moved(rt, ignored, true);
            n.still_since = now;
            return;
        }
        if (now - n.still_since < kNudgeSettleMs) return;
        n.group = std::move(n.groups.back());
        n.groups.pop_back();
        for (uint32_t index : n.group) {
            const Slot& slot = g.slots[index];
            float value = 0;
            if (!rt.alive(g.owners[slot.owner].object) || !mem::rd(slot.address, value) || !std::isfinite(value)) continue;
            const NudgeItem item{index, value, value + kNudgeStep};
            if (mem::guarded_store(slot.address, &item.written, sizeof(float))) n.live.push_back(item);
        }
        if (n.live.empty()) return;
        n.stage = Nudge::Watch;
        n.at = now, n.moved_at = now, n.frames = 0, n.last_seen = 0;
        return;
    }
    if (touched || now >= n.deadline) {  // not this group's doing: it goes round again
        nudge_restore();
        if (now < n.deadline) n.groups.push_back(n.group);
        n.stage = Nudge::Settle;
        n.still_since = now;
        return;
    }
    ++n.frames;
    if (moved.size < kNudgeSeen) {
        if (now - n.at < kNudgeWatchMs || n.frames < 3) return;
        nudge_restore();
        n.stage = Nudge::Settle;
        return;
    }
    if (n.live.size() > 1) {  // something in here: half at a time
        const size_t half = n.live.size() / 2;
        std::vector<uint32_t> first, second;
        for (size_t i = 0; i < n.live.size(); ++i) (i < half ? first : second).push_back(n.live[i].slot);
        nudge_restore();
        n.groups.push_back(std::move(second));
        n.groups.push_back(std::move(first));
        n.stage = Nudge::Settle;
        n.still_since = now;
        return;
    }
    // One value turned it: wait for a smoothed camera to get there. Past any angle's nudge, it is out at once.
    if (std::abs(moved.size - n.last_seen) > 0.01) n.moved_at = now;
    n.last_seen = moved.size;
    if (moved.size <= kNudgeMaxTurn && now - n.moved_at < kNudgeWatchMs && now - n.at < kNudgeMaxMs) return;
    const NudgeItem item = n.live.front();
    Slot& slot = g.slots[item.slot];
    float value = 0;
    const bool kept = mem::rd(slot.address, value) && std::abs(double(value) - item.written) < 1e-3;
    const bool pitch_axis = std::abs(moved.pitch) > std::abs(moved.yaw);
    const double per_degree = nudge_per_degree(kNudgeStep, pitch_axis ? moved.pitch : moved.yaw, pitch_axis ? moved.yaw : moved.pitch);
    probe_log("probe: nudge %s %s: the camera turned yaw %.2f, pitch %.2f; value %s", g.owners[slot.owner].klass.c_str(),
              g.paths[slot.path].c_str(), moved.yaw, moved.pitch, kept ? "kept" : "rewritten by the game");
    const bool settled = now - n.moved_at >= kNudgeWatchMs;  // still turning: something the game keeps adding
    if (kept && settled && per_degree != 0 && (pitch_axis ? n.found_pitch : n.found_yaw) < 0) {
        (pitch_axis ? slot.nudged_pitch : slot.nudged_yaw) = per_degree;
        std::vector<uint32_t>& list = pitch_axis ? n.pitch : n.yaw;
        if (list.size() < size_t(kMaxCandidates)) list.push_back(item.slot);
        g.camera = moved.camera->object;
    }
    nudge_restore();
    n.stage = Nudge::Settle;
    n.still_since = now;
}

// --- A Transform's turn tested live ---

bool turn_transform(Runtime& rt, void* transform, double pitch_up, double yaw_right) {
    NativeQuaternion native{};
    if (!transform || !g_api.set_rotation || !rt.returned(rt.invoke(g_api.get_rotation, transform), native)) return false;
    const Quaternion turned = add_turn(from_native(native), pitch_up, yaw_right);
    if (!finite(turned)) return false;
    NativeQuaternion value = to_native(turned);
    void* arguments[] = {&value};
    rt.invoke(g_api.set_rotation, transform, arguments);
    return !rt.threw();
}

bool start_body(Runtime& rt, ULONGLONG now, bool need_pitch) {
    if (!g.camera || !g_api.get_parent || !g_api.set_rotation) return false;
    const int parent = find_body_parent(rt, g.camera);
    probe_log("probe: yaw has no float; the camera's body: %s", parent ? std::format("{} level(s) above it", parent).c_str() : "none");
    if (!parent || in_list(rejected_list(), std::format("parent {}", parent))) return false;
    BodyTest b;
    b.parent = parent;
    b.need_pitch = need_pitch;
    b.pitch_slot = g.test.found_pitch;
    b.still_since = now;
    g.body = b;
    g.phase = State::Body;
    return true;
}

// The Transform the body test turns: the parent for yaw, the camera's own Transform for pitch. Never the Camera component
// itself: Transform.get_rotation on it crashed Unity's native code (The Planet Crafter Demo).
void* body_target(Runtime& rt) { return parent_transform(rt, g.camera, g.body.pitch_stage ? 0 : g.body.parent); }

void body_undo(Runtime& rt) {
    turn_transform(rt, body_target(rt), g.body.pitch_stage ? -g.body.asked : 0, g.body.pitch_stage ? 0 : -g.body.asked);
}

void body_write(Runtime& rt) {
    ProbeResult result;
    result.yaw_parent = g.body.parent;
    result.automatic = g.quiet;
    if (g.body.pitch_slot >= 0) {
        const Slot& pitch = g.slots[size_t(g.body.pitch_slot)];
        result.pitch_class = g.owners[pitch.owner].klass;
        result.pitch_path = g.paths[pitch.path];
        result.pitch_per_degree = slot_per_degree(uint32_t(g.body.pitch_slot), true);
        result.camera_field = camera_field(rt, g.owners[pitch.owner].object, g.camera);
    }
    write_result(rt, std::move(result));
}

void body_step(Runtime& rt, ULONGLONG now) {
    BodyTest& b = g.body;
    ViewAngles angles;
    if (!read_angles(rt, g.camera, angles)) return give_up(rt, "the camera went away during the live test");
    if (std::abs(wrap180(angles.yaw - b.last_yaw)) + std::abs(angles.pitch - b.last_pitch) > kNudgeStill) b.still_since = now;
    b.last_yaw = angles.yaw, b.last_pitch = angles.pitch;
    const double yaw = wrap180(angles.yaw - b.base_yaw), pitch = angles.pitch - b.base_pitch;
    const double moved = b.pitch_stage ? pitch : yaw, other = b.pitch_stage ? yaw : pitch;
    void* target = body_target(rt);
    const auto turn = [&](double size) { return b.pitch_stage ? turn_transform(rt, target, size, 0) : turn_transform(rt, target, 0, size); };
    switch (b.stage) {
    case BodyTest::Settle:
        if (look_input()) b.still_since = now;
        if (now - b.still_since < kRestMs) return;
        b.base_yaw = angles.yaw, b.base_pitch = angles.pitch;
        b.asked = b.pitch_stage && angles.pitch > 0 ? -kTestTurn : kTestTurn;
        if (!turn(b.asked)) return give_up(rt, "the camera's Transforms can't be written");
        b.stage = BodyTest::Measure;
        b.at = now, b.ok_since = 0, b.passed = false;
        return;
    case BodyTest::Measure:
        if (look_input()) {
            turn(-b.asked);
            b.stage = BodyTest::Settle;
            b.still_since = now;
            return;
        }
        if (turn_followed(b.asked, moved, other)) {
            if (!b.ok_since) b.ok_since = now;
            if (now - b.ok_since >= 200) b.passed = true;
        } else {
            b.ok_since = 0;
        }
        if (b.passed || now - b.at >= 1000) {
            probe_log("probe: test %s: asked %.1f, the camera moved %.1f (other axis %.1f): %s",
                      b.pitch_stage ? "pitch on the camera's Transform" : std::format("yaw on parent {}", b.parent).c_str(), b.asked, moved,
                      other, b.passed ? "kept" : "failed");
            if (b.passed) turn(-b.asked);  // one the game undid has nothing to undo
            b.stage = BodyTest::Back;
            b.at = now;
        }
        return;
    case BodyTest::Back:
        if (now - b.at < kRestMs) return;
        if (!b.passed) return give_up(rt, b.pitch_stage ? "the game rewrites the camera's pitch" : "the game rewrites its body's yaw");
        if (b.need_pitch && !b.pitch_stage) {
            b.pitch_stage = true;
            b.stage = BodyTest::Settle;
            b.still_since = now;
            return;
        }
        return body_write(rt);
    }
}

// --- The phases ---

void restart_scan(Runtime& rt, const char* why) {
    log("probe: %s: looking at the scene again", why);
    release_scene(rt);
    g.phase = State::Scan;
    g.next_try = 0;
    g.turns = 0;
    g.turned = g.pitch_total = g.yaw2 = g.pitch2 = 0;
    g.look_ms = 0;
}

void scan_step(Runtime& rt, ULONGLONG now) {
    if (now < g.next_try) return;
    g.next_try = now + 2000;
    list_cameras(rt);
    for (Camera& camera : g.cameras) {
        ViewAngles angles;
        if (read_angles(rt, camera.object, angles)) camera.yaw = angles.yaw, camera.pitch = angles.pitch;
    }
    if (g.cameras.empty()) {
        constexpr int kScanTries = 5;
        if (++g.scan_tries < kScanTries) return;
        log("probe: no perspective camera found after %d tries", kScanTries);
        probe_log("probe: no perspective camera found: nothing to look at in this scene");
        if (!g.automatic && !g.quiet) notify::show(L"Gyro4All probe: no camera found in this scene.\nTry again in gameplay. See Gyro4All\\Gyro4All.log.", 10);
        return end(rt, "no camera found");
    }
    log("probe: %zu camera(s) to watch", g.cameras.size());
    scan_scripts(rt);
    dump(rt);
    if (g.discovery) {
        g.nudge = {};
        g.nudge.groups = nudge_groups(rt);
        g.nudge.deadline = now + kNudgeBudgetMs;
        g.nudge.still_since = now;
        g.phase = State::Nudging;
        return;
    }
    g.phase = State::Pick;
    g.last_tick = now;
    if (g.quiet) return;
    notify::show(g.automatic ? L"Gyro4All: this camera doesn't answer the gyro. To teach it to Gyro4All, turn the camera with the "
                               L"RIGHT STICK or MOUSE (more than 30°, also up and down), then stop. Repeat 3 times.\nThen don't "
                               L"touch the controller for a few seconds."
                             : L"Gyro4All probe ready to find the camera. Turn it with the RIGHT STICK or MOUSE (more than 30°, "
                               L"also up and down), then stop. Repeat 3 times.\nThen don't touch the controller while the probe "
                               L"tests what it found.",
                 20);
}

void pick_step(Runtime& rt, ULONGLONG now) {
    const bool look = look_input();
    if (look) g.look_ms += now - g.last_tick;
    g.last_tick = now;
    if (now < g.next_sample) return;
    g.next_sample = now + kSampleMs;
    Camera* best = nullptr;
    for (Camera& camera : g.cameras) {
        ViewAngles angles;
        if (!read_angles(rt, camera.object, angles)) return restart_scan(rt, "a camera went away");
        if (look) camera.moved += std::abs(wrap180(angles.yaw - camera.yaw));
        camera.yaw = angles.yaw;
        if (!best || camera.moved > best->moved) best = &camera;
    }
    if (best && best->moved >= kPickDegrees) {
        g.camera = best->object;
        ViewAngles angles;
        read_angles(rt, g.camera, angles);
        g.yaw = angles.yaw, g.pitch = angles.pitch;
        g.still_since = now;
        g.phase = State::Capture;
        sample_slots(0, 0, false);  // the changes since the scan aren't this turn's
        const std::string name = g_api.get_game_object && g_api.get_name ? hierarchy(rt, g.camera) : "";
        log("probe: the camera that turns with the stick or mouse: %s", name.c_str());
        probe_log("probe: the camera that turns with the stick or mouse: %s", name.c_str());
        return;
    }
    if (g.look_ms >= kNoCameraLookMs) {
        log("probe: no camera turned with %llu ms of stick or mouse: a fixed scene camera", g.look_ms);
        probe_log("probe: no camera turned with the stick or mouse: a fixed scene camera, or the game isn't taking look input here");
        if (!g.quiet)
            notify::show(L"Gyro4All probe: no camera of this scene turns with the stick or mouse, so it can't be aimed.\n"
                         L"Try again where the camera moves.",
                         12);
        end(rt, "no camera turned");
    }
}

void capture_step(Runtime& rt, ULONGLONG now) {
    if (look_input()) g.last_look = now;
    if (now < g.next_sample) return;
    g.next_sample = now + kSampleMs;
    ViewAngles angles;
    if (!read_angles(rt, g.camera, angles)) return restart_scan(rt, "the camera went away");
    const double dyaw = wrap180(angles.yaw - g.yaw), dpitch = angles.pitch - g.pitch;
    g.yaw = angles.yaw, g.pitch = angles.pitch;
    const bool moving = std::abs(dyaw) + std::abs(dpitch) > 0.05;
    const bool count = moving && now - g.last_look < kLookHoldMs;
    if (moving) g.still_since = now;
    if (count) {
        g.yaw2 += dyaw * dyaw;
        g.pitch2 += dpitch * dpitch;
        g.turned += std::abs(dyaw);
        g.pitch_total += std::abs(dpitch);
    }
    sample_slots(dyaw, dpitch, count);
    if (g.turned < kTurnDegrees || now - g.still_since < kRestMs) return;
    ++g.turns;
    g.turned = 0;
    log("probe: turn %d captured", g.turns);
    probe_log("probe: turn %d captured", g.turns);
    if (g.turns >= kTurnsForTest && g.pitch_total >= kMinPitchDegrees && start_test(rt, now)) return;
    if (g.turns >= kMaxTurns) {
        if (start_test(rt, now)) return;
        g.test = {};
        return finish(rt);
    }
    if (g.quiet) return;
    notify::show(g.turns >= kTurnsForTest && g.pitch_total < kMinPitchDegrees
                     ? L"Gyro4All probe: now also turn the camera UP AND DOWN, then stop."
                 : g.turns < kTurnsForTest ? std::format(L"Gyro4All probe: turn {} of {} done.\nTurn the camera again with the RIGHT "
                                                         L"STICK or MOUSE (more than 30°, also up and down), then stop.",
                                                         g.turns, kTurnsForTest)
                                           : std::format(L"Gyro4All probe: turn {} done, nothing to test yet.\nTurn the camera again "
                                                         L"with the RIGHT STICK or MOUSE (more than 30°, also up and down), then stop.",
                                                         g.turns),
                 10);
}

bool start(Runtime& rt, bool automatic, const char* why) {
    if (g.phase != State::Off) return false;
    if (!rt.ready() || !rt.has_reflection() || !resolve_api(rt)) {
        log("probe: can't run (%s): this game's Unity runtime lacks what it needs (runtime %s, reflection exports %s)", why,
            backend_name(rt.backend()), rt.has_reflection() ? "found" : "missing");
        return false;
    }
    open_log();
    g = {};
    g.phase = State::Scan;
    g.automatic = automatic;
    g.started_at = GetTickCount64();
    log("probe: started (%s)", why);
    probe_log("probe: started (%s)", why);
    if (!automatic) notify::show(L"Gyro4All probe started: looking for the scene's cameras.\nIt needs gameplay: it waits while a menu is open.", 6);
    return true;
}

void keys(Runtime& rt) {
    const bool focused = host::game_has_focus();
    if (probe_key_pressed(g_key, focused, g_key_down)) {
        const bool running = g.phase != State::Off, background = g.quiet;
        if (running) {
            if (g.phase == State::Testing && g.test.stage == Test::Measure) {
                const std::vector<uint32_t>& list = g.test.pitch_axis ? g.test.pitch : g.test.yaw;
                if (g.test.index < list.size()) restore_value(g.slots[list[g.test.index]]);
            }
            if (g.phase == State::Nudging) nudge_restore();
            if (g.phase == State::Body && g.body.stage == BodyTest::Measure) body_undo(rt);
            end(rt, "probe key");
        }
        if (!running || background) {
            if (!start(rt, false, "probe key")) notify::show(L"Gyro4All probe can't run here: see Gyro4All\\Gyro4All.log.", 8);
        } else {
            notify::show(L"Gyro4All probe cancelled.", 5);
        }
    }
    // Plain F7 only: with Ctrl, Shift or Alt it may be the probe key.
    const bool f7 = focused && key_held(VK_F7) && !key_held(VK_CONTROL) && !key_held(VK_SHIFT) && !key_held(VK_MENU);
    if (f7 && !g_f7_down && rt.ready() && rt.has_reflection() && resolve_api(rt)) {
        if (g.phase == State::Off) {
            scan_scripts(rt);
            dump(rt);
            release_scene(rt);
        } else {
            dump(rt);
        }
        notify::show(L"Gyro4All: the scene's cameras written to Gyro4All\\Gyro4All-probe.log.", 4);
    }
    g_f7_down = f7;
}

// --- The game's own pitch, under a pitch on the camera's Transform ---

// A view with its yaw on the body and its pitch on the camera's Transform has no float for the pitch. Many games keep one
// anyway and copy it to the camera, some only on look input: their next stick turn would put back the pitch the gyro moved.
// The floats of the scripts on the camera and what it hangs from are compared with the camera's pitch after the player's own
// turns; one that always matches is the game's look state, and goes into the profile with PitchTransform=1.
struct PitchWatch {
    void* camera = nullptr;
    bool done = false;  // found, or nothing on this camera's rig can be it
    std::vector<Owner> owners;
    std::vector<std::string> paths;
    std::vector<Slot> slots;
    std::vector<float> values;
    PitchStateMatch match;
    double last_pitch = 1e9;
    int samples = 0;
} g_pitch;

void pitch_release(Runtime& rt) {
    for (const Owner& owner : g_pitch.owners) rt.free_handle(owner.handle);
    g_pitch.owners.clear();
    g_pitch.paths.clear();
    g_pitch.slots.clear();
}

void pitch_scan(Runtime& rt, void* camera) {
    std::unordered_set<void*> rig;
    void* transform = rt.invoke(g_api.get_transform, camera);
    for (int depth = 0; transform && depth < 16; ++depth) {
        rig.insert(transform);
        transform = rt.invoke(g_api.get_parent, transform);
    }
    Scanner scan;
    scan.paths = &g_pitch.paths;
    scan.slots = &g_pitch.slots;
    void* array = rt.find_objects_of_type(rt.find_class("UnityEngine", "MonoBehaviour"));
    const size_t count = rt.array_length(array);
    for (size_t i = 0; i < count; ++i) {
        void** element = static_cast<void**>(rt.array_element(array, i));
        void* object = element ? *element : nullptr;
        void* klass = object ? rt.object_class(object) : nullptr;
        if (!klass || skipped_namespace(rt.class_namespace(klass)) || !rt.alive(object) ||
            !rig.count(rt.invoke(g_api.get_transform, object)))
            continue;
        g_pitch.owners.push_back({object, rt.new_handle(object, false), rt.class_full_name(klass)});
        add_fields(rt, scan, uint32_t(g_pitch.owners.size() - 1), reinterpret_cast<uintptr_t>(object), klass, "", 0, 0, true);
    }
    // What the player's turns already showed isn't the look state stays out.
    const std::string not_these = rejected_list();
    std::erase_if(g_pitch.slots, [&](const Slot& slot) {
        return in_list(not_these, g_pitch.owners[slot.owner].klass + " " + g_pitch.paths[slot.path]);
    });
    g_pitch.match.reset(g_pitch.slots.size());
    log("unity: pitch on the camera's Transform: watching %zu floats of %zu scripts on its rig for the game's own pitch",
        g_pitch.slots.size(), g_pitch.owners.size());
}

void pitch_found(Runtime& rt, int yaw_parent) {
    std::vector<PitchStateMatch::Found> found = g_pitch.match.found();
    // Copies of one value: a name that says pitch first, then the first script.
    const auto named = std::find_if(found.begin(), found.end(), [](const PitchStateMatch::Found& f) {
        std::string path = g_pitch.paths[g_pitch.slots[f.index].path];
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return path.find("pitch") != std::string::npos;
    });
    const PitchStateMatch::Found pick = named != found.end() ? *named : found.front();
    for (const PitchStateMatch::Found& f : found) {
        const Slot& slot = g_pitch.slots[f.index];
        log("unity: %s %s follows the camera's pitch (%g per degree up)%s", g_pitch.owners[slot.owner].klass.c_str(),
            g_pitch.paths[slot.path].c_str(), f.per_degree, f.index == pick.index ? ": the game's pitch" : "");
    }
    const Slot& slot = g_pitch.slots[pick.index];
    ProbeResult result;
    result.pitch_class = g_pitch.owners[slot.owner].klass;
    result.pitch_path = g_pitch.paths[slot.path];
    result.pitch_per_degree = pick.per_degree;
    result.pitch_transform = true;
    result.yaw_parent = yaw_parent;
    result.automatic = true;
    g_pitch.done = true;
    pitch_release(rt);
    write_result(rt, std::move(result));
}

}  // namespace

const char* probe_default_settings() { return kSettings; }

void probe_pitch_state(Runtime& rt, void* camera, int yaw_parent, bool sample, double pitch_up) {
    if (!camera || (g_pitch.done && camera == g_pitch.camera)) return;
    if (g.phase != State::Off || !rt.ready() || !rt.has_reflection() || !resolve_api(rt) || !g_api.get_parent) return;
    if (camera != g_pitch.camera) {
        pitch_release(rt);
        g_pitch = {};
        g_pitch.camera = camera;
        pitch_scan(rt, camera);
        g_pitch.done = g_pitch.slots.empty();
        return;
    }
    if (!sample || std::abs(pitch_up - g_pitch.last_pitch) < 0.5) return;
    g_pitch.last_pitch = pitch_up;
    g_pitch.values.resize(g_pitch.slots.size());
    for (size_t i = 0; i < g_pitch.slots.size(); ++i) {
        const Slot& slot = g_pitch.slots[i];
        float value = 0;
        g_pitch.values[i] = rt.alive(g_pitch.owners[slot.owner].object) && mem::rd(slot.address, value) ? value : NAN;
    }
    g_pitch.match.add(g_pitch.values, pitch_up);
    ++g_pitch.samples;
    if (!g_pitch.match.found().empty()) return pitch_found(rt, yaw_parent);
    if (g_pitch.match.hopeless()) {
        log("unity: no float on the camera's rig follows its pitch (%d turns): the pitch stays on its Transform", g_pitch.samples);
        g_pitch.done = true;
        pitch_release(rt);
    }
}


void probe_configure(bool wanted_by_profile) {
    const bool wanted = wanted_by_profile || settings::get(L"Probe", 0) != 0;
    if (wanted && !g_was_wanted) g_start_wanted = true;
    if (!wanted) g_start_wanted = false;
    g_was_wanted = wanted;
    g_auto_setting = settings::get(L"AutoProbe", 1) != 0;
    g_key = parse_probe_key(probe_key_setting());
    log("settings: Probe=%d AutoProbe=%d ProbeKey=%s (Gyro4All.ini, the game's profile overrides); F7 = dump the cameras", wanted ? 1 : 0,
        g_auto_setting ? 1 : 0, g_key.name.empty() ? "(none)" : g_key.name.c_str());
}

void probe_frame(Runtime& rt, bool gameplay) {
    if (!rt.ready()) return;
    keys(rt);
    if (g_start_wanted) {
        g_start_wanted = false;
        if (!start(rt, false, "Probe=1")) notify::show(L"Gyro4All probe can't run here: see Gyro4All\\Gyro4All.log.", 8);
    }
    if (g.phase == State::Off) return;
    const ULONGLONG now = GetTickCount64();
    if (g.automatic && !g.discovery && g.phase != State::Testing && g.phase != State::Body && g.turns == 0 &&
        now - g.started_at > (g.quiet ? kQuietGiveUpMs : kAutoGiveUpMs)) {
        end(rt, "AutoProbe: no camera turn with the stick or mouse in time");
        if (!g.quiet) notify::show(L"Gyro4All: stopped looking for the camera (no turn with the stick or mouse).", 6);
        return;
    }
    if (!gameplay || (g.quiet && g_menu_on_screen)) {
        // A turn being tested goes back at once, and is tried again when the game is back.
        if (g.phase == State::Testing && g.test.stage != Test::Settle) {
            const std::vector<uint32_t>& list = g.test.pitch_axis ? g.test.pitch : g.test.yaw;
            if (g.test.stage == Test::Measure && g.test.index < list.size()) restore_value(g.slots[list[g.test.index]]);
            g.test.stage = Test::Settle;
        }
        if (g.phase == State::Testing) g.test.still_since = now;
        if (g.phase == State::Nudging && g.nudge.stage == Nudge::Watch) {
            nudge_restore();
            g.nudge.groups.push_back(g.nudge.group);
            g.nudge.stage = Nudge::Settle;
        }
        if (g.phase == State::Nudging) g.nudge.still_since = now, g.nudge.deadline = std::max(g.nudge.deadline, now + 2000);
        if (g.phase == State::Body && g.body.stage != BodyTest::Settle) {
            if (g.body.stage == BodyTest::Measure) body_undo(rt);
            g.body.stage = BodyTest::Settle;
        }
        if (g.phase == State::Body) g.body.still_since = now;
        g.last_tick = now;
        return;
    }
    switch (g.phase) {
    case State::Scan: return scan_step(rt, now);
    case State::Nudging: return nudge_step(rt, now);
    case State::Body: return body_step(rt, now);
    case State::Pick: return pick_step(rt, now);
    case State::Capture: return capture_step(rt, now);
    case State::Testing: return test_step(rt, now);
    case State::Off: return;
    }
}

bool probe_active() { return g.phase != State::Off; }

bool probe_holds_gyro() {
    return g.phase != State::Off && (!g.automatic || g.discovery || g.phase == State::Testing || g.phase == State::Body);
}

bool probe_watching_quietly() {
    return g.quiet && !g.discovery && (g.phase == State::Scan || g.phase == State::Pick || g.phase == State::Capture);
}

bool probe_discovery_pending() {
    return g_auto_setting && auto_window_open() && !g_discovery_done && g_discoveries < kMaxDiscoveries && g_discover_camera != g_discover_tried &&
           (g.phase == State::Off || probe_watching_quietly());
}

bool probe_menu_on_screen() { return g_menu_on_screen; }

// The UI element that has the pad's or keyboard's focus, if it is still on screen: what makes a menu a menu, however many
// buttons a HUD has. A locked cursor doesn't say "not a menu": Risk of Rain 2 locks it and draws its own. Every event system,
// not only EventSystem.current: that game has one per player.
void* focused_ui(Runtime& rt) {
    void* klass = rt.find_class("UnityEngine.EventSystems", "EventSystem");
    void* selected = rt.find_method(klass, "get_currentSelectedGameObject", 0);
    void* active = rt.find_method(rt.find_class("UnityEngine", "GameObject"), "get_activeInHierarchy", 0);
    if (!selected) return nullptr;
    void* array = rt.find_objects_of_type(klass);
    const size_t count = rt.array_length(array);
    for (size_t i = 0; i < count; ++i) {
        void** element = static_cast<void**>(rt.array_element(array, i));
        void* system = element ? *element : nullptr;
        void* object = system && rt.alive(system) ? rt.invoke(selected, system) : nullptr;
        if (!object || !rt.alive(object)) continue;
        uint8_t on = 1;  // a menu that closed may leave its last button selected
        if (active) rt.returned(rt.invoke(active, object), on);
        if (on) return object;
    }
    return nullptr;
}

// Throttled. A game could keep something focused during play: after a minute on the same camera it stops counting for
// that camera.
void watch_menu(Runtime& rt, void* camera, ULONGLONG now) {
    if (!g_auto_setting || !camera) {
        g_menu_on_screen = false;
        g_menu_since = 0;
        return;
    }
    if (now < g_menu_next) return;
    g_menu_next = now + kMenuCheckMs;
    void* focused = focused_ui(rt);
    const bool menu = focused && camera != g_menu_camera;
    if (menu != g_menu_on_screen) {
        const std::string name = focused && resolve_api(rt) ? narrow_string(rt, rt.invoke(g_api.get_name, focused)) : "";
        if (menu) log("unity: UI focus on '%s': a menu", name.c_str());
        else log("unity: no UI focus%s: not a menu", focused ? " that counts" : "");
    }
    g_menu_on_screen = menu;
    if (!menu) {
        g_menu_since = 0;
        return;
    }
    if (!g_menu_since) g_menu_since = now;
    if (now - g_menu_since < kMenuPatienceMs) return;
    log("unity: a minute of UI focus on this camera: taken as gameplay");
    g_menu_camera = camera;
    g_menu_on_screen = false;
    g_menu_since = 0;
}

void probe_discovery_again() {
    g_discovery_done = false;
    g_discover_tried = nullptr;
    g_discover_next = 0;
    if (g_discoveries >= kMaxDiscoveries) g_discoveries = kMaxDiscoveries - 1;
    // A pitch found by the watch may have been what was wrong: the next view with no pitch float watches again, without it.
    if (g_pitch.done && g_pitch.owners.empty()) g_pitch.camera = nullptr, g_pitch.done = false;
}

void probe_discover(Runtime& rt, void* camera, bool allowed) {
    const ULONGLONG now = GetTickCount64();
    watch_menu(rt, allowed ? camera : nullptr, now);
    if (allowed && camera && !g_menu_on_screen && g_play_last) {
        const bool was_open = auto_window_open();
        g_play_ms += std::min<ULONGLONG>(now - g_play_last, 1000);
        if (was_open && !auto_window_open())
            log("probe: the first %llu s of play are over: no more automatic searches (%s to search again)",
                kAutoWindowMs / 1000, "the probe key");
    }
    g_play_last = allowed && camera ? now : 0;
    if (g.discovery && (g.phase == State::Scan || g.phase == State::Nudging) && (camera != g_discover_tried || g_menu_on_screen)) {
        nudge_restore();
        if (g_menu_on_screen) --g_discoveries;  // it starts over when the menu closes
        end(rt, g_menu_on_screen ? "a menu" : "the camera changed");
        g_discover_tried = nullptr;
    }
    if (g_menu_on_screen) {
        g_menu_left_at = now;
        g_discover_next = 0;  // a camera searched before this menu gets its second look when it closes
    }
    if (!allowed || camera != g_discover_camera || g_menu_on_screen) {
        g_discover_since = now;
        g_discover_view.reset();
    }
    g_discover_camera = camera;
    if (!allowed || g_menu_on_screen || !camera || !g_auto_setting || !auto_window_open() || g_discovery_done ||
        g_discoveries >= kMaxDiscoveries)
        return;
    if (probe_watching_quietly() && g.phase == State::Capture && camera != g_discover_turned) {
        log("probe: the player turns this camera: searching it again when it stops");
        g_discover_turned = camera;
        g_discover_tried = nullptr;
        g_discover_since = now;
        g_discover_view.reset();
    }
    const bool again = camera == g_discover_tried;
    if (again && (camera == g_discover_retried || now < g_discover_next)) return;
    if (now - g_menu_left_at < kDiscoverAfterMenuMs) return;  // focus comes and goes between one menu and the next
    ViewAngles view;
    if (!resolve_api(rt) || !read_angles(rt, camera, view)) return;
    // A camera that never settles (a long pan, a strong sway) is searched anyway after a while: finding nothing costs little.
    if (!g_discover_view.add(now, view.yaw, view.pitch, kDiscoverStableMs) && now - g_discover_since < kDiscoverPatienceMs) return;
    // Turns already captured on this camera are worth more than looking again.
    if (probe_watching_quietly() && (!again || g.turns == 0)) end(rt, again ? "looking again" : "a new camera");
    if (g.phase != State::Off) return;
    if (again) g_discover_retried = camera;
    g_discover_tried = camera;
    g_discover_next = now + kDiscoverRetryMs;
    ++g_discoveries;
    if (!start(rt, true, "looking for the camera by itself")) {
        g_discoveries = kMaxDiscoveries;
        return;
    }
    g.discovery = g.quiet = true;
}

bool probe_request_auto(Runtime& rt, const char* why) {
    if (!g_auto_setting || !auto_window_open() || g.phase != State::Off || g_auto_starts >= kMaxAutoStarts || GetTickCount64() < g_next_auto) return false;
    if (!start(rt, true, why)) {
        g_auto_starts = kMaxAutoStarts;  // it won't work later either: said once
        return false;
    }
    ++g_auto_starts;
    return true;
}

}  // namespace gyro4all::unity
