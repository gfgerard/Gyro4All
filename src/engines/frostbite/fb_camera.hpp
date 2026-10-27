#pragma once
// Frostbite cameras: each game keeps its player camera in its own runtime classes, so the module has one camera model
// per game (like reEngine's), found from the reflected types (fb_types.hpp) or the game's code, without addresses of the
// exe. Dead Space (2023): deadspace_camera.cpp; Battlefield 4: bf4_camera.cpp; Mass Effect: Andromeda: mea_camera.cpp.
//
// A model hooks the game's camera update, reports which camera objects run and turns the active one.
#include "fb_types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gyro4all::fb {

// What the camera model found this frame, for the core.
struct CameraFrame {
    bool in_game = false;  // the player's camera runs (its update ran in the last moments)
    bool active = false;   // ... and takes the player's input: the gyro may turn it
    std::string target;    // the camera objects, for the log
};

class Camera {
public:
    virtual ~Camera() = default;
    virtual const char* game() const = 0;
    // Attach thread, once the types are found: the model's classes, vtables and code. False = not this game's camera
    // (its classes aren't there) or something didn't match; why = what, for the log.
    virtual bool resolve(const TypeTable& types, std::string& why) = 0;
    // Attach thread, after resolve: hook the game's camera update. False = not hooked; why = what, for the log.
    virtual bool install(std::string& why) = 0;
    // Game thread, once per frame.
    virtual CameraFrame read() = 0;
    // Game thread, after read: turn the active camera (degrees; yaw right, pitch up positive). The model may apply it
    // later, on the game's own camera update. True when it was taken.
    virtual bool turn(double pitch, double yaw) = 0;
    // Diagnostics (WriteWatch's "cam"): the active camera object to watch, 0 = none now. Any thread.
    virtual uintptr_t watch_address() const { return 0; }
    // The scene part of the status line.
    virtual std::string status() const = 0;
};

// Any thread (a model's camera hook): wakes Gyro4All's frame thread and waits, up to timeout_ms, for a frame that
// started after this call to finish, so the gyro's turn queued by turn() is up to date right now. False on timeout
// (the frame thread busy or not started): go on without it.
bool sync_gyro_frame(unsigned timeout_ms);

std::unique_ptr<Camera> make_deadspace_camera();
std::unique_ptr<Camera> make_bf4_camera();
std::unique_ptr<Camera> make_mea_camera();

}  // namespace gyro4all::fb
