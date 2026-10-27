// Pragmata (TDB 83): the same camera system as RE Requiem (app.CameraSystem, a static singleton, keeps one
// app.CameraBlender per camera role; the main one, role 0, names the controller in use), with other names: the
// blender's field is _BusyCameraController, and the player has one controller, app.PlayerCameraController, which holds
// its own rotation: <CameraObjectRotation>k__BackingField (inherited from app.CameraControllerRoot; checked live with
// tools/reEngine/camera_finder.lua: a write there turns the camera and stays). Pitch limits: _CameraAngleLimit's
// _DefaultPitchLimit, a via.Range (start, size). Pause: app.PauseManager's _CurrentPauseTypes. Pitch the other way round
// from RE7's, like RE Requiem's (user-verified: inverted with RE7's sign); yaw the same.
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <format>

namespace gyro4all::re {

namespace {

constexpr double kPitchSign = -1;

class PragmataCamera final : public Camera {
public:
    const char* game() const override { return "Pragmata (app.PlayerCameraController)"; }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.camera_system = find_type(tdb, "app", "CameraSystem");
        t_.player = find_type(tdb, "app", "PlayerCameraController");
        const uint32_t blender = find_type(tdb, "app", "CameraBlender");
        if (!t_.camera_system || !t_.player || !blender) return false;
        t_.busy = field_offset(tdb, blender, "_BusyCameraController");
        if (t_.busy < 0) return false;  // RE Requiem's blender spells it <BusyCameraController>k__BackingField
        t_.pause_manager = find_type(tdb, "app", "PauseManager");
        const uint32_t root = find_type(tdb, "app", "CameraControllerRoot");
        const uint32_t angle_limit = find_nested_type(tdb, "app.PlayerCameraController.CameraAngleLimit");
        t_.blenders = field_offset(tdb, t_.camera_system, "_CameraBlenders");
        t_.role = field_offset(tdb, blender, "_Role");
        t_.rotation = field_offset(tdb, root, "<CameraObjectRotation>k__BackingField");
        t_.angle_limit = field_offset(tdb, t_.player, "_CameraAngleLimit");
        t_.pitch_limit = angle_limit ? field_offset(tdb, angle_limit, "_DefaultPitchLimit") : -1;
        t_.pause_types = field_offset(tdb, t_.pause_manager, "_CurrentPauseTypes");
        log("re: scene types: CameraSystem %u (_CameraBlenders +0x%x), CameraBlender %u (_Role +0x%x, "
            "_BusyCameraController +0x%x), PlayerCameraController %u (_CameraAngleLimit +0x%x), CameraControllerRoot %u "
            "(CameraObjectRotation +0x%x), CameraAngleLimit %u (_DefaultPitchLimit +0x%x), PauseManager %u "
            "(_CurrentPauseTypes +0x%x)",
            t_.camera_system, t_.blenders, blender, t_.role, t_.busy, t_.player, t_.angle_limit, root, t_.rotation,
            angle_limit, t_.pitch_limit, t_.pause_manager, t_.pause_types);
        // The pitch limit is optional (default range without it).
        return t_.pause_manager && t_.blenders >= 0 && t_.role >= 0 && t_.rotation >= 0 && t_.pause_types >= 0;
    }

    bool find_singletons(const std::vector<uintptr_t>& slots) override {
        for (uintptr_t slot : slots) {
            const uint32_t type = object_type(*tdb_, mem::rdp(slot));
            if (type == t_.camera_system) camera_system_slot_ = slot;
            if (type == t_.pause_manager) pause_manager_slot_ = slot;
        }
        if (!camera_system_slot_ || !pause_manager_slot_) return false;
        log("re: app.CameraSystem slot at %s, app.PauseManager slot at %s",
            mem::module_offset(camera_system_slot_).c_str(), mem::module_offset(pause_manager_slot_).c_str());
        return true;
    }

    // One view: the player's own camera. Every other controller (events, gimmicks, fixed cameras, partner talks) is
    // left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.controller != 0;
        f.paused = s_.pause_types != 0;
        f.free_look = s_.rotation != 0;
        f.target = s_.controller ? short_name(s_.controller) : "no camera";
        f.view = s_.pause_types ? std::format("{} (paused, types 0x{:x})", f.target, s_.pause_types) : f.target;
        return f;
    }

    bool turn(double pitch, double yaw) override {
        if (!s_.rotation) return false;
        return turn_rotation(s_.rotation, pitch, yaw, s_.pitch_lower, s_.pitch_upper, kPitchSign);
    }

    bool pitch(double& degrees) override {
        double yaw = 0;
        return s_.rotation && read_rotation(s_.rotation, yaw, degrees, kPitchSign);
    }

    // read_rotation's yaw grows to the left.
    bool angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (!s_.rotation || !read_rotation(s_.rotation, yaw, pitch, kPitchSign)) return false;
        yaw = -yaw;
        camera = s_.rotation;
        return true;
    }

    std::string status() const override {
        if (!s_.controller) return "no camera";
        double yaw = 0, pitch = 0;
        const bool known = s_.rotation && read_rotation(s_.rotation, yaw, pitch, kPitchSign);
        return std::format("camera={} pause=0x{:x}{}", short_name(s_.controller), s_.pause_types,
                           known ? std::format(" yaw={:.1f} pitch={:.1f} range={:.0f}..{:.0f}", yaw, pitch,
                                               s_.pitch_lower, s_.pitch_upper)
                                 : std::string{});
    }

private:
    struct Types {
        uint32_t camera_system = 0, pause_manager = 0, player = 0;
        int32_t blenders = -1, role = -1, busy = -1;  // CameraSystem, CameraBlender
        int32_t rotation = -1;                        // CameraControllerRoot
        int32_t angle_limit = -1, pitch_limit = -1;   // PlayerCameraController, its CameraAngleLimit
        int32_t pause_types = -1;                     // PauseManager
    };

    struct Scene {
        uint32_t controller = 0;  // type of the main blender's controller in use
        uintptr_t rotation = 0;   // the player camera's <CameraObjectRotation> (0: not the player's own camera)
        double pitch_lower = -60, pitch_upper = 60;
        uint32_t pause_types = 0;
    };

    // Field address of a managed object (its field base plus offset), 0 if unreadable.
    static uintptr_t field(uintptr_t obj, int32_t offset) {
        const int32_t base = field_base(obj);
        return obj && base >= 0 && offset >= 0 ? obj + base + offset : 0;
    }

    Scene read_scene() {
        Scene s;
        const uintptr_t pauses = mem::rdp(pause_manager_slot_);
        if (const uintptr_t types = field(pauses, t_.pause_types)) mem::rd(types, s.pause_types);
        const uintptr_t system = mem::rdp(camera_system_slot_);
        const uintptr_t blenders = system ? mem::rdp(field(system, t_.blenders)) : 0;
        int32_t count = 0;
        if (!blenders || !mem::rd(blenders + kArrayCount, count)) return s;
        uintptr_t main = 0;
        for (int32_t i = 0; i < count && i < 8 && !main; ++i) {
            const uintptr_t blender = mem::rdp(blenders + kArrayItems + 8 * i);
            int32_t role = -1;
            if (blender && mem::rd(field(blender, t_.role), role) && role == 0) main = blender;
        }
        const uintptr_t controller = main ? mem::rdp(field(main, t_.busy)) : 0;
        s.controller = object_type(*tdb_, controller);
        if (!is_a(*tdb_, s.controller, t_.player)) return s;
        s.rotation = field(controller, t_.rotation);
        const uintptr_t limit = mem::rdp(field(controller, t_.angle_limit));
        float range[2];  // via.Range: start, size
        if (limit && mem::rd(field(limit, t_.pitch_limit), range) && range[1] > 0) {
            s.pitch_lower = range[0];
            s.pitch_upper = range[0] + range[1];
        }
        return s;
    }

    // "PlayerCameraController" for app.PlayerCameraController.
    std::string short_name(uint32_t type) const {
        const std::string name = type ? type_name(*tdb_, type) : "none";
        return name.substr(name.find_last_of('.') + 1);
    }

    const Tdb* tdb_ = nullptr;
    Types t_;
    uintptr_t camera_system_slot_ = 0, pause_manager_slot_ = 0;
    Scene s_;
};

}  // namespace

std::unique_ptr<Camera> make_pragmata_camera() { return std::make_unique<PragmataCamera>(); }

}  // namespace gyro4all::re
