// RE Requiem (TDB 83): app.CameraSystem (a static singleton) keeps one app.CameraBlender per camera role; the main one
// (role 0) names the camera controller in use (<BusyCameraController>). The player's own cameras are
// app.PlayerTPSCameraController and app.PlayerFPSCameraController; each has an ...ActionInterpolation component whose
// _RotationInterpolation._NextInfo (app.PlayerCameraRotationCalc) holds <CameraRotation>, the quaternion the game turns
// into the camera every frame (checked live: a write there turns the camera and stays). Pitch limits: the rotation
// interpolation's _PitchRangeDeg, a via.Range (start, size). Pause: app.PauseManager's _CurrentPauseTypeBits. Its
// pitch is the other way round from RE7's (user-verified: inverted with RE7's sign), its yaw is the same.
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <format>

namespace gyro4all::re {

namespace {

constexpr double kPitchSign = -1;

class RequiemCamera final : public Camera {
public:
    const char* game() const override { return "RE Requiem (app.CameraSystem)"; }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.camera_system = find_type(tdb, "app", "CameraSystem");
        t_.tps = find_type(tdb, "app", "PlayerTPSCameraController");
        t_.fps = find_type(tdb, "app", "PlayerFPSCameraController");
        if (!t_.camera_system || !t_.tps || !t_.fps) return false;
        t_.pause_manager = find_type(tdb, "app", "PauseManager");
        const uint32_t blender = find_type(tdb, "app", "CameraBlender");
        t_.tps_interpolation = find_type(tdb, "app", "PlayerCameraTPSActionInterpolation");
        t_.fps_interpolation = find_type(tdb, "app", "PlayerCameraFPSActionInterpolation");
        const uint32_t rotation_interpolation = find_type(tdb, "app", "PlayerCameraRotationInterpolation");
        const uint32_t rotation_calc = find_type(tdb, "app", "PlayerCameraRotationCalc");
        t_.blenders = field_offset(tdb, t_.camera_system, "_CameraBlenders");
        t_.role = field_offset(tdb, blender, "_Role");
        t_.busy = field_offset(tdb, blender, "<BusyCameraController>k__BackingField");
        t_.tps_rotation_interpolation = field_offset(tdb, t_.tps_interpolation, "_RotationInterpolation");
        t_.fps_rotation_interpolation = field_offset(tdb, t_.fps_interpolation, "_RotationInterpolation");
        t_.next_info = field_offset(tdb, rotation_interpolation, "_NextInfo");
        t_.pitch_range = field_offset(tdb, rotation_interpolation, "_PitchRangeDeg");
        t_.camera_rotation = field_offset(tdb, rotation_calc, "<CameraRotation>k__BackingField");
        t_.pause_bits = field_offset(tdb, t_.pause_manager, "_CurrentPauseTypeBits");
        log("re: scene types: CameraSystem %u (_CameraBlenders +0x%x), CameraBlender %u (_Role +0x%x, "
            "BusyCameraController +0x%x), PlayerTPSCameraController %u, PlayerFPSCameraController %u, TPS/FPS "
            "ActionInterpolation %u/%u (_RotationInterpolation +0x%x/+0x%x), PlayerCameraRotationInterpolation %u "
            "(_NextInfo +0x%x, _PitchRangeDeg +0x%x), PlayerCameraRotationCalc %u (CameraRotation +0x%x), PauseManager "
            "%u (_CurrentPauseTypeBits +0x%x)",
            t_.camera_system, t_.blenders, blender, t_.role, t_.busy, t_.tps, t_.fps, t_.tps_interpolation,
            t_.fps_interpolation, t_.tps_rotation_interpolation, t_.fps_rotation_interpolation, rotation_interpolation,
            t_.next_info, t_.pitch_range, rotation_calc, t_.camera_rotation, t_.pause_manager, t_.pause_bits);
        return t_.pause_manager && t_.tps_interpolation && t_.fps_interpolation && t_.blenders >= 0 && t_.role >= 0 &&
               t_.busy >= 0 && t_.tps_rotation_interpolation >= 0 && t_.fps_rotation_interpolation >= 0 &&
               t_.next_info >= 0 && t_.pitch_range >= 0 && t_.camera_rotation >= 0 && t_.pause_bits >= 0;
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

    // One view: the player's own third- or first-person camera. Every other controller (aiming, events, gimmicks,
    // fixed and detail-search cameras, photo mode) is left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.controller != 0;
        f.paused = s_.pause_bits != 0;
        f.free_look = s_.rotation != 0;
        f.target = s_.controller ? short_name(s_.controller) : "no camera";
        f.view = s_.pause_bits ? std::format("{} (paused, bits 0x{:x})", f.target, s_.pause_bits) : f.target;
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
        return std::format("camera={} pause=0x{:x}{}", short_name(s_.controller), s_.pause_bits,
                           known ? std::format(" yaw={:.1f} pitch={:.1f} range={:.0f}..{:.0f}", yaw, pitch,
                                               s_.pitch_lower, s_.pitch_upper)
                                 : std::string{});
    }

private:
    struct Types {
        uint32_t camera_system = 0, pause_manager = 0, tps = 0, fps = 0, tps_interpolation = 0, fps_interpolation = 0;
        int32_t blenders = -1, role = -1, busy = -1;                              // CameraSystem, CameraBlender
        int32_t tps_rotation_interpolation = -1, fps_rotation_interpolation = -1;  // ...ActionInterpolation
        int32_t next_info = -1, pitch_range = -1;                                 // PlayerCameraRotationInterpolation
        int32_t camera_rotation = -1;                                             // PlayerCameraRotationCalc
        int32_t pause_bits = -1;                                                  // PauseManager
    };

    struct Scene {
        uint32_t controller = 0;      // type of the main blender's controller in use
        uintptr_t rotation = 0;       // the player camera's <CameraRotation> (0: not the player's own camera)
        double pitch_lower = -60, pitch_upper = 60;
        uint64_t pause_bits = 0;
    };

    // Field address of a managed object (its field base plus offset), 0 if unreadable.
    static uintptr_t field(uintptr_t obj, int32_t offset) {
        const int32_t base = field_base(obj);
        return obj && base >= 0 ? obj + base + offset : 0;
    }

    Scene read_scene() {
        Scene s;
        const uintptr_t pauses = mem::rdp(pause_manager_slot_);
        if (const uintptr_t bits = field(pauses, t_.pause_bits)) mem::rd(bits, s.pause_bits);
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
        const bool tps = is_a(*tdb_, s.controller, t_.tps), fps = !tps && is_a(*tdb_, s.controller, t_.fps);
        if (!tps && !fps) return s;
        // The interpolation component is looked up again when the controller changes or no longer has it.
        if (controller != cached_controller_ ||
            mem::rdp(cached_interpolation_ + kComponentOwner) != mem::rdp(controller + kComponentOwner)) {
            cached_controller_ = controller;
            cached_interpolation_ = find_component(*tdb_, mem::rdp(controller + kComponentOwner),
                                                   tps ? t_.tps_interpolation : t_.fps_interpolation);
        }
        const uintptr_t interpolation = mem::rdp(
            field(cached_interpolation_, tps ? t_.tps_rotation_interpolation : t_.fps_rotation_interpolation));
        const uintptr_t calc = interpolation ? mem::rdp(field(interpolation, t_.next_info)) : 0;
        s.rotation = calc ? field(calc, t_.camera_rotation) : 0;
        float range[2];  // via.Range: start, size
        if (s.rotation && mem::rd(field(interpolation, t_.pitch_range), range) && range[1] > 0) {
            s.pitch_lower = range[0];
            s.pitch_upper = range[0] + range[1];
        }
        return s;
    }

    // "PlayerTPSCameraController" for app.PlayerTPSCameraController.
    std::string short_name(uint32_t type) const {
        const std::string name = type ? type_name(*tdb_, type) : "none";
        return name.substr(name.find_last_of('.') + 1);
    }

    const Tdb* tdb_ = nullptr;
    Types t_;
    uintptr_t camera_system_slot_ = 0, pause_manager_slot_ = 0;
    Scene s_;
    uintptr_t cached_controller_ = 0, cached_interpolation_ = 0;
};

}  // namespace

std::unique_ptr<Camera> make_requiem_camera() { return std::make_unique<RequiemCamera>(); }

}  // namespace gyro4all::re
