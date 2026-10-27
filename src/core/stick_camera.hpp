#pragma once
// Stick camera (StickCamera=1): Gyro4All turns the camera with the right stick itself, for games whose own stick turn
// is capped (too slow even at full deflection). No mouse is emulated, so the game keeps its controller prompts: the game
// is given a centred right stick (the controller shown as XInput, or the game's own PlayStation reports; the stick click
// is left alone) and the stick's turn is written in degrees through Engine::apply, like the gyro's. Engine-neutral, no
// game memory access: host.cpp decides when it may turn (gameplay view, no flick stick) and passes the physical stick.
//
// - Radial deadzone: the deflection past it is rescaled to 0..1 and raised to the curve exponent; the direction is
//   kept, so diagonals turn no faster than the axes.
// - Edge acceleration: while the deflection stays at or past the edge, the speed ramps from speed to edge_speed over
//   edge_ramp_ms (both axes by the same factor); it starts over when the stick leaves the edge.
#include <cstdint>

namespace gyro4all {

class StickCamera {
public:
    struct Settings {
        double speed = 360;         // yaw at full deflection, degrees/s
        double speed_pitch = 200;   // pitch at full deflection, degrees/s
        double deadzone = 0.10;     // 0..1
        double curve = 2.0;         // response exponent (1 = linear)
        double edge = 0.90;         // deflection where the acceleration starts, 0..1
        double edge_speed = 720;    // yaw after the ramp, degrees/s (pitch scaled alike)
        double edge_ramp_ms = 400;  // time to reach edge_speed at the edge
        bool invert_y = false;      // stick up turns the camera down
    };

    void configure(const Settings& s);
    const Settings& settings() const { return s_; }
    // One frame: the stick (x right, y up, -1..1) and a monotonic clock. Returns this frame's turn in degrees (GyroLib's
    // sense: pitch up, yaw right), already integrated over the frame time (at most kMaxDt; none on the first frame).
    void turn(float x, float y, uint64_t now_ns, double& pitch, double& yaw);
    // The stick camera stopped turning (gate closed): the next frame starts without frame time or edge ramp.
    void reset();

    static constexpr double kMaxDt = 0.1;  // seconds: a stall or a pause doesn't turn in one jump

private:
    Settings s_;
    uint64_t last_ns_ = 0;
    double edge_s_ = 0;  // seconds held at the edge
};

}  // namespace gyro4all
