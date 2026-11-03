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
// Onimusha: Way of the Sword (TDB 82, checked in the demo) is on the same ace camera framework: its app.CameraManager has
// no _MasterPlCamera but an app.cInGameCameraOperator (_InGameCameraOperator) whose _GameCamera, an
// app.GameCameraController, is the player's camera. There BaseCamParam is rebuilt every frame from the base param
// worker's current param (_BaseParamWorker, an ace.cCameraParamWorker, ._CurrentParam): that one keeps the turn (found by
// write tests in game: BaseCamParam, PrevCameraParam, CurrentParam, AddCamParam, DirectCamParam and InputRot._AddDeg
// don't). While the camera blends (bow aim in and out), the worker rebuilds _CurrentParam from its work points, so the
// turn also goes into them and the goal param (turn_blend). Same signs as MH Wilds. Pitch limit: the game's
// _MiniComponent.DegLimit (app.mcCam_DynamicDegLimit) ._StaticPitchLimit (-70..45 in the demo). The gyro leaves the camera
// to the game unless _CameraMode is NORMAL, <IsActiveLayer> is set and the lock-on camera (_MiniComponent.LockOn,
// app.mcCam_CameraLockOn) isn't _IsActive: lock-on aims at the target and rewrites the turn.
#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace gyro4all::re {

namespace {

constexpr double kYawSign = -1, kPitchSign = 1;  // _AttachDeg change for a turn right / a look up

class MhWildsCamera final : public Camera {
public:
    const char* game() const override {
        return onimusha() ? "Onimusha: Way of the Sword (app.GameCameraController)"
                          : "Monster Hunter Wilds (app.PlayerCameraController)";
    }

    bool resolve(const Tdb& tdb) override {
        tdb_ = &tdb;
        t_.manager = find_type(tdb, "app", "CameraManager");
        const uint32_t param = find_type(tdb, "ace", "cCameraParam");
        const uint32_t basic = find_type(tdb, "app.user_data", "PlayerCameraBasicParam");
        if (!t_.manager || !param) return false;
        t_.master = field_offset(tdb, t_.manager, "_MasterPlCamera");
        if (t_.master >= 0) {
            t_.player = find_type(tdb, "app", "PlayerCameraController");
        } else if (!resolve_onimusha(tdb)) {
            return false;  // another game's app.CameraManager
        }
        if (!t_.player) return false;
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
        return (t_.event >= 0 || onimusha()) && t_.base_param >= 0 && t_.argument >= 0 && t_.eye >= 0 && t_.look >= 0 &&
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
        f.free_look = s_.rotation != 0 && !s_.event && (!onimusha() || player_camera());
        f.target = s_.controller ? short_name(s_.argument_type) : "no camera";
        f.view = std::format("{}{}{}{}", f.target, s_.event ? " (event camera)" : "",
                             s_.pause ? std::format(" (paused, 0x{:x})", s_.pause) : std::string{},
                             onimusha() && s_.controller
                                 ? std::format(" (mode {}{}{})", s_.mode, s_.active_layer ? "" : ", layer inactive",
                                               s_.locked_on ? ", locked on" : "")
                                 : std::string{});
        if (onimusha() && s_.limit_read && (s_.pitch_lower != logged_lower_ || s_.pitch_upper != logged_upper_)) {
            logged_lower_ = s_.pitch_lower;
            logged_upper_ = s_.pitch_upper;
            log("re: pitch limit %.1f..%.1f (mcCam_DynamicDegLimit._StaticPitchLimit)", s_.pitch_lower, s_.pitch_upper);
        }
        return f;
    }

    // As addRotationDegree: both attach settings' _AttachDeg, the pitch kept within the camera's range (never pushed
    // further out), the yaw in 0..360.
    bool turn(double pitch, double yaw) override {
        if (!s_.rotation) return false;
        const double x = limited_pitch(pitch);
        double y = std::fmod(s_.y + kYawSign * yaw, 360.0);
        if (y < 0) y += 360;
        const float out[2] = {float(x), float(y)};
        if (!mem::guarded_store(s_.rotation, out, sizeof(out))) return false;
        if (s_.look_rotation) mem::guarded_store(s_.look_rotation, out, sizeof(out));
        if (onimusha()) turn_blend(x - s_.x, kYawSign * yaw);
        s_.x = out[0];
        s_.y = out[1];
        return true;
    }

    // The pitch kept within the camera's range, as turn does, so a turn stopped there isn't judged as undone.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        if (s_.rotation) pitch = kPitchSign * (limited_pitch(pitch) - s_.x);
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
        if (onimusha())
            out += std::format(" mode={} layer={} locked_on={}", s_.mode, s_.active_layer ? 1 : 0, s_.locked_on ? 1 : 0);
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
        // Onimusha: CameraManager._InGameCameraOperator, cInGameCameraOperator._GameCamera; GameCameraController's
        // _CameraMode, <IsActiveLayer>, _BaseParamWorker (cCameraParamWorker._CurrentParam) and _MiniComponent (an
        // inline MINI_COMPONENT_HOLDER: LockOn, DegLimit, BaseParam).
        int32_t camera_operator = -1, game_camera = -1;
        int32_t mode = -1, active_layer = -1, worker = -1, worker_current = -1, mini = -1;
        int32_t lock_on = -1, lock_active = -1;        // holder.LockOn, mcCam_CameraLockOn._IsActive
        int32_t deg_limit = -1, static_limit = -1;     // holder.DegLimit, mcCam_DynamicDegLimit._StaticPitchLimit
        // The blend: cCameraParamWorker._WorkPointList (List`1 _items, _size) and _PreviousWorkPoint, each
        // cCameraParamWorkPointBase's Start/End/CurrentCameraParam; holder.BaseParam (app.mcCam_BaseWorker, an
        // mcCam_CameraParamSelector) ._GoalParam.
        int32_t work_points = -1, previous_point = -1, list_items = -1, list_size = -1;
        int32_t point_start = -1, point_end = -1, point_current = -1;
        int32_t base_selector = -1, goal_param = -1;
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
        int32_t mode = -1;          // Onimusha: GameCameraController._CameraMode
        bool active_layer = false;  // Onimusha: <IsActiveLayer>
        bool locked_on = false;     // Onimusha: the lock-on camera is active
        bool limit_read = false;    // Onimusha: the pitch range is the game's _StaticPitchLimit
    };

    bool resolve_onimusha(const Tdb& tdb) {
        const uint32_t op = find_type(tdb, "app", "cInGameCameraOperator");
        t_.camera_operator = field_offset(tdb, t_.manager, "_InGameCameraOperator");
        t_.game_camera = op ? field_offset(tdb, op, "_GameCamera") : -1;
        t_.player = find_type(tdb, "app", "GameCameraController");
        if (t_.camera_operator < 0 || t_.game_camera < 0 || !t_.player) return false;
        const uint32_t worker = find_type(tdb, "ace", "cCameraParamWorker");
        const uint32_t holder = find_nested_type(tdb, "app.GameCameraController.MINI_COMPONENT_HOLDER");
        const uint32_t lock_on = find_type(tdb, "app", "mcCam_CameraLockOn");
        const uint32_t limit = find_type(tdb, "app", "mcCam_DynamicDegLimit");
        const uint32_t list = find_type(tdb, "System.Collections.Generic", "List`1");
        const uint32_t point = find_type(tdb, "ace", "cCameraParamWorkPointBase");
        const uint32_t selector = find_type(tdb, "app", "mcCam_CameraParamSelector");
        t_.mode = field_offset(tdb, t_.player, "_CameraMode");
        t_.active_layer = field_offset(tdb, t_.player, "<IsActiveLayer>k__BackingField");
        t_.worker = field_offset(tdb, t_.player, "_BaseParamWorker");
        t_.worker_current = worker ? field_offset(tdb, worker, "_CurrentParam") : -1;
        t_.mini = field_offset(tdb, t_.player, "_MiniComponent");
        t_.lock_on = holder ? field_offset(tdb, holder, "LockOn") : -1;
        t_.lock_active = lock_on ? field_offset(tdb, lock_on, "_IsActive") : -1;
        t_.deg_limit = holder ? field_offset(tdb, holder, "DegLimit") : -1;
        t_.static_limit = limit ? field_offset(tdb, limit, "_StaticPitchLimit") : -1;
        t_.work_points = worker ? field_offset(tdb, worker, "_WorkPointList") : -1;
        t_.previous_point = worker ? field_offset(tdb, worker, "_PreviousWorkPoint") : -1;
        t_.list_items = list ? field_offset(tdb, list, "_items") : -1;
        t_.list_size = list ? field_offset(tdb, list, "_size") : -1;
        t_.point_start = point ? field_offset(tdb, point, "<StartCameraParam>k__BackingField") : -1;
        t_.point_end = point ? field_offset(tdb, point, "<EndCameraParam>k__BackingField") : -1;
        t_.point_current = point ? field_offset(tdb, point, "<CurrentCameraParam>k__BackingField") : -1;
        t_.base_selector = holder ? field_offset(tdb, holder, "BaseParam") : -1;
        t_.goal_param = selector ? field_offset(tdb, selector, "_GoalParam") : -1;
        log("re: scene types (Onimusha): _InGameCameraOperator +0x%x, _GameCamera +0x%x, _CameraMode +0x%x, "
            "<IsActiveLayer> +0x%x, _BaseParamWorker +0x%x (_CurrentParam +0x%x), _MiniComponent +0x%x (LockOn +0x%x "
            "_IsActive +0x%x, DegLimit +0x%x _StaticPitchLimit +0x%x)",
            t_.camera_operator, t_.game_camera, t_.mode, t_.active_layer, t_.worker, t_.worker_current, t_.mini,
            t_.lock_on, t_.lock_active, t_.deg_limit, t_.static_limit);
        return t_.worker >= 0 && t_.worker_current >= 0;
    }

    bool onimusha() const { return t_.master < 0; }

    // Onimusha: the player turns the camera (MODE_ID NORMAL, its layer active, not locked on).
    bool player_camera() const { return s_.mode == 0 && s_.active_layer && !s_.locked_on; }

    // The pitch after a turn, kept within the camera's range (never pushed further out).
    double limited_pitch(double pitch) const {
        const double lower = std::min(s_.pitch_lower, s_.pitch_upper), upper = std::max(s_.pitch_lower, s_.pitch_upper);
        return std::clamp(s_.x + kPitchSign * pitch, std::min(lower, s_.x), std::max(upper, s_.x));
    }

    // Onimusha: the same turn (pitch, yaw: _AttachDeg changes) in every param of the worker's blend and the goal param,
    // each _AttachDeg once (eye and look-at).
    void turn_blend(double pitch, double yaw) {
        std::vector<uintptr_t> params;
        auto point = [&](uintptr_t p) {
            if (!p) return;
            for (int32_t offset : {t_.point_start, t_.point_end, t_.point_current})
                if (const uintptr_t param = mem::rdp(field(p, offset))) params.push_back(param);
        };
        if (const uintptr_t worker = mem::rdp(field(s_.controller, t_.worker))) {
            point(mem::rdp(field(worker, t_.previous_point)));
            const uintptr_t list = mem::rdp(field(worker, t_.work_points));
            const uintptr_t items = list ? mem::rdp(field(list, t_.list_items)) : 0;
            int32_t size = 0;
            if (items && mem::rd(field(list, t_.list_size), size))
                for (int32_t i = 0; i < std::min(size, 8); ++i) point(mem::rdp(items + kArrayItems + 8 * uintptr_t(i)));
        }
        const uintptr_t holder = field(s_.controller, t_.mini);
        const uintptr_t selector = holder && t_.base_selector >= 0 ? mem::rdp(holder + t_.base_selector) : 0;
        if (const uintptr_t goal = selector ? mem::rdp(field(selector, t_.goal_param)) : 0) params.push_back(goal);
        std::vector<uintptr_t> done = {s_.rotation, s_.look_rotation};
        for (uintptr_t param : params)
            for (int32_t setting : {t_.eye, t_.look}) {
                const uintptr_t at = attach_deg(param, setting);
                if (!at || std::find(done.begin(), done.end(), at) != done.end()) continue;
                done.push_back(at);
                float v[2];
                if (!mem::rd(at, v) || !std::isfinite(v[0]) || !std::isfinite(v[1])) continue;
                v[0] += float(pitch);
                v[1] += float(yaw);
                mem::guarded_store(at, v, sizeof(v));
            }
    }

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
        if (t_.event >= 0 && mem::rd(field(manager, t_.event), event)) s.event = event != 0;
        uintptr_t controller = 0;
        if (!onimusha()) controller = mem::rdp(field(manager, t_.master));
        else if (const uintptr_t op = mem::rdp(field(manager, t_.camera_operator)))
            controller = mem::rdp(field(op, t_.game_camera));
        if (!is_a(*tdb_, object_type(*tdb_, controller), t_.player)) return s;
        s.controller = controller;
        uintptr_t param = mem::rdp(field(controller, t_.base_param));
        if (onimusha()) {
            uint8_t flag = 0;
            mem::rd(field(controller, t_.mode), s.mode);
            if (mem::rd(field(controller, t_.active_layer), flag)) s.active_layer = flag != 0;
            const uintptr_t holder = field(controller, t_.mini);
            const uintptr_t lock_on = holder && t_.lock_on >= 0 ? mem::rdp(holder + t_.lock_on) : 0;
            if (lock_on && mem::rd(field(lock_on, t_.lock_active), flag)) s.locked_on = flag != 0;
            const uintptr_t limit = holder && t_.deg_limit >= 0 ? mem::rdp(holder + t_.deg_limit) : 0;
            float range[2];  // lower, upper
            if (limit && mem::rd(field(limit, t_.static_limit), range) && std::isfinite(range[0]) &&
                std::isfinite(range[1]) && range[1] - range[0] > 10 && range[0] > -90 && range[1] < 90) {
                s.pitch_lower = range[0];
                s.pitch_upper = range[1];
                s.limit_read = true;
            }
            // The worker's current param keeps the turn (BaseCamParam is rebuilt from it).
            s.argument_type = param ? object_type(*tdb_, mem::rdp(field(param, t_.argument))) : 0;
            const uintptr_t worker = mem::rdp(field(controller, t_.worker));
            param = worker ? mem::rdp(field(worker, t_.worker_current)) : 0;
        } else {
            s.argument_type = param ? object_type(*tdb_, mem::rdp(field(param, t_.argument))) : 0;
        }
        // MH Wilds: only with an argument, the player's free-look camera.
        const uintptr_t at = param && (s.argument_type || onimusha()) ? attach_deg(param, t_.eye) : 0;
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
    double logged_lower_ = 0, logged_upper_ = 0;  // Onimusha: the pitch limit last logged
};

}  // namespace

std::unique_ptr<Camera> make_mhwilds_camera() { return std::make_unique<MhWildsCamera>(); }

}  // namespace gyro4all::re
