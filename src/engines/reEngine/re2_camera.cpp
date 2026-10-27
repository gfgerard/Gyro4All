// Resident Evil 2 and 3 remakes (RT builds, TDB 70; RE2 classes in app.ropeway, RE3's in offline): the camera system
// (camera.CameraSystem, a singleton) names the controller in use (<BusyCameraController>). The player's own cameras,
// camera.PlayerCameraController (free look) and camera.PlayerSightCameraController (aiming), both derive from
// camera.TwirlerCameraControllerRoot, which keeps the stick-driven angles as two floats, <Pitch> and <Yaw> (radians),
// and turns them into the camera every frame. The gyro adds to them, so the game's own pitch limits apply. Found with
// the runtime TdbDump (MODLOG 2026-10-17): the Xbox/Microsoft Store exe can't be read from disk.
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <cmath>
#include <format>
#include <numbers>

namespace gyro4all::re {

namespace {

constexpr double kRadians = std::numbers::pi / 180;

class Re2Camera final : public Camera {
public:
    const char* game() const override {
        return re3_ ? "Resident Evil 3 (offline.camera.CameraSystem)" : "Resident Evil 2 (app.ropeway.camera.CameraSystem)";
    }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        // RE2's namespace, then RE3's.
        for (const char* ns : {"app.ropeway.camera", "offline.camera"}) {
            t_.camera_system = find_type(tdb, ns, "CameraSystem");
            t_.twirler = find_type(tdb, ns, "TwirlerCameraControllerRoot");
            t_.player = find_type(tdb, ns, "PlayerCameraController");
            t_.sight = find_type(tdb, ns, "PlayerSightCameraController");
            t_.root = find_type(tdb, ns, "CameraControllerRoot");
            re3_ = std::string(ns) == "offline.camera";
            if (t_.camera_system && t_.twirler && t_.player) break;
        }
        if (!t_.camera_system || !t_.twirler || !t_.player) return false;
        t_.busy = field_offset(tdb, t_.camera_system, "<BusyCameraController>k__BackingField");
        t_.controllable = field_offset(tdb, t_.twirler, "<IsControllable>k__BackingField");
        t_.pitch = field_offset(tdb, t_.twirler, "<Pitch>k__BackingField");
        t_.yaw = field_offset(tdb, t_.twirler, "<Yaw>k__BackingField");
        t_.rotation = field_offset(tdb, t_.root, "<CameraRotation>k__BackingField");
        t_.open_pause = field_offset(tdb, t_.player, "<IsOpenPause>k__BackingField");
        t_.yaw_min = field_offset(tdb, t_.twirler, "<YawMin>k__BackingField");
        t_.yaw_max = field_offset(tdb, t_.twirler, "<YawMax>k__BackingField");
        t_.pitch_min = field_offset(tdb, t_.twirler, "<PitchMin>k__BackingField");
        t_.pitch_max = field_offset(tdb, t_.twirler, "<PitchMax>k__BackingField");
        log("re: scene types: CameraSystem %u (BusyCameraController +0x%x), TwirlerCameraControllerRoot %u "
            "(IsControllable +0x%x, Pitch +0x%x, Yaw +0x%x), CameraControllerRoot %u (CameraRotation +0x%x), "
            "PlayerCameraController %u (IsOpenPause +0x%x), PlayerSightCameraController %u",
            t_.camera_system, t_.busy, t_.twirler, t_.controllable, t_.pitch, t_.yaw, t_.root, t_.rotation, t_.player,
            t_.open_pause, t_.sight);
        return t_.busy >= 0 && t_.controllable >= 0 && t_.pitch >= 0 && t_.yaw >= 0 && t_.rotation >= 0;
    }

    bool find_singletons(const std::vector<uintptr_t>& slots) override {
        for (uintptr_t slot : slots)
            if (object_type(*tdb_, mem::rdp(slot)) == t_.camera_system) camera_system_slot_ = slot;
        if (!camera_system_slot_) return false;
        log("re: CameraSystem slot at %s", mem::module_offset(camera_system_slot_).c_str());
        return true;
    }

    // One view: the player's free look and aim (both twirler cameras of the player). Fixed, gimmick, event, inventory
    // and other cameras are left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.controller != 0;
        f.paused = s_.paused;
        f.free_look = s_.free_look();
        f.target = s_.controller ? short_name(s_.controller) : "no camera";
        f.object = s_.object;
        f.system = mem::rdp(camera_system_slot_);
        f.view = std::format("{}{}{}", f.target, s_.controller && !s_.controllable ? " (not controllable)" : "",
                             s_.paused ? " (paused)" : "");
        return f;
    }

    // GyroLib: yaw right, pitch up positive.
    bool turn(double pitch, double yaw) override {
        if (!s_.free_look()) return false;
        float p = 0, y = 0;
        if (!mem::rd(s_.fields + t_.pitch, p) || !mem::rd(s_.fields + t_.yaw, y)) return false;
        p = float(p + kPitchSign * pitch * kRadians);
        y = float(std::remainder(y + kYawSign * yaw * kRadians, 2 * std::numbers::pi));
        return mem::guarded_store(s_.fields + t_.pitch, &p, sizeof(p)) &&
               mem::guarded_store(s_.fields + t_.yaw, &y, sizeof(y));
    }

    // Read now, not from read(): the core reads them back before this frame's turn.
    bool angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float p = 0, y = 0;
        if (!s_.free_look() || !mem::rd(s_.fields + t_.pitch, p) || !mem::rd(s_.fields + t_.yaw, y)) return false;
        pitch = kPitchSign * p / kRadians;
        yaw = kYawSign * y / kRadians;
        camera = s_.object;
        return true;
    }

    bool pitch(double& degrees) override {
        float p = 0;
        if (!s_.free_look() || !mem::rd(s_.fields + t_.pitch, p)) return false;
        degrees = kPitchSign * p / kRadians;
        return true;
    }

    std::string status() const override {
        if (!s_.controller) return camera_system_slot_ ? "no camera" : "no CameraSystem";
        return std::format("camera={} ctl={} pause={} Pitch={:.3f} Yaw={:.3f} rot=({:.1f}, {:.1f}) limits pitch "
                           "{:.3f}..{:.3f} yaw {:.3f}..{:.3f}",
                           short_name(s_.controller), int(s_.controllable), int(s_.paused), s_.raw_pitch, s_.raw_yaw,
                           s_.rot_pitch, s_.rot_yaw, s_.pitch_min, s_.pitch_max, s_.yaw_min, s_.yaw_max);
    }

private:
    // Which way the floats turn the camera (user-verified in RE2, MODLOG 2026-10-17). <Yaw> grows to the left; the
    // status line's rot pitch has the opposite sign to <Pitch> (the controller's quaternion looks down -Z).
    static constexpr double kPitchSign = 1, kYawSign = -1;

    struct Types {
        uint32_t camera_system = 0, twirler = 0, player = 0, sight = 0, root = 0;
        int32_t busy = -1;                                 // CameraSystem.<BusyCameraController>
        int32_t controllable = -1, pitch = -1, yaw = -1;   // TwirlerCameraControllerRoot
        int32_t rotation = -1;                             // CameraControllerRoot.<CameraRotation>, for the status line
        int32_t open_pause = -1;                           // PlayerCameraController.<IsOpenPause>, a ValueTrigger`1<bool>
        int32_t yaw_min = -1, yaw_max = -1;                // Twirler <YawMin>/<YawMax> (floats, radians)
        int32_t pitch_min = -1, pitch_max = -1;            // Twirler <PitchMin>/<PitchMax> (DampingFloat, value at +0)
    };

    struct Scene {
        uint32_t controller = 0;  // type of the busy camera controller
        uintptr_t object = 0;     // the busy camera controller
        uintptr_t fields = 0;     // where its fields start
        bool player = false;      // a player twirler camera (free look or aim)
        bool controllable = false, paused = false;
        float raw_pitch = 0, raw_yaw = 0;
        float yaw_min = 0, yaw_max = 0, pitch_min = 0, pitch_max = 0;  // the game's limits (pitch: DampingFloat.<Current>)
        double rot_yaw = 0, rot_pitch = 0;
        bool free_look() const { return player && controllable && !paused && fields; }
    };

    Scene read_scene() {
        Scene s;
        const uintptr_t system = mem::rdp(camera_system_slot_);
        const int32_t system_base = field_base(system);
        if (!system || system_base < 0) return s;
        const uintptr_t controller = mem::rdp(system + system_base + t_.busy);
        const int32_t base = field_base(controller);
        s.controller = object_type(*tdb_, controller);
        if (!s.controller || base < 0) return s;
        s.object = controller;
        s.fields = controller + base;
        s.player = is_a(*tdb_, s.controller, t_.player) || (t_.sight && is_a(*tdb_, s.controller, t_.sight));
        uint8_t flag = 0;
        s.controllable = mem::rd(s.fields + t_.controllable, flag) && flag;
        mem::rd(s.fields + t_.pitch, s.raw_pitch);
        mem::rd(s.fields + t_.yaw, s.raw_yaw);
        if (t_.yaw_min >= 0) mem::rd(s.fields + t_.yaw_min, s.yaw_min), mem::rd(s.fields + t_.yaw_max, s.yaw_max);
        if (t_.pitch_min >= 0) {
            mem::rd(mem::rdp(s.fields + t_.pitch_min) + field_base(mem::rdp(s.fields + t_.pitch_min)), s.pitch_min);
            mem::rd(mem::rdp(s.fields + t_.pitch_max) + field_base(mem::rdp(s.fields + t_.pitch_max)), s.pitch_max);
        }
        read_rotation(s.fields + t_.rotation, s.rot_yaw, s.rot_pitch);
        if (t_.open_pause >= 0 && is_a(*tdb_, s.controller, t_.player)) s.paused = open_pause(s.fields + t_.open_pause);
        return s;
    }

    // The value of PlayerCameraController.<IsOpenPause> (a ValueTrigger`1<bool>: its field by name, found once).
    bool open_pause(uintptr_t field) {
        const uintptr_t trigger = mem::rdp(field);
        const int32_t base = field_base(trigger);
        if (!trigger || base < 0) return false;
        if (value_offset_ < 0 && !value_logged_) {
            const uint32_t type = object_type(*tdb_, trigger);
            for (const char* name : {"<Value>k__BackingField", "_Value", "Value", "<Current>k__BackingField", "_Current"})
                if (value_offset_ < 0) value_offset_ = field_offset(*tdb_, type, name);
            value_logged_ = true;
            log("re: IsOpenPause is a %s, value +0x%x", type_name(*tdb_, type).c_str(), value_offset_);
        }
        uint8_t flag = 0;
        return value_offset_ >= 0 && mem::rd(trigger + base + value_offset_, flag) && flag;
    }

    std::string short_name(uint32_t type) const {
        const std::string name = type ? type_name(*tdb_, type) : "none";
        return name.substr(name.find_last_of('.') + 1);
    }

    const Tdb* tdb_ = nullptr;
    bool re3_ = false;
    Types t_;
    uintptr_t camera_system_slot_ = 0;
    int32_t value_offset_ = -1;
    bool value_logged_ = false;
    Scene s_;
};

}  // namespace

std::unique_ptr<Camera> make_re2_camera() { return std::make_unique<Re2Camera>(); }

}  // namespace gyro4all::re
