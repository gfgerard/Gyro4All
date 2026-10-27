#pragma once
// RE Engine cameras: each game family keeps its player camera in its own classes, so the module has one camera model
// per family (RE7's app.PlayerCamera, RE2/RE3's camera.CameraSystem and RE4's chainsaw.CameraSystem twirler angles, RE Requiem's and Pragmata's app.CameraSystem, MH Wilds' app.CameraManager). Most
// end in a quaternion the game builds the camera from every frame; the gyro rotates it (turn_rotation). MH Wilds keeps
// yaw and pitch in degrees instead. Shared helpers here, models in re7_camera.cpp, re2_camera.cpp, re4_camera.cpp, requiem_camera.cpp,
// pragmata_camera.cpp and mhwilds_camera.cpp.
#include "re_tdb.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gyro4all::re {

// What the camera model found this frame, for the core.
struct CameraFrame {
    bool in_game = false;    // the player's camera exists
    bool paused = false;
    bool free_look = false;  // the player turns the camera: the gyro may too
    std::string target;      // the active camera controller, for the log
    std::string view;        // target plus its state, logged when it changes
    uintptr_t object = 0;    // the active camera object, for CameraWatch= (0 = none)
    uintptr_t system = 0;    // the object that picks it (camera system or manager), also watched
};

class Camera {
public:
    virtual ~Camera() = default;
    virtual const char* game() const = 0;
    // Attach thread: the model's types and field offsets from the TDB. False = not this game's camera.
    virtual bool resolve(const Tdb& tdb) = 0;
    // Attach thread, after resolve: its singletons, from the exe's static singleton slots (they are filled during the
    // game's startup: may be called again until it returns true).
    virtual bool find_singletons(const std::vector<uintptr_t>& slots) = 0;
    // Game thread, once per frame.
    virtual CameraFrame read() = 0;
    // Game thread, after read: turn the free-look camera (degrees; yaw right, pitch up positive). True when written.
    virtual bool turn(double pitch, double yaw) = 0;
    // The free-look camera's pitch (degrees, up positive), for recenter.
    virtual bool pitch(double& degrees) = 0;
    // The scene part of the status line.
    virtual std::string status() const = 0;
    // Game thread: Engine::view_angles, the free-look camera's angles read now (degrees in turn's sense: pitch up, yaw
    // right) and which camera they belong to. False = unknown.
    virtual bool angles(double& /*pitch*/, double& /*yaw*/, uintptr_t& /*camera*/) { return false; }
};

std::unique_ptr<Camera> make_re7_camera();
std::unique_ptr<Camera> make_re2_camera();
std::unique_ptr<Camera> make_re4_camera();
std::unique_ptr<Camera> make_requiem_camera();
std::unique_ptr<Camera> make_pragmata_camera();
std::unique_ptr<Camera> make_mhwilds_camera();

// ---- shared helpers (game thread unless noted)

// Native layouts (REFramework ReClass_Internal_RE2_TDB70.hpp / _RE9.hpp; checked live in RE7 and RE Requiem).
constexpr uintptr_t kGameObjectTransform = 0x18;  // via.GameObject -> its via.Transform (the first component)
constexpr uintptr_t kComponentOwner = 0x10;       // via.Component -> its via.GameObject
constexpr uintptr_t kComponentNext = 0x18;        // via.Component -> the GameObject's next component
constexpr uintptr_t kArrayCount = 0x1C;           // managed array: element count, elements from 0x20
constexpr uintptr_t kArrayItems = 0x20;

// The first component of the GameObject that is (or derives from) type, 0 if none.
uintptr_t find_component(const Tdb& tdb, uintptr_t game_object, uint32_t type);
// The type of one of the TDB's nested classes by full name (e.g. "app.PlayerCamera.MaximumOperatable"), 0 if absent.
// Attach thread: walks every type.
uint32_t find_nested_type(const Tdb& tdb, const char* full_name);

// Forward of a via.Quaternion (x, y, z, w; +Z forward, +Y up) as yaw and pitch in degrees. Checked in RE7: a positive
// yaw turns left, a positive pitch looks up. pitch_sign -1: a camera whose pitch is the other way round (RE Requiem's
// player camera: its yaw matches RE7's, its pitch doesn't).
bool read_rotation(uintptr_t address, double& yaw, double& pitch, double pitch_sign = 1);
// Turns the quaternion at address by yaw (positive = right) about the world's up axis and pitch (positive = up) about
// the camera's own horizontal axis, the resulting pitch kept within [lower, upper] (never pushed further out, never
// pulled back in). A rotation past straight up or down would flip the camera around.
bool turn_rotation(uintptr_t address, double pitch, double yaw, double lower, double upper, double pitch_sign = 1);

// CameraWatch=1, for finding what the game's own input turns in a camera no model knows: every 500 ms, logs the fields
// of a camera object (and of the managed objects it references, one level down) that changed, by name. slot 0 or 1:
// two objects watched side by side (CameraFrame's object and system).
void watch_fields(const Tdb& tdb, int slot, uintptr_t object, uint64_t now_ms);

}  // namespace gyro4all::re
