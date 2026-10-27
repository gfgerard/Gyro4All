// Resident Evil 4 remake (TDB 71; classes from the TDB, offline with tools/reEngine/tdbdump.py; checked live by the user).
// The camera system (chainsaw.CameraSystem, a singleton) keeps one chainsaw.MainCameraController per camera role
// (_CameraControllerList; the only role is Main = 0), which names the controller in use (_BusyCameraController). The
// player's own cameras, chainsaw.PlayerCameraController (on foot, free look and aim) and chainsaw.VehicleCameraController
// (the boat), derive from chainsaw.TwirlerCameraControllerRoot, which keeps the stick-driven angles as two floats,
// _Yaw and _Pitch (radians), like RE2's twirler cameras; the gyro adds to them, so the game's own pitch limits apply.
// Pause: share.PauseManager's _CurrentTargetPauseUpdate (bits of the paused update targets).
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <cmath>
#include <format>
#include <numbers>

namespace gyro4all::re {

namespace {

constexpr double kRadians = std::numbers::pi / 180;

class Re4Camera final : public Camera {
public:
    const char* game() const override { return "Resident Evil 4 (chainsaw.CameraSystem)"; }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.camera_system = find_type(tdb, "chainsaw", "CameraSystem");
        t_.twirler = find_type(tdb, "chainsaw", "TwirlerCameraControllerRoot");
        if (!t_.camera_system || !t_.twirler) return false;
        t_.main = find_type(tdb, "chainsaw", "MainCameraController");
        t_.root = find_type(tdb, "chainsaw", "CameraControllerRoot");
        t_.pause_manager = find_type(tdb, "share", "PauseManager");
        t_.controllers = field_offset(tdb, t_.camera_system, "_CameraControllerList");
        t_.role = field_offset(tdb, t_.main, "_Role");
        t_.busy = field_offset(tdb, t_.main, "_BusyCameraController");
        t_.pitch = field_offset(tdb, t_.twirler, "_Pitch");
        t_.yaw = field_offset(tdb, t_.twirler, "_Yaw");
        t_.input_control = field_offset(tdb, t_.root, "_IsInputControl");
        t_.rotation = field_offset(tdb, t_.root, "_CameraRotation");
        t_.pause_bits = field_offset(tdb, t_.pause_manager, "_CurrentTargetPauseUpdate");
        log("re: scene types: CameraSystem %u (_CameraControllerList +0x%x), MainCameraController %u (_Role +0x%x, "
            "_BusyCameraController +0x%x), TwirlerCameraControllerRoot %u (_Pitch +0x%x, _Yaw +0x%x), "
            "CameraControllerRoot %u (_IsInputControl +0x%x, _CameraRotation +0x%x), PauseManager %u "
            "(_CurrentTargetPauseUpdate +0x%x)",
            t_.camera_system, t_.controllers, t_.main, t_.role, t_.busy, t_.twirler, t_.pitch, t_.yaw, t_.root,
            t_.input_control, t_.rotation, t_.pause_manager, t_.pause_bits);
        return t_.main && t_.root && t_.pause_manager && t_.controllers >= 0 && t_.role >= 0 && t_.busy >= 0 &&
               t_.pitch >= 0 && t_.yaw >= 0 && t_.input_control >= 0 && t_.rotation >= 0 && t_.pause_bits >= 0;
    }

    bool find_singletons(const std::vector<uintptr_t>& slots) override {
        for (uintptr_t slot : slots) {
            const uint32_t type = object_type(*tdb_, mem::rdp(slot));
            if (type == t_.camera_system) camera_system_slot_ = slot;
            if (type == t_.pause_manager) pause_manager_slot_ = slot;
        }
        if (!camera_system_slot_ || !pause_manager_slot_) return false;
        log("re: chainsaw.CameraSystem slot at %s, share.PauseManager slot at %s",
            mem::module_offset(camera_system_slot_).c_str(), mem::module_offset(pause_manager_slot_).c_str());
        return true;
    }

    // One view: the player's twirler cameras (on foot and the boat). Event, gimmick, inventory, merchant, photo mode
    // and other cameras are left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.controller != 0;
        f.paused = s_.pause_bits != 0;
        f.free_look = s_.free_look();
        f.target = s_.controller ? short_name(s_.controller) : "no camera";
        f.object = s_.object;
        f.system = mem::rdp(camera_system_slot_);
        f.view = std::format("{}{}{}", f.target, s_.twirler && !s_.input_control ? " (no input control)" : "",
                             s_.pause_bits ? std::format(" (paused, bits 0x{:x})", s_.pause_bits) : "");
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
        return std::format("camera={} input={} pause=0x{:x} Pitch={:.3f} Yaw={:.3f} rot=({:.1f}, {:.1f})",
                           short_name(s_.controller), int(s_.input_control), s_.pause_bits, s_.raw_pitch, s_.raw_yaw,
                           s_.rot_pitch, s_.rot_yaw);
    }

private:
    // Which way the floats turn the camera: RE2's twirler signs (<Yaw> grows to the left), also right in RE4 (user).
    static constexpr double kPitchSign = 1, kYawSign = -1;

    struct Types {
        uint32_t camera_system = 0, main = 0, twirler = 0, root = 0, pause_manager = 0;
        int32_t controllers = -1;                  // CameraSystem._CameraControllerList (MainCameraController[])
        int32_t role = -1, busy = -1;              // MainCameraController._Role, _BusyCameraController
        int32_t pitch = -1, yaw = -1;              // TwirlerCameraControllerRoot._Pitch, _Yaw (radians)
        int32_t input_control = -1, rotation = -1;  // CameraControllerRoot._IsInputControl, _CameraRotation
        int32_t pause_bits = -1;                   // PauseManager._CurrentTargetPauseUpdate
    };

    struct Scene {
        uint32_t controller = 0;  // type of the main camera's busy controller
        uintptr_t object = 0;     // that controller
        uintptr_t fields = 0;     // where its fields start
        bool twirler = false;     // one of the player's twirler cameras
        bool input_control = false;
        uint32_t pause_bits = 0;
        float raw_pitch = 0, raw_yaw = 0;
        double rot_yaw = 0, rot_pitch = 0;
        // _IsInputControl is shown in the status line but not required: its meaning in play is not checked yet.
        bool free_look() const { return twirler && !pause_bits && fields; }
    };

    // Field address of a managed object (its field base plus offset), 0 if unreadable.
    static uintptr_t field(uintptr_t obj, int32_t offset) {
        const int32_t base = field_base(obj);
        return obj && base >= 0 ? obj + base + offset : 0;
    }

    Scene read_scene() {
        Scene s;
        if (const uintptr_t bits = field(mem::rdp(pause_manager_slot_), t_.pause_bits)) mem::rd(bits, s.pause_bits);
        const uintptr_t system = mem::rdp(camera_system_slot_);
        const uintptr_t list = system ? mem::rdp(field(system, t_.controllers)) : 0;
        int32_t count = 0;
        if (!list || !mem::rd(list + kArrayCount, count)) return s;
        uintptr_t main = 0;
        for (int32_t i = 0; i < count && i < 8 && !main; ++i) {
            const uintptr_t controller = mem::rdp(list + kArrayItems + 8 * i);
            int32_t role = -1;
            if (controller && mem::rd(field(controller, t_.role), role) && role == 0) main = controller;
        }
        const uintptr_t controller = main ? mem::rdp(field(main, t_.busy)) : 0;
        const int32_t base = field_base(controller);
        s.controller = object_type(*tdb_, controller);
        if (!s.controller || base < 0) return s;
        s.object = controller;
        s.fields = controller + base;
        s.twirler = is_a(*tdb_, s.controller, t_.twirler);
        uint8_t flag = 0;
        s.input_control = mem::rd(s.fields + t_.input_control, flag) && flag;
        mem::rd(s.fields + t_.pitch, s.raw_pitch);
        mem::rd(s.fields + t_.yaw, s.raw_yaw);
        read_rotation(s.fields + t_.rotation, s.rot_yaw, s.rot_pitch);
        return s;
    }

    // "PlayerCameraController" for chainsaw.PlayerCameraController.
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

std::unique_ptr<Camera> make_re4_camera() { return std::make_unique<Re4Camera>(); }

}  // namespace gyro4all::re
