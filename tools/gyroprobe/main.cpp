// gyroprobe: headless GyroLib + SDL reader. Prints accumulated gyro yaw/pitch and reader
// diagnostics, so we can check sensor access while a game holds the same controller.
// Usage: gyroprobe [seconds=60] | gyroprobe --settings (list GyroLib settings and choices)
#include <gyrolib/gyrolib.hpp>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    const bool list_settings = argc > 1 && std::string(argv[1]) == "--settings";
    const double seconds = argc > 1 && !list_settings ? std::atof(argv[1]) : 60.0;
    try {
        gyrolib::Context gyro;
        gyro.register_view(1, "Camera");
        if (list_settings) {
            const uint32_t n = gl_menu_setting_count(gyro.get());
            for (uint32_t i = 0; i < n; ++i) {
                gl_setting_info s{};
                if (gl_setting_at(gyro.get(), i, &s) != GL_OK) continue;
                std::printf("%-40s %-34s = %-8g [%g..%g] %s\n", s.id, s.label ? s.label : "", s.value,
                            s.minimum, s.maximum, s.unit ? s.unit : "");
                const uint32_t c = gl_menu_choice_count(gyro.get(), s.id);
                for (uint32_t j = 0; j < c; ++j) {
                    gl_choice ch{};
                    if (gl_choice_at(gyro.get(), s.id, j, &ch) == GL_OK)
                        std::printf("%44s %g = %s\n", "", ch.value, ch.label ? ch.label : "");
                }
            }
            return 0;
        }
        gl_set_gamepad_menu_shortcut(gyro.get(), 0);
        gl_set_auto_calibration_allowed(gyro.get(), 1);
        gyrolib::SdlInput input(gyro);

        const uint64_t start = SDL_GetTicksNS();
        uint64_t next_print = start;
        double yaw = 0, pitch = 0;
        std::string last_error;
        while (true) {
            const uint64_t now = SDL_GetTicksNS();
            if ((now - start) / 1e9 > seconds) break;
            gl_host_state host{};
            host.focused = 1;           // headless: always treat as focused
            host.camera_allowed = 1;
            const gl_output out = input.update(now, host, 1);
            yaw += out.yaw_degrees;
            pitch += out.pitch_degrees;

            if (last_error != input.error()) {
                last_error = input.error();
                if (!last_error.empty()) std::printf("[error] %s\n", last_error.c_str());
            }
            if (now >= next_print) {
                next_print = now + 500'000'000ull;
                gl_diagnostics d{};
                gl_get_diagnostics(gyro.get(), &d);
                std::printf("t=%5.1fs yaw=%8.2f pitch=%7.2f active=%u src=%u accepted=%llu rejected=%llu "
                            "dropped=%llu calib=%u still=%u dps=(%.1f %.1f %.1f) endpoints=",
                            (now - start) / 1e9, yaw, pitch, out.gyro_active, d.source,
                            (unsigned long long)d.accepted_samples, (unsigned long long)d.rejected_samples,
                            (unsigned long long)d.dropped_samples, d.calibration_state, d.stationary,
                            d.calibrated_dps.x, d.calibrated_dps.y, d.calibrated_dps.z);
                const uint32_t n = gl_endpoint_count(gyro.get());
                for (uint32_t i = 0; i < n; ++i) {
                    gl_endpoint e{};
                    if (gl_get_endpoint(gyro.get(), i, &e) == GL_OK)
                        std::printf("[%s%s gyro=%u] ", e.name, e.connected ? "" : " (off)", e.caps.gyro);
                }
                std::printf("\n");
                std::fflush(stdout);
            }
            SDL_Delay(4);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fatal: %s\n", error.what());
        return 1;
    }
    return 0;
}
