// Resident Evil 7 (TDB 70) and Resident Evil Village (RE8, TDB 69): app.PlayerCamera, a component of the player's
// GameObject in RE7; in RE8 held by the main camera GameObject's app.OnPauseCameraCaller. Free look is the solver's
// MaximumOperatable controller; the gyro rotates that controller's rotation, which the game turns into the camera every
// frame. Checked live: MODLOG 2026-10-15 (RE7), 2026-10-17 (RE8, found with
// tools/reEngine/camera_finder.lua). RE8 renames the fields (the second name in each pair below), keeps the player in
// app.PropsManager instead of app.ObjectManager, has no IsPause (app.HIDVibrationManager's pause flags instead) and its
// solver's currentType is an object, not RE7's CameraTypeEnum (free look then by the controller's type alone).
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <format>

namespace gyro4all::re {

namespace {

class Re7Camera final : public Camera {
public:
    const char* game() const override {
        return t_.re8 ? "Resident Evil Village (app.PlayerCamera)" : "Resident Evil 7 (app.PlayerCamera)";
    }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.player_camera = find_type(tdb, "app", "PlayerCamera");
        if (!t_.player_camera) return false;
        // The player's GameObject: RE7 app.ObjectManager.PlayerObj, RE8 app.PropsManager.<Player>.
        t_.manager = find_type(tdb, "app", "ObjectManager");
        t_.player_obj = field_offset(tdb, t_.manager, "PlayerObj");
        if (t_.player_obj < 0) {
            t_.manager = find_type(tdb, "app", "PropsManager");
            t_.player_obj = field_offset(tdb, t_.manager, "<Player>k__BackingField");
            t_.re8 = t_.player_obj >= 0;
            t_.props_camera = field_offset(tdb, t_.manager, "<Camera>k__BackingField");
            t_.pause_caller = find_type(tdb, "app", "OnPauseCameraCaller");
            t_.caller_camera = field_offset(tdb, t_.pause_caller, "playerCamera");
            log("re: RE8 camera path: PropsManager.<Camera> +0x%x -> its GameObject -> OnPauseCameraCaller %u "
                "(playerCamera +0x%x)", t_.props_camera, t_.pause_caller, t_.caller_camera);
        }
        t_.solver = field_offset(tdb, t_.player_camera, "BaseTransSolver");
        t_.camera_rotation = either(tdb, t_.player_camera, "<CameraRotation>k__BackingField", "<cameraRotation>k__BackingField");
        t_.by_animation = field_offset(tdb, t_.player_camera, "<IsControlledByAnimation>k__BackingField");
        t_.gun_aim = either(tdb, t_.player_camera, "IsGunAim", "<isGunAim>k__BackingField");
        t_.paused = field_offset(tdb, t_.player_camera, "IsPause");
        t_.vibration = find_type(tdb, "app", "HIDVibrationManager");
        t_.old_pause = field_offset(tdb, t_.vibration, "IsOldPause");
        t_.old_partial_pause = field_offset(tdb, t_.vibration, "IsOldPartialPause");
        const uint32_t solver_type = find_nested_type(tdb, "app.PlayerCamera.BaseTransformSolver");
        const uint32_t controller_type = find_nested_type(tdb, "app.PlayerCamera.CameraController");
        t_.free_look = find_nested_type(tdb, "app.PlayerCamera.MaximumOperatable");
        // RE7's currentType is an enum (0 = MaximumOperatable); RE8's is an app.PlayerCamera.CameraType object.
        if (find_nested_type(tdb, "app.PlayerCamera.CameraTypeEnum"))
            t_.solver_type = field_offset(tdb, solver_type, "<currentType>k__BackingField");
        t_.solver_controller = either(tdb, solver_type, "CurrentController", "<CurrentController>k__BackingField");
        t_.rotation = either(tdb, controller_type, "<rotation>k__BackingField", "<Rotation>k__BackingField");
        t_.lower_limit = field_offset(tdb, controller_type, "XRotateLowerLimit");
        t_.upper_limit = field_offset(tdb, controller_type, "XRotateUpperLimit");
        log("re: scene types: %s %u (player +0x%x), PlayerCamera %u (BaseTransSolver +0x%x, CameraRotation +0x%x, "
            "IsControlledByAnimation +0x%x, IsGunAim +0x%x, IsPause +0x%x), HIDVibrationManager %u (IsOldPause +0x%x, "
            "IsOldPartialPause +0x%x), BaseTransformSolver %u (currentType enum +0x%x, CurrentController +0x%x), "
            "CameraController %u (rotation +0x%x, XRotateLowerLimit +0x%x, XRotateUpperLimit +0x%x), MaximumOperatable %u",
            t_.re8 ? "PropsManager" : "ObjectManager", t_.manager, t_.player_obj, t_.player_camera, t_.solver,
            t_.camera_rotation, t_.by_animation, t_.gun_aim, t_.paused, t_.vibration, t_.old_pause, t_.old_partial_pause,
            solver_type, t_.solver_type, t_.solver_controller, controller_type, t_.rotation, t_.lower_limit,
            t_.upper_limit, t_.free_look);
        // RE7 needs its IsPause and currentType; RE8 has neither (its pause flags are optional).
        const bool re7_parts = t_.re8 ? t_.props_camera >= 0 && t_.pause_caller && t_.caller_camera >= 0
                                      : t_.paused >= 0 && t_.solver_type >= 0;
        return t_.free_look && t_.player_obj >= 0 && t_.solver >= 0 && t_.camera_rotation >= 0 &&
               t_.by_animation >= 0 && t_.gun_aim >= 0 && re7_parts && t_.solver_controller >= 0 && t_.rotation >= 0 &&
               t_.lower_limit >= 0 && t_.upper_limit >= 0;
    }

    bool find_singletons(const std::vector<uintptr_t>& slots) override {
        for (uintptr_t slot : slots) {
            const uint32_t type = object_type(*tdb_, mem::rdp(slot));
            if (type == t_.manager) manager_slot_ = slot;
            if (t_.vibration && type == t_.vibration) vibration_slot_ = slot;
        }
        if (!manager_slot_) return false;
        log("re: app.%s slot at %s", t_.re8 ? "PropsManager" : "ObjectManager", mem::module_offset(manager_slot_).c_str());
        if (t_.re8)
            log("re: app.HIDVibrationManager slot %s",
                vibration_slot_ ? ("at " + mem::module_offset(vibration_slot_)).c_str() : "not found: no pause detection");
        return true;
    }

    // One view, the player's free look (MaximumOperatable); scripted cameras (animations, elastic or pivot rotation
    // in events, quick turns) are left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.camera != 0;
        f.paused = s_.paused;
        f.free_look = s_.free_look();
        f.target = s_.camera ? short_name(s_.controller) : (s_.player ? "player without PlayerCamera" : "no player");
        f.view = s_.camera ? std::format("{} (type {}{}{})", f.target, s_.solver_type,
                                         s_.by_animation ? ", animation" : "", s_.paused ? ", paused" : "")
                           : f.target;
        return f;
    }

    // The controller's XRotateLower/UpperLimit (RE7: -58 / 75) keep the pitch in range.
    bool turn(double pitch, double yaw) override {
        if (!s_.free_look()) return false;
        float lower = -80, upper = 80;
        mem::rd(s_.controller_fields + t_.lower_limit, lower);
        mem::rd(s_.controller_fields + t_.upper_limit, upper);
        return turn_rotation(s_.controller_fields + t_.rotation, pitch, yaw, lower, upper);
    }

    bool pitch(double& degrees) override {
        double yaw = 0;
        return s_.free_look() && read_rotation(s_.controller_fields + t_.rotation, yaw, degrees);
    }

    // read_rotation's yaw grows to the left.
    bool angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (!s_.free_look() || !read_rotation(s_.controller_fields + t_.rotation, yaw, pitch)) return false;
        yaw = -yaw;
        camera = s_.controller_fields;
        return true;
    }

    std::string status() const override {
        if (!s_.camera) return s_.player ? "player without PlayerCamera" : "no player";
        return std::format("camera={} type={} anim={} aim={} pause={} yaw={:.1f} pitch={:.1f}", short_name(s_.controller),
                           s_.solver_type, int(s_.by_animation), int(s_.aiming), int(s_.paused), s_.yaw, s_.pitch);
    }

private:
    struct Types {
        bool re8 = false;                                               // the player comes from app.PropsManager
        uint32_t manager = 0, player_camera = 0, free_look = 0;         // free_look: app.PlayerCamera.MaximumOperatable
        int32_t player_obj = -1;                                        // manager: RE7 ObjectManager, RE8 PropsManager
        int32_t props_camera = -1;                                      // RE8 PropsManager.<Camera> (via.Camera)
        uint32_t pause_caller = 0;                                      // RE8 app.OnPauseCameraCaller, on that camera's
        int32_t caller_camera = -1;                                     //   GameObject: its playerCamera
        int32_t solver = -1, camera_rotation = -1;                      // app.PlayerCamera
        int32_t by_animation = -1, gun_aim = -1, paused = -1;           // paused: RE7's app.DoomsBehavior.IsPause
        uint32_t vibration = 0;                                         // RE8 app.HIDVibrationManager:
        int32_t old_pause = -1, old_partial_pause = -1;                 //   last frame's pause, partial pause
        int32_t solver_type = -1, solver_controller = -1;               // app.PlayerCamera.BaseTransformSolver
        int32_t rotation = -1, lower_limit = -1, upper_limit = -1;      // app.PlayerCamera.CameraController
    };

    // The offset of the first of two field names (RE7's, then RE8's) that exists, else -1.
    static int32_t either(const Tdb& tdb, uint32_t type, const char* first, const char* second) {
        const int32_t offset = field_offset(tdb, type, first);
        return offset >= 0 ? offset : field_offset(tdb, type, second);
    }

    struct Scene {
        uintptr_t player = 0, camera = 0;  // via.GameObject, app.PlayerCamera
        int32_t solver_type = -1;          // RE7's app.PlayerCamera.CameraTypeEnum, 0 = MaximumOperatable (RE8: not read)
        uint32_t controller = 0;           // type of the solver's current camera controller
        uintptr_t controller_fields = 0;   // where its fields start
        bool operatable = false;           // the controller is MaximumOperatable (or a DLC subclass)
        bool by_animation = false, aiming = false, paused = false, rotation_ok = false;
        double yaw = 0, pitch = 0;         // <CameraRotation>, degrees
        bool type_checked = true;          // false in RE8: no enum, the controller's type decides
        bool free_look() const {
            return operatable && controller_fields && (!type_checked || solver_type == 0) && !by_animation;
        }
    };

    Scene read_scene() {
        Scene s;
        const uintptr_t manager = mem::rdp(manager_slot_);
        const int32_t manager_base = field_base(manager);
        s.player = manager_base >= 0 ? mem::rdp(manager + manager_base + t_.player_obj) : 0;
        if (!s.player) return s;
        if (t_.re8) {
            // RE8: app.PlayerCamera is not on the player; reached from the main camera's GameObject (PropsManager's
            // <Camera>), whose app.OnPauseCameraCaller holds it (the path camera_finder.lua confirmed).
            const uintptr_t main_camera = mem::rdp(manager + manager_base + t_.props_camera);
            const uintptr_t camera_object = main_camera ? mem::rdp(main_camera + kComponentOwner) : 0;
            if (camera_object != cached_player_ || !cached_camera_ ||
                !is_a(*tdb_, object_type(*tdb_, cached_camera_), t_.player_camera)) {
                cached_player_ = camera_object;
                const uintptr_t caller = find_component(*tdb_, camera_object, t_.pause_caller);
                const int32_t caller_base = field_base(caller);
                const uintptr_t camera = caller_base >= 0 ? mem::rdp(caller + caller_base + t_.caller_camera) : 0;
                cached_camera_ = is_a(*tdb_, object_type(*tdb_, camera), t_.player_camera) ? camera : 0;
            }
        } else if (s.player != cached_player_ || mem::rdp(cached_camera_ + kComponentOwner) != s.player) {
            // The component is looked up again when the player object changes or the cached one no longer belongs to it.
            cached_player_ = s.player;
            cached_camera_ = find_component(*tdb_, s.player, t_.player_camera);
        }
        s.camera = cached_camera_;
        const int32_t base = field_base(s.camera);
        if (!s.camera || base < 0) return s;
        const uintptr_t fields = s.camera + base;
        uint8_t flag = 0;
        s.by_animation = mem::rd(fields + t_.by_animation, flag) && flag;
        s.aiming = mem::rd(fields + t_.gun_aim, flag) && flag;
        if (t_.paused >= 0) s.paused = mem::rd(fields + t_.paused, flag) && flag;
        // RE8: the vibration manager's copy of last frame's pause and partial pause (inventory, map).
        const uintptr_t vibration = vibration_slot_ ? mem::rdp(vibration_slot_) : 0;
        if (const int32_t vibration_base = field_base(vibration); vibration && vibration_base >= 0) {
            for (const int32_t offset : {t_.old_pause, t_.old_partial_pause})
                if (offset >= 0 && mem::rd(vibration + vibration_base + offset, flag) && flag) s.paused = true;
        }
        s.rotation_ok = read_rotation(fields + t_.camera_rotation, s.yaw, s.pitch);
        const uintptr_t solver = mem::rdp(fields + t_.solver);
        const int32_t solver_base = field_base(solver);
        if (solver_base >= 0) {
            s.type_checked = t_.solver_type >= 0;
            if (s.type_checked) mem::rd(solver + solver_base + t_.solver_type, s.solver_type);
            const uintptr_t controller = mem::rdp(solver + solver_base + t_.solver_controller);
            const int32_t controller_base = field_base(controller);
            s.controller = object_type(*tdb_, controller);
            if (controller_base >= 0) s.controller_fields = controller + controller_base;
            s.operatable = is_a(*tdb_, s.controller, t_.free_look);
        }
        return s;
    }

    // "ElasticRotation" for app.PlayerCamera.ElasticRotation.
    std::string short_name(uint32_t type) const {
        const std::string name = type ? type_name(*tdb_, type) : "none";
        return name.substr(name.find_last_of('.') + 1);
    }

    const Tdb* tdb_ = nullptr;
    Types t_;
    uintptr_t manager_slot_ = 0, vibration_slot_ = 0;
    Scene s_;
    uintptr_t cached_player_ = 0, cached_camera_ = 0;
};

}  // namespace

std::unique_ptr<Camera> make_re7_camera() { return std::make_unique<Re7Camera>(); }

}  // namespace gyro4all::re
