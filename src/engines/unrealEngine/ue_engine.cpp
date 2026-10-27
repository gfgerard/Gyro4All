#include "ue_engine.hpp"

#include "camera_match.hpp"
#include "engine.hpp"
#include "host.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "settings.hpp"
#include "ue_probe.hpp"
#include "ue_update_rotation.hpp"
#include "unreal.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <string>
#include <utility>
#include <vector>

// Unreal Engine (UE 4.25+ / UE5): the scene (GEngine -> GameViewport -> World -> LocalPlayers[0] -> PlayerController,
// Pawn, PlayerCameraManager), the profile's views and how the gyro turns them, and the follow check.
namespace gyro4all::unreal {
namespace {

std::vector<View> g_views;          // read at start and again whenever the profile file changes
FILETIME g_profile_time{};           // the profile's last write time when it was read
bool g_views_changed = false;        // for Engine::views_changed
ue::Obj g_engine = 0;
ULONGLONG g_next_engine_try = 0;
uint32_t g_last_view = UINT32_MAX;
Scene g_scene;
View* g_view = nullptr;
View* g_base = nullptr;              // the view for the view target (g_view may be one of its Match=Camera views)
CameraMatch g_match;

FILETIME profile_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(settings::profile_path().c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime
                                                                                                : FILETIME{};
}

void load_profile() {
    probe_configure(has_views(settings::profile_path()));
    g_views = load_views(settings::profile_path());
    g_profile_time = profile_stamp();
}

// The profile file changed (edited by hand, or a view added by the probe): read its views again and use them at once.
// Only the views: the profile's [Gyro4All] settings still apply from the next start.
void check_profile_file() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    const FILETIME stamp = profile_stamp();
    if (CompareFileTime(&stamp, &g_profile_time) == 0) return;
    g_profile_time = stamp;
    log("profile: the profile file changed, views read again");
    g_views = load_views(settings::profile_path());
    // A new view can't take a sensitivity profile's id once that profile has its tab (at start the view wins, and the
    // core turns that profile off).
    std::erase_if(g_views, [](const View& v) {
        const bool taken = std::find(std::begin(kSensitivityViewIds), std::end(kSensitivityViewIds), v.id) !=
                           std::end(kSensitivityViewIds);
        if (taken) log("profile: view %s ignored, Id=%u belongs to a sensitivity profile", v.key.c_str(), v.id);
        return taken;
    });
    g_view = g_base = nullptr;
    g_views_changed = true;
    g_last_view = UINT32_MAX;  // log the active view again
}

Scene read_scene() {
    Scene s;
    if (!ue::init()) return s;
    if (!g_engine) {
        const ULONGLONG now = GetTickCount64();
        if (now < g_next_engine_try) return s;
        g_next_engine_try = now + 1000;
        g_engine = ue::find_instance("GameEngine");
        if (!g_engine) return s;
        log("ue: engine %s (%s)", ue::name_of(g_engine).c_str(), ue::name_of(ue::class_of(g_engine)).c_str());
    }
    const ue::Obj viewport = ue::read_obj(g_engine, "GameViewport");
    const ue::Obj world = ue::read_obj(viewport, "World");
    const ue::Obj instance = ue::read_obj(world, "OwningGameInstance");
    const ue::Prop players = ue::prop_of(instance, "LocalPlayers");
    if (!players) return s;
    uintptr_t data = 0;
    int32_t count = 0;
    if (!ue::read_ptr(instance + players.offset, data) || !ue::read_i32(instance + players.offset + 8, count) || count < 1)
        return s;
    uintptr_t local_player = 0;
    if (!ue::read_ptr(data, local_player)) return s;
    s.pc = ue::read_obj(local_player, "PlayerController");
    if (!s.pc) return s;
    s.pawn = ue::read_obj(s.pc, "Pawn");
    s.cursor = ue::read_bool(s.pc, "bShowMouseCursor", false);

    const ue::Obj camera = ue::read_obj(s.pc, "PlayerCameraManager");
    s.camera = camera;
    // Read every frame: the follow check keeps watching after its verdict (offsets cached, a few guarded reads).
    s.pov_ok = ue::camera_pov(camera, s.pov_pitch, s.pov_yaw);
    s.loc_ok = probe_on() && s.pov_ok && ue::camera_pov_location(camera, s.pov_loc);
    const ue::Prop view_target = ue::prop_of(camera, "ViewTarget");
    const ue::Prop target = ue::find_prop(ue::struct_type(view_target), "Target");
    if (view_target && target) ue::read_ptr(camera + view_target.offset + target.offset, s.view_target);

    const ue::Obj level = ue::read_obj(world, "PersistentLevel");
    const ue::Obj settings = ue::read_obj(level, "WorldSettings");
    // UWorld::IsPaused(): WorldSettings has a pauser (PauserPlayerState in 4.2x/5.x, Pauser in older engines).
    s.paused = ue::prop_of(settings, "PauserPlayerState") ? ue::read_obj(settings, "PauserPlayerState") != 0
                                                          : ue::read_obj(settings, "Pauser") != 0;
    // Scripted moments count as a pause (the gyro stops, as the stick does in them):
    // - a reflected IsInActiveCutscene on the pawn (Dead Island 2's BP_Base_Player_C);
    // - the pawn's CharacterMovement in MOVE_Flying (5): games move the character along a scripted path with it (Dead
    //   Island 2's in-engine scenes, where the stick doesn't turn the camera; MovementMode is 1, walking, in play).
    const bool cutscene = ue::read_bool(s.pawn, "IsInActiveCutscene", false);
    const ue::Obj movement = ue::read_obj(s.pawn, "CharacterMovement");
    const ue::Prop mode = ue::prop_of(movement, "MovementMode");
    int32_t mode_value = 0;
    const bool flying = mode && mode.element_size == 1 && ue::read_i32(movement + mode.offset, mode_value) &&
                        (mode_value & 0xFF) == 5;
    static int last_scripted = 0;  // 0 none, 1 cutscene, 2 flying
    const int scripted = cutscene ? 1 : flying ? 2 : 0;
    if (scripted != last_scripted && s.pawn) {
        log("scene: %s", scripted == 1   ? "the pawn is in a cutscene (IsInActiveCutscene): gyro paused"
                         : scripted == 2 ? "the pawn moves in MOVE_Flying (a scripted scene): gyro paused"
                                         : "scripted scene over");
        last_scripted = scripted;
    }
    s.paused = s.paused || scripted != 0;
    return s;
}

View* find_view(uint32_t id) {
    for (View& v : g_views)
        if (v.id == id) return &v;
    return nullptr;
}

// The view for the camera's view target: a ViewTargetClass view (not Match=Camera), else the default one.
View* base_view(const Scene& s) {
    if (s.view_target) {
        for (View& v : g_views)
            if (!v.match_camera && !v.view_target_class.empty() && ue::is_a(s.view_target, v.view_target_class.c_str()))
                return &v;
    }
    // Main menus usually possess a placeholder DefaultPawn (SpectatorPawn derives from it), or a game's own menu pawn
    // with a fixed camera (Ghostrunner 2, Valor Mortis: BP_MenuPawn_C): no gameplay camera.
    if (!s.pawn || ue::is_a(s.pawn, "DefaultPawn") || ue::is_a_like(s.pawn, "MenuPawn")) return nullptr;
    return &g_views.front();
}

// The view the gyro turns: the base view, or the Match=Camera view for the same view target that the camera follows
// now (core/camera_match.hpp).
View* active_view(const Scene& s, View* base) {
    if (!base) return nullptr;
    std::vector<uint32_t> members = {base->id};
    for (const View& v : g_views)
        if (v.match_camera && (v.view_target_class.empty() || ue::is_a(s.view_target, v.view_target_class.c_str())))
            members.push_back(v.id);
    View* view = find_view(g_match.select(members));
    return view ? view : base;
}

// Subobject lookups need a full object scan: cache them and re-check the cached object cheaply.
struct SubCache {
    ue::Obj outer = 0, found = 0;
    std::string name;
    ULONGLONG retry = 0;
};
std::vector<SubCache> g_sub_cache;

// One path step: an object property of outer, else a subobject of outer by name, or by class with "@Class"
// (first subobject whose class chain has it: for instance names that change every session).
ue::Obj resolve_part(ue::Obj outer, const std::string& name) {
    const bool by_class = !name.empty() && name.front() == '@';
    const std::string key = by_class ? name.substr(1) : name;
    if (!by_class)
        if (const ue::Obj o = ue::read_obj(outer, name.c_str())) return o;
    auto find = [&] { return by_class ? ue::find_subobject_of_class(outer, key.c_str()) : ue::find_subobject(outer, key.c_str()); };
    auto still_valid = [&](ue::Obj o) {
        return o && ue::outer_of(o) == outer && (by_class ? ue::is_a(o, key.c_str()) : ue::name_of(o) == key);
    };
    for (SubCache& c : g_sub_cache) {
        if (c.outer != outer || c.name != name) continue;
        if (still_valid(c.found)) return c.found;
        if (GetTickCount64() < c.retry) return 0;
        c.found = find();
        c.retry = GetTickCount64() + 2000;
        return c.found;
    }
    if (g_sub_cache.size() > 32) g_sub_cache.clear();
    g_sub_cache.push_back({outer, find(), name, GetTickCount64() + 2000});
    return g_sub_cache.back().found;
}

// A profile object path: Root (PC, Pawn, Camera, ViewTarget) then steps (see resolve_part).
}  // namespace

ue::Obj resolve_path(const std::vector<std::string>& path, const Scene& s) {
    if (path.empty()) return 0;
    const std::string& root = path.front();
    ue::Obj o = root == "PC"     ? s.pc
              : root == "Pawn"   ? s.pawn
              : root == "Camera" ? ue::read_obj(s.pc, "PlayerCameraManager")
                                 : s.view_target;
    for (size_t i = 1; i < path.size() && o; ++i) o = resolve_part(o, path[i]);
    return o;
}

ue::Obj view_object(const View& view, const Scene& s) {
    if (view.object.empty()) return view.id == 1 ? s.pc : s.view_target;
    return resolve_path(view.object, s);
}

bool apply(View& view, const Scene& s, double pitch, double yaw) {
    const ue::Obj target = view_object(view, s);
    if (!target) return false;
    if (view.pitch_view && view.pitch_view != view.id && pitch != 0) {
        // PitchView=: the pitch goes into the other view's rotation (only its rotation: its own PitchView and OnGyro
        // don't apply here).
        const auto other = std::find_if(g_views.begin(), g_views.end(), [&](const View& v) { return v.id == view.pitch_view; });
        if (other == g_views.end()) return false;
        View pitch_only = *other;
        pitch_only.pitch_view = 0;
        pitch_only.on_gyro.clear();
        if (!apply(pitch_only, s, pitch, 0)) return false;
        pitch = 0;
    }
    if (!view.offsets.empty()) {
        bool ok = true;
        for (int32_t off : view.offsets) ok &= ue::add_rotator(target + off, view.rotator_size, pitch, yaw);
        if (!ok) return false;
    } else if (update_rotation::is_input_property(view.property)) {
        // The stick's look input: queued for the controller's UpdateRotation (ue_update_rotation.hpp).
        int32_t offset = -1, size = 0;
        if (target != s.pc || !update_rotation::rotation_input(target, offset, size) ||
            !update_rotation::add(target, offset, size, pitch, yaw))
            return false;
    } else {
        const ue::Prop p = ue::prop_of(target, view.property.empty() ? "ControlRotation" : view.property.c_str());
        if (!p) return false;
        if (view.world_rotation) {
            const char* why = nullptr;
            if (!ue::add_world_rotation(target, p, pitch, yaw, why)) {
                if (why != view.world_why)
                    log("profile: view %u WorldRotation not applied to %s: %s", view.id, ue::name_of(target).c_str(), why);
                view.world_why = why;
                return false;
            }
        } else if (!ue::add_rotator(target + p.offset, p.element_size, pitch, yaw)) {
            return false;
        }
    }
    for (View::OnGyro& w : view.on_gyro) {
        const ue::Obj o = w.object.empty() ? target : resolve_path(w.object, s);
        if ((!o || !ue::write_number(o, w.property.c_str(), w.value)) && !w.warned) {
            w.warned = true;
            std::string path;
            for (const std::string& part : w.object) path += part + ".";
            log("profile: OnGyro %s%s not written (%s)", path.c_str(), w.property.c_str(),
                o ? "property missing or not a number" : "object not found");
        }
    }
    return true;
}

// The candidate's current rotation (its first rotator for raw groups).
bool read_view_rotation(const View& v, const Scene& s, double& pitch, double& yaw) {
    const ue::Obj target = view_object(v, s);
    if (!target) return false;
    double roll = 0;
    if (!v.offsets.empty()) return ue::read_rotator(target + v.offsets.front(), v.rotator_size, pitch, yaw, roll);
    if (update_rotation::is_input_property(v.property)) {
        // RotationInput only holds this frame's input: the view is ControlRotation, plus the turn not taken yet.
        const ue::Prop c = ue::prop_of(target, "ControlRotation");
        double queued_pitch = 0, queued_yaw = 0;
        update_rotation::pending(target, queued_pitch, queued_yaw);
        if (!c || !ue::read_rotator(target + c.offset, c.element_size, pitch, yaw, roll)) return false;
        pitch += queued_pitch, yaw += queued_yaw;
        return true;
    }
    const ue::Prop p = ue::prop_of(target, v.property.empty() ? "ControlRotation" : v.property.c_str());
    return p && ue::read_rotator(target + p.offset, p.element_size, pitch, yaw, roll);
}

// The pawn's own rotation (root component), NaN if unknown: a candidate that turns the character is noted.
double pawn_yaw(const Scene& s) {
    const ue::Obj root = ue::read_obj(s.pawn, "RootComponent");
    const ue::Prop p = ue::prop_of(root, "RelativeRotation");
    double pitch = 0, yaw = 0, roll = 0;
    return p && ue::read_rotator(root + p.offset, p.element_size, pitch, yaw, roll) ? yaw : std::nan("");
}

namespace {

void diagnostics(View& view, const Scene& s) {
    const ue::Obj target = view_object(view, s);
    if (!target) return;
    if (view.dump && !view.dumped) {
        view.dumped = true;
        log("dump: properties of %s (%s)", ue::name_of(target).c_str(), ue::name_of(ue::class_of(target)).c_str());
        ue::dump_props(target, "Actor");
    }
    for (size_t i = 0; i < view.watch.size(); ++i) {
        std::string value = ue::value_string(target, view.watch[i].c_str());
        if (value != view.watched_values[i]) {
            log("watch: %s.%s = %s", view.label.c_str(), view.watch[i].c_str(), value.empty() ? "?" : value.c_str());
            view.watched_values[i] = std::move(value);
        }
    }
}

// Step 1 of finding a camera: tells the user when the gyro turns the view's rotation but the camera doesn't
// follow (the game ignores ControlRotation, or the profile's rotation), or confirms that it does. Only on the
// player's own camera (see player_camera): scripted or fixed cameras ignore every rotation by design.
// After the verdict the check keeps running, only to log when the camera stops or starts following again (a
// camera switch inside the same view target, e.g. a game action that takes over the camera).
struct FollowCheck {
    double applied = 0, moved = 0, last_pitch = 0, last_yaw = 0, total_applied = 0, total_moved = 0;
    bool have_last = false, warned = false, confirmed = false;
    int ignored = 0, followed = 0;  // 2 s windows with enough gyro motion where the camera ignored / followed it
    int ignored_in_a_row = 0;       // ... ignored since the last followed window
    bool lost = false;              // after "confirmed": the camera stopped following (logged)
    bool stick = false;             // the right stick or the mouse moved in this window: the camera motion can't be judged
    ULONGLONG window_end = 0;
    // Some games' gameplay camera is its own actor, not the pawn (Hogwarts Legacy: BP_ThirdPersonCameraStackActor_C).
    // A view target class whose camera turned 30+ degrees while the right stick was held is the player's camera.
    ue::Obj stick_class = 0;        // view target class being measured
    double stick_moved = 0;         // camera rotation seen with the stick held, for stick_class
    std::vector<ue::Obj> player_cameras;
} g_follow;

// The view target is the player's own camera: the pawn, or a camera actor that follows the right stick.
bool player_camera(const Scene& s, double moved) {
    if (s.view_target == s.pawn) return true;
    const ue::Obj cls = ue::class_of(s.view_target);
    if (!cls) return false;
    if (std::find(g_follow.player_cameras.begin(), g_follow.player_cameras.end(), cls) != g_follow.player_cameras.end())
        return true;
    if (cls != g_follow.stick_class) g_follow.stick_class = cls, g_follow.stick_moved = 0;
    if (look_input()) g_follow.stick_moved += moved;
    if (g_follow.stick_moved < 30) return false;
    g_follow.player_cameras.push_back(cls);
    log("camera: view target %s turns with the right stick: treated as the player's camera", ue::name_of(cls).c_str());
    return true;
}

// Which camera the player sees, for the follow check's log lines.
std::string camera_description(const Scene& s) {
    std::string text = std::format("target {}, camera manager {}", ue::name_of(ue::class_of(s.view_target)),
                                   ue::name_of(ue::class_of(s.camera)));
    const std::string style = ue::value_string(s.camera, "CameraStyle");
    if (!style.empty()) text += ", CameraStyle " + style;
    return text;
}

// gyro_yaw: the gyro's yaw written this frame (degrees, absolute). The windows judge yaw only: at the pitch limit
// (looking straight up or down) the camera ignores pitch writes, which isn't the game ignoring the gyro.
void follow_check(const Scene& s, const View* view, double gyro_yaw) {
    if (!s.pov_ok) {
        g_follow.have_last = false;
        return;
    }
    const double moved_yaw = g_follow.have_last ? std::abs(wrap180(s.pov_yaw - g_follow.last_yaw)) : 0;
    const double moved = g_follow.have_last ? moved_yaw + std::abs(wrap180(s.pov_pitch - g_follow.last_pitch)) : 0;
    g_follow.last_pitch = s.pov_pitch, g_follow.last_yaw = s.pov_yaw, g_follow.have_last = true;
    if (!view || s.paused || s.cursor || !s.pawn) return;
    if (view->view_target_class.empty() && !player_camera(s, moved)) return;
    if (gyro_yaw <= 0) return;
    g_follow.applied += gyro_yaw;
    g_follow.moved += moved_yaw;
    if (look_input()) g_follow.stick = true;
    const ULONGLONG now = GetTickCount64();
    if (!g_follow.window_end) g_follow.window_end = now + 2000;
    if (now < g_follow.window_end) return;
    int verdict = 0;  // this window: -1 ignored, 1 followed, 0 unclear or not judged
    const bool judged = g_follow.applied >= 10 && !g_follow.stick;
    if (judged) {  // enough gyro motion in this window to judge, no stick or mouse
        const double ratio = g_follow.moved / g_follow.applied;
        if (ratio < 0.2) verdict = -1;
        else if (ratio > 0.5) verdict = 1;
    }
    const double window_applied = g_follow.applied, window_moved = g_follow.moved;
    g_follow.applied = g_follow.moved = 0;
    g_follow.window_end = 0;
    g_follow.stick = false;
    std::string rotation = view->property.empty() ? "ControlRotation" : view->property;
    if (!view->offsets.empty()) rotation = "the profile's raw offsets";
    if (verdict == -1) ++g_follow.ignored_in_a_row;
    if (verdict == 1) g_follow.ignored_in_a_row = 0;
    if (g_follow.warned || g_follow.confirmed) {  // verdict given: only log changes from here on
        // Three ignored windows in a row (about 6 s of gyro), as for the first verdict: one or two can be the pitch
        // limit, a camera blend, a short scripted moment or a hitch. Two gave a false AutoProbe start in PAYDAY 3
        // (an action that held the camera for ~4 s).
        if (g_follow.confirmed && !g_follow.lost && g_follow.ignored_in_a_row >= 3) {
            g_follow.lost = true;
            log("camera: stopped following the gyro (%s, view '%s'; last window: gyro %.0f degrees, camera %.0f) - %s",
                rotation.c_str(), view->label.c_str(), window_applied, window_moved, camera_description(s).c_str());
            request_auto_probe();
        } else if (verdict == 1 && (g_follow.lost || (g_follow.warned && !g_follow.confirmed))) {
            g_follow.lost = false;
            g_follow.confirmed = true;
            log("camera: follows the gyro again (%s, view '%s') - %s", rotation.c_str(), view->label.c_str(),
                camera_description(s).c_str());
            auto_probe_camera_follows();
        }
        return;
    }
    if (verdict == -1) ++g_follow.ignored;
    if (verdict == 1) ++g_follow.followed;
    if (judged) {
        g_follow.total_applied += window_applied;
        g_follow.total_moved += window_moved;
    }
    if (!g_follow.warned && g_follow.ignored >= 3 && g_follow.followed == 0) {
        g_follow.warned = true;
        log("camera: the gyro turned %s (view '%s') by %.0f degrees but the camera moved only %.0f: the game ignores it. "
            "To find the camera's real rotation, set Probe=1 in %s, restart and turn the camera with the right stick "
            "(ADVANCED.md: \"Finding the camera of a new game\").",
            rotation.c_str(), view->label.c_str(), g_follow.total_applied, g_follow.total_moved, narrow(settings::settings_path()).c_str());
        if (probe_on())
            notify::show(L"Gyro4All: the gyro doesn't move this camera yet.\nThe probe is on: turn the camera with the "
                         L"right stick or mouse to find the right rotation.", 12);
        else if (request_auto_probe())
            ;  // AutoProbe started: its own notice says what to do
        else if (view->offsets.empty() && view->property.empty())
            notify::show(L"Gyro4All: this game's camera seems to ignore the gyro.\nTo find it, set Probe=1 in Gyro4All\\Gyro4All.ini, "
                         L"restart the game. Ignore this if it's actually working.", 15);
        else
            notify::show(L"Gyro4All: the camera doesn't follow the profile's rotation.\nThe profile may be outdated: "
                         L"set Probe=1 in Gyro4All\\Gyro4All.ini to find the camera again.", 15);
    }
    if (!g_follow.confirmed && g_follow.followed >= 3) {
        g_follow.confirmed = true;
        log("camera: confirmed, the camera follows the gyro (%s, view '%s') - %s", rotation.c_str(), view->label.c_str(),
            camera_description(s).c_str());
    }
}

// The fast path of the Match=Camera choice reads the current view's rotation back: its yaw before and after this
// frame's gyro write, to see on the next frame whether the game put it back.
struct WriteBack {
    uint32_t view = 0;  // 0 = nothing written last frame
    double before = 0, after = 0, written = 0;
} g_write_back;

// This frame's gyro write into the active view (UnrealEngine::apply), for the Match=Camera choice and the follow check.
struct FrameWrite {
    bool read_back = false;  // the view's rotation was read before the write (more than one Match member)
    double now_yaw = 0;      // ... its yaw then
    int rewritten = -1;      // last frame's write: put back by the game (1), kept (0), unknown (-1)
    double yaw = 0, pitch = 0;  // written (or lost, with failed)
    bool failed = false;     // the view's rotation couldn't be written (its object is missing)
} g_write;

// One frame of the Match=Camera choice (core/camera_match.hpp). On a switch, the gyro the dropped view lost is written
// into the new one at once, so the camera catches up instead of losing the turn. Logs each switch with its reason (up
// to 200 per session: a cutscene ignores every view and would repeat them) and every view that follows after a switch.
// The carried-over turn isn't counted in the core's "applied" totals.
void camera_match_frame(const Scene& s, const View* view, double yaw, double pitch, bool write_failed, int rewritten) {
    static bool have_last = false, all_ignored = false;
    static double last_yaw = 0;
    static int switches_logged = 0;
    // Yaw only: at the pitch limit the camera ignores pitch whatever view turns it (camera_match.hpp).
    double moved = 0;
    if (s.pov_ok && have_last) moved = std::abs(wrap180(s.pov_yaw - last_yaw));
    if (s.pov_ok) last_yaw = s.pov_yaw;
    have_last = s.pov_ok;
    if (!view || !s.pov_ok || s.paused || s.cursor) return;
    auto label = [](uint32_t id) {
        const View* v = find_view(id);
        return v ? v->label : std::string("?");
    };
    CameraMatch::Frame f;
    f.yaw = yaw, f.pitch = pitch, f.moved = moved, f.look = look_input(), f.write_failed = write_failed;
    f.rewritten = rewritten;
    switch (g_match.frame(f, GetTickCount64())) {
    case CameraMatch::Switched: {
        g_write_back = {};
        View* next = find_view(g_match.current());
        const double cy = g_match.carry_yaw(), cp = g_match.carry_pitch();
        const bool carried = next && (cy != 0 || cp != 0) && apply(*next, s, cp, cy);
        const char* why = g_match.reason() == CameraMatch::Rewritten ? "is rewritten by the game"
                        : g_match.reason() == CameraMatch::Missing   ? "can't be written (object not found)"
                                                                     : "is ignored by the camera";
        if (++switches_logged <= 200)
            log("camera: '%s' %s, trying '%s' (%.1f degrees carried over)", label(g_match.previous()).c_str(), why,
                label(g_match.current()).c_str(), carried ? std::abs(cy) + std::abs(cp) : 0.0);
        break;
    }
    case CameraMatch::Follows:
        all_ignored = false;
        log("camera: the camera follows view '%s' now (Match=Camera) - %s", label(g_match.current()).c_str(),
            camera_description(s).c_str());
        break;
    case CameraMatch::AllIgnored:
        g_write_back = {};
        if (!all_ignored) log("camera: no view of this camera follows the gyro (cutscene, or a camera to learn) - %s",
                              camera_description(s).c_str());
        all_ignored = true;
        break;
    case CameraMatch::None: break;
    }
}

std::string status_scene(const Scene& s, const View* view) {
    return std::format("view={} pc={} pawn={} target={} paused={} cursor={}", view ? view->label : "none",
                       ue::name_of(ue::class_of(s.pc)), ue::name_of(ue::class_of(s.pawn)),
                       ue::name_of(ue::class_of(s.view_target)), int(s.paused), int(s.cursor));
}

void status_details(const Scene& s, const View* view) {
    // Object addresses, for a debugger (e.g. a write breakpoint on ControlRotation); logged when they change.
    static ue::Obj last_pc = 0, last_pawn = 0, last_camera = 0;
    if (s.pc != last_pc || s.pawn != last_pawn || s.camera != last_camera) {
        last_pc = s.pc, last_pawn = s.pawn, last_camera = s.camera;
        const ue::Prop cr = ue::prop_of(s.pc, "ControlRotation");
        log("status: addresses pc=0x%llX (ControlRotation +0x%X) pawn=0x%llX camera=0x%llX", (unsigned long long)s.pc,
            cr ? unsigned(cr.offset) : 0u, (unsigned long long)s.pawn, (unsigned long long)s.camera);
    }
    if (view && !view->offsets.empty()) {
        const ue::Obj o = view_object(*view, s);
        std::string text;
        for (int32_t off : view->offsets) {
            double p = 0, y = 0, r = 0;
            if (o && ue::read_rotator(o + off, view->rotator_size, p, y, r))
                text += std::format(" +0x{:X}=(P {:.2f} Y {:.2f})", off, p, y);
        }
        log("status: view object %s%s", o ? ue::name_of(o).c_str() : "(not found)", text.c_str());
    }
}

// The engine's reflection data can't be read (e.g. "ue: core classes not found"): nothing can turn the camera, and the
// probe needs the same data, so say so once instead of staying silent.
void engine_check() {
    static ULONGLONG since = GetTickCount64();
    static bool told = false;
    if (told || ue::ready() || GetTickCount64() - since < 60000) return;
    told = true;
    log("ue: the engine's data still can't be read after 60 s: the gyro can't turn the camera in this game "
        "(the probe needs the same data). Please report the game with this log.");
    notify::show(L"Gyro4All: this game isn't supported yet (its engine data can't be read),\n"
                 L"so the gyro can't turn the camera. Please report it with Gyro4All\\Gyro4All.log.", 20);
}

}  // namespace

bool right_stick_moving() {
    bool available = false;
    const bool moving = host::right_stick_moving(&available);
    if (available) probe_stick_seen();
    return moving;
}

bool look_input() { return right_stick_moving() || host::mouse_moving(); }

void profile_changed() { g_profile_time = {}; }

View test_view_for(const std::string& object, const std::string& property, const std::vector<int32_t>& offsets, int size,
                   bool world) {
    View v;
    v.label = "probe test";
    v.object = split(object, '.');
    v.property = property;
    v.offsets = offsets;
    v.rotator_size = size ? size : 24;
    v.world_rotation = world;
    return v;
}

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

class UnrealEngine final : public Engine {
public:
    const char* name() const override { return "Unreal"; }

    // "<Project>-Win64-Shipping.exe" (or WinGDK for Microsoft Store builds), or an exe in <Project>\Binaries\Win64
    // next to the project's Content\Paks or the engine's Engine\Binaries (renamed exes, e.g. HogwartsLegacy.exe).
    bool detect(const std::wstring& exe_path) const override {
        std::wstring lower = exe_path;
        for (wchar_t& c : lower) c = wchar_t(towlower(c));
        if (lower.ends_with(L"-win64-shipping.exe") || lower.ends_with(L"-wingdk-shipping.exe")) return true;
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));  // ...\<Project>\Binaries\Win64
        const std::wstring project = dir + L"\\..\\..";
        return exists(project + L"\\Content\\Paks") || exists(project + L"\\..\\Engine\\Binaries");
    }

    // "<Project>-Win64-Shipping" -> the modules of a modular build are "<Project>-<Module>-Win64-Shipping.dll".
    std::wstring module_prefix(const std::wstring& exe_stem) const override {
        std::wstring stem = exe_stem;
        for (wchar_t& c : stem) c = wchar_t(towlower(c));
        const size_t dash = stem.find(L'-');
        return dash != std::wstring::npos && dash > 0 ? stem.substr(0, dash + 1) : std::wstring{};
    }

    // Steam/Epic/GOG builds are "<Project>-Win64-Shipping", Microsoft Store / Game Pass ones "<Project>-WinGDK-Shipping":
    // a profile made with either fits the other.
    std::vector<std::wstring> profile_aliases(const std::wstring& exe_stem) const override {
        std::wstring lower = exe_stem;
        for (wchar_t& c : lower) c = wchar_t(towlower(c));
        const std::wstring stores[] = {L"-win64-shipping", L"-wingdk-shipping"}, names[] = {L"-Win64-Shipping", L"-WinGDK-Shipping"};
        for (int i = 0; i < 2; ++i)
            if (lower.ends_with(stores[i]))
                return {exe_stem.substr(0, exe_stem.size() - stores[i].size()) + names[1 - i]};
        return {};
    }

    // UE only checks empty XInput slots again after WM_DEVICECHANGE (XInputInterface's bNeedsControllerStateUpdate).
    const wchar_t* window_class() const override { return L"UnrealWindow"; }

    const char* default_settings() const override { return probe_default_settings(); }

    void configure() override { load_profile(); }

    std::vector<ViewInfo> views() const override {
        std::vector<ViewInfo> out;
        for (const View& v : g_views) out.push_back({v.id, v.label, v.parent});
        return out;
    }

    EngineFrame read_frame() override {
        check_profile_file();
        g_scene = read_scene();
        const Scene& s = g_scene;
        engine_check();
        probe_hotkey(s);
        if (probe_on()) probe_keys(s);
        auto_probe_watch();
        g_base = base_view(s);
        g_view = active_view(s, g_base);
        if (g_view) diagnostics(*g_view, s);
        const uint32_t view_id = g_view ? g_view->id : 0;
        const std::string target = ue::name_of(ue::class_of(s.view_target));
        // Logged for the base view; switches between Match=Camera views are logged by camera_match_frame.
        const uint32_t base_id = g_base ? g_base->id : 0;
        if (base_id != g_last_view) {
            g_last_view = base_id;
            log("view: %s (target %s)", g_base ? g_base->label.c_str() : "none", target.c_str());
        }
        // Match=Camera fast path: did the game put back last frame's write into this view? Its rotation now is also
        // this frame's "before". Read before the gyro writes (apply).
        g_write = {};
        double now_pitch = 0;
        g_write.read_back = g_view && g_match.active() && read_view_rotation(*g_view, s, now_pitch, g_write.now_yaw);
        if (g_write.read_back && g_write_back.view == g_view->id)
            g_write.rewritten = CameraMatch::rewritten(g_write_back.before, g_write_back.after, g_write.now_yaw,
                                                       g_write_back.written);
        g_write_back = {};
        EngineFrame f;
        f.in_game = s.pc != 0;
        f.paused = s.paused;
        f.cursor = s.cursor;
        f.view_id = view_id;
        f.base_view_id = base_id;
        f.target = target;
        return f;
    }

    bool views_changed() override { return std::exchange(g_views_changed, false); }

    bool hold_gyro() const override { return probe_capturing(); }

    bool apply(double pitch, double yaw) override {
        if (!g_view) return false;
        g_write.yaw = yaw, g_write.pitch = pitch;  // written, or lost (carried over to the next view)
        if (!unreal::apply(*g_view, g_scene, pitch, yaw)) {
            g_write.failed = true;
            return false;
        }
        double after_pitch = 0, after_yaw = 0;
        if (g_write.read_back && read_view_rotation(*g_view, g_scene, after_pitch, after_yaw))
            g_write_back = {g_view->id, g_write.now_yaw, after_yaw, yaw};
        return true;
    }

    // The rotation apply adds to (ControlRotation, a profile's Property= or its first Offsets=), for the stick fallback.
    // Not Match=Camera views (a rewritten rotation makes the camera match try the next one) nor WorldRotation ones.
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (!g_view || g_view->world_rotation || g_view->match_camera || !read_view_rotation(*g_view, g_scene, pitch, yaw))
            return false;
        camera = view_object(*g_view, g_scene) ^ (uintptr_t(g_view->id) << 48);
        return true;
    }

    // Every view whose rotation can be read (the generic ControlRotation, a profile's Property= or Offsets=) can be
    // recentered, unless its profile says Recenter=0. Custom cameras are best effort: their level may not be 0, see
    // RecenterPitch=.
    Recenter recenter_support(std::string& detail) override {
        if (!g_view) {
            detail = "(no view)";
            return Recenter::NotYet;
        }
        double pitch = 0, yaw = 0;
        if (!g_view->recenter) {
            detail = std::format("'{}' (Recenter=0 in the profile)", g_view->label);
            return Recenter::No;
        }
        if (!read_view_rotation(*g_view, g_scene, pitch, yaw)) {
            detail = std::format("'{}' (its rotation can't be read yet)", g_view->label);
            return Recenter::NotYet;
        }
        detail = std::format("'{}' (level pitch {:.1f})", g_view->label, g_view->recenter_pitch);
        return Recenter::Yes;
    }

    // Through the same writes as the gyro (all raw offsets, WorldRotation, OnGyro). UE keeps pitch as 0..360
    // (350 = 10 degrees up), hence the wrap.
    bool recenter(double fraction) override {
        double pitch = 0, yaw = 0;
        if (!g_view || !read_view_rotation(*g_view, g_scene, pitch, yaw)) return false;
        const double delta = wrap180(g_view->recenter_pitch - pitch) * fraction;
        return delta == 0 || unreal::apply(*g_view, g_scene, delta, 0);
    }

    void after_frame(double gyro_deg) override {
        camera_match_frame(g_scene, g_view, g_write.yaw, g_write.pitch, g_write.failed, g_write.rewritten);
        follow_check(g_scene, g_view, gyro_deg > 0 ? std::abs(g_write.yaw) : 0);
        if (probe_on()) auto_probe(g_scene);
    }

    std::string status_scene() const override { return unreal::status_scene(g_scene, g_view); }
    void status_details() override { unreal::status_details(g_scene, g_view); }
};

}  // namespace

Engine& engine() {
    static UnrealEngine instance;
    return instance;
}

}  // namespace gyro4all::unreal
