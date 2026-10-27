// Monster Hunter Wilds (TDB 81; classes from the TDB, offline with tools/reEngine/tdbdump.py; checked live in the demo).
// app.CameraManager (a static singleton) keeps the player's camera in _MasterPlCamera, an app.PlayerCameraController.
// The free-look rotation lives in its BaseCamParam (an ace.cCameraParam): <EyeAttachSetting>k__BackingField
// (ace.cCameraParam.cAttachSetting) -> <AttachParam>k__BackingField (cAttachSetting.cAttachParam) -> _AttachDeg, a
// via.vec2 (x pitch, y yaw, degrees). The controller's get_RotationDegree returns it, addRotationDegree adds to it and
// clamps the pitch to _BasicParam._PitchDegreeRange, and their setter writes the look-at attach setting's _AttachDeg too
// (disassembled from the running game with tools/reEngine/methoddump.py; the exe's code is encrypted on disk). Checked
// live by writing it: the camera keeps the turn; +y turns left, +x looks up.
// Not these (they don't keep a turn): BaseCamParam.RotationDegree and CurrentParam.RotationDegree (rebuilt every frame
// from _AttachDeg), mcCam_LimitRotation._BeforeRotDeg, and the operator's input amounts (app.cPlayerCameraOperator:
// _MouseRotateAmount, _GyroInputAmount, _RotAmount, _PadInput; recomputed from the devices in the same pass that turns
// the camera).
// The argument (<ParamArgument>k__BackingField, e.g. app.cCameraParamArgument_ThirdPersonAttach) names the camera mode
// and is required (free look). Event cameras: CameraManager._IsEventCamera. Pause: app.PauseManager's _ActivePause bit
// set.
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace gyro4all::re {

namespace {

constexpr double kYawSign = -1, kPitchSign = 1;  // _AttachDeg change for a turn right / a look up

class MhWildsCamera final : public Camera {
public:
    const char* game() const override { return "Monster Hunter Wilds (app.PlayerCameraController)"; }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.manager = find_type(tdb, "app", "CameraManager");
        t_.player = find_type(tdb, "app", "PlayerCameraController");
        const uint32_t param = find_type(tdb, "ace", "cCameraParam");
        const uint32_t basic = find_type(tdb, "app.user_data", "PlayerCameraBasicParam");
        if (!t_.manager || !t_.player || !param) return false;
        t_.master = field_offset(tdb, t_.manager, "_MasterPlCamera");
        if (t_.master < 0) return false;  // another game's app.CameraManager
        const uint32_t setting = find_nested_type(tdb, "ace.cCameraParam.cAttachSetting");
        const uint32_t attach = find_nested_type(tdb, "ace.cCameraParam.cAttachSetting.cAttachParam");
        t_.event = field_offset(tdb, t_.manager, "_IsEventCamera");
        t_.base_param = field_offset(tdb, t_.player, "BaseCamParam");
        t_.basic_param = field_offset(tdb, t_.player, "_BasicParam");
        t_.argument = field_offset(tdb, param, "<ParamArgument>k__BackingField");
        t_.eye = field_offset(tdb, param, "<EyeAttachSetting>k__BackingField");
        t_.look = field_offset(tdb, param, "<LookAtAttachSetting>k__BackingField");
        t_.attach_param = setting ? field_offset(tdb, setting, "<AttachParam>k__BackingField") : -1;
        t_.attach_deg = attach ? field_offset(tdb, attach, "_AttachDeg") : -1;
        t_.pitch_range = basic ? field_offset(tdb, basic, "_PitchDegreeRange") : -1;
        t_.pause_manager = find_type(tdb, "app", "PauseManager");
        t_.active_pause = t_.pause_manager ? field_offset(tdb, t_.pause_manager, "_ActivePause") : -1;
        const uint32_t bitset = find_type(tdb, "ace", "Bitset");
        t_.bits = bitset ? field_offset(tdb, bitset, "_Value") : -1;
        log("re: scene types: CameraManager %u (_MasterPlCamera +0x%x, _IsEventCamera +0x%x), PlayerCameraController "
            "%u (BaseCamParam +0x%x, _BasicParam +0x%x), cCameraParam %u (ParamArgument +0x%x, <EyeAttachSetting> "
            "+0x%x, <LookAtAttachSetting> +0x%x), cAttachSetting %u (<AttachParam> +0x%x), cAttachParam %u "
            "(_AttachDeg +0x%x), PlayerCameraBasicParam %u (_PitchDegreeRange +0x%x), PauseManager %u (_ActivePause "
            "+0x%x, Bitset._Value +0x%x)",
            t_.manager, t_.master, t_.event, t_.player, t_.base_param, t_.basic_param, param, t_.argument, t_.eye,
            t_.look, setting, t_.attach_param, attach, t_.attach_deg, basic, t_.pitch_range, t_.pause_manager,
            t_.active_pause, t_.bits);
        // The pitch limits and the pause are optional (default range, never paused without them).
        return t_.event >= 0 && t_.base_param >= 0 && t_.argument >= 0 && t_.eye >= 0 && t_.look >= 0 &&
               t_.attach_param >= 0 && t_.attach_deg >= 0;
    }

    bool find_singletons(const std::vector<uintptr_t>& slots) override {
        for (uintptr_t slot : slots) {
            const uint32_t type = object_type(*tdb_, mem::rdp(slot));
            if (type == t_.manager) manager_slot_ = slot;
            if (t_.pause_manager && type == t_.pause_manager) pause_manager_slot_ = slot;
        }
        if (!manager_slot_) return false;
        log("re: app.CameraManager slot at %s, app.PauseManager slot at %s", mem::module_offset(manager_slot_).c_str(),
            pause_manager_slot_ ? mem::module_offset(pause_manager_slot_).c_str() : "(none)");
        return true;
    }

    // One view: the player camera's free look, whatever its argument (on foot, mounted, climbing...). Event cameras
    // are left to the game.
    CameraFrame read() override {
        s_ = read_scene();
        CameraFrame f;
        f.in_game = s_.controller != 0;
        f.paused = s_.pause != 0;
        f.free_look = s_.rotation != 0 && !s_.event;
        f.target = s_.controller ? short_name(s_.argument_type) : "no camera";
        f.view = std::format("{}{}{}", f.target, s_.event ? " (event camera)" : "",
                             s_.pause ? std::format(" (paused, 0x{:x})", s_.pause) : std::string{});
        return f;
    }

    // As addRotationDegree: both attach settings' _AttachDeg, the pitch kept within the camera's range (never pushed
    // further out), the yaw in 0..360.
    bool turn(double pitch, double yaw) override {
        if (!s_.rotation) return false;
        const double lower = std::min(s_.pitch_lower, s_.pitch_upper), upper = std::max(s_.pitch_lower, s_.pitch_upper);
        const double x = std::clamp(s_.x + kPitchSign * pitch, std::min(lower, s_.x), std::max(upper, s_.x));
        double y = std::fmod(s_.y + kYawSign * yaw, 360.0);
        if (y < 0) y += 360;
        const float out[2] = {float(x), float(y)};
        if (!mem::guarded_store(s_.rotation, out, sizeof(out))) return false;
        if (s_.look_rotation) mem::guarded_store(s_.look_rotation, out, sizeof(out));
        s_.x = out[0];
        s_.y = out[1];
        return true;
    }

    // Read now (turn keeps s_.x/y up to date only for its own writes).
    bool angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float xy[2] = {};
        if (!s_.rotation || !mem::guarded_copy(xy, s_.rotation, sizeof(xy))) return false;
        pitch = kPitchSign * xy[0];
        yaw = kYawSign * xy[1];
        camera = s_.rotation;
        return true;
    }

    // Up positive, 0 = level.
    bool pitch(double& degrees) override {
        if (!s_.rotation) return false;
        degrees = kPitchSign * s_.x;
        return true;
    }

    std::string status() const override {
        if (!s_.controller) return "no camera";
        std::string out = std::format("camera={} event={} pause=0x{:x}", short_name(s_.argument_type), s_.event ? 1 : 0,
                                      s_.pause);
        if (s_.rotation)
            out += std::format(" rot=({:.1f}, {:.1f}) range={:.0f}..{:.0f}", s_.x, s_.y, s_.pitch_lower, s_.pitch_upper);
        return out;
    }

private:
    struct Types {
        uint32_t manager = 0, player = 0, pause_manager = 0;
        int32_t master = -1, event = -1;             // CameraManager
        int32_t base_param = -1, basic_param = -1;   // PlayerCameraController
        int32_t argument = -1, eye = -1, look = -1;  // cCameraParam
        int32_t attach_param = -1, attach_deg = -1;  // cAttachSetting, cAttachParam
        int32_t pitch_range = -1;                    // PlayerCameraBasicParam
        int32_t active_pause = -1, bits = -1;        // PauseManager, ace.Bitset
    };

    struct Scene {
        uintptr_t controller = 0;     // the master player camera controller
        uint32_t argument_type = 0;   // BaseCamParam's argument type (the camera mode)
        uintptr_t rotation = 0;       // the eye attach setting's _AttachDeg (0: no free-look camera)
        uintptr_t look_rotation = 0;  // the look-at attach setting's
        double x = 0, y = 0;          // pitch and yaw
        double pitch_lower = -80, pitch_upper = 80;
        bool event = false;
        uint32_t pause = 0;  // the first word of the active pause bits
    };

    // Field address of a managed object (its field base plus offset), 0 if unreadable.
    static uintptr_t field(uintptr_t obj, int32_t offset) {
        const int32_t base = field_base(obj);
        return obj && base >= 0 && offset >= 0 ? obj + base + offset : 0;
    }

    // param.<attach setting>.<AttachParam>._AttachDeg.
    uintptr_t attach_deg(uintptr_t param, int32_t setting_offset) const {
        const uintptr_t setting = mem::rdp(field(param, setting_offset));
        const uintptr_t attach = setting ? mem::rdp(field(setting, t_.attach_param)) : 0;
        return attach ? field(attach, t_.attach_deg) : 0;
    }

    Scene read_scene() {
        Scene s;
        if (const uintptr_t pauses = pause_manager_slot_ ? mem::rdp(pause_manager_slot_) : 0) {
            const uintptr_t bitset = mem::rdp(field(pauses, t_.active_pause));
            const uintptr_t words = bitset ? mem::rdp(field(bitset, t_.bits)) : 0;
            int32_t count = 0;
            if (words && mem::rd(words + kArrayCount, count) && count > 0) mem::rd(words + kArrayItems, s.pause);
        }
        const uintptr_t manager = mem::rdp(manager_slot_);
        if (!manager) return s;
        uint8_t event = 0;
        if (mem::rd(field(manager, t_.event), event)) s.event = event != 0;
        const uintptr_t controller = mem::rdp(field(manager, t_.master));
        if (!is_a(*tdb_, object_type(*tdb_, controller), t_.player)) return s;
        s.controller = controller;
        const uintptr_t param = mem::rdp(field(controller, t_.base_param));
        s.argument_type = param ? object_type(*tdb_, mem::rdp(field(param, t_.argument))) : 0;
        // Only with an argument: the player's free-look camera.
        const uintptr_t at = s.argument_type ? attach_deg(param, t_.eye) : 0;
        float rotation[2];
        if (at && mem::rd(at, rotation) && std::isfinite(rotation[0]) && std::isfinite(rotation[1])) {
            s.rotation = at;
            s.look_rotation = attach_deg(param, t_.look);
            s.x = rotation[0];
            s.y = rotation[1];
        }
        const uintptr_t basic = t_.basic_param >= 0 ? mem::rdp(field(controller, t_.basic_param)) : 0;
        float range[2];  // via.Range: start, size
        if (basic && t_.pitch_range >= 0 && mem::rd(field(basic, t_.pitch_range), range) && range[1] > 0 &&
            range[1] < 180) {
            s.pitch_lower = range[0];
            s.pitch_upper = range[0] + range[1];
        }
        return s;
    }

    // "cCameraParamArgument_ThirdPersonAttach" for app.cCameraParamArgument_ThirdPersonAttach.
    std::string short_name(uint32_t type) const {
        const std::string name = type ? type_name(*tdb_, type) : "none";
        return name.substr(name.find_last_of('.') + 1);
    }

    const Tdb* tdb_ = nullptr;
    Types t_;
    uintptr_t manager_slot_ = 0, pause_manager_slot_ = 0;
    Scene s_;
};

}  // namespace

std::unique_ptr<Camera> make_mhwilds_camera() { return std::make_unique<MhWildsCamera>(); }

}  // namespace gyro4all::re
