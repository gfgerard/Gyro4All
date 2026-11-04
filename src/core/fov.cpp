#include "fov.hpp"

#include "log.hpp"
#include "settings.hpp"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace gyro4all::fov {

double reference_setting() {
    const std::string text = settings::get_string(L"ZoomReferenceFOV", L"0");
    const double fov = std::atof(text.c_str());
    if (fov == 0 || is_valid(fov)) return fov > 0 ? fov : 0;
    static std::string logged;
    if (logged != text) {
        logged = text;
        log("settings: ZoomReferenceFOV=%s is not a field of view (degrees, 1-178): ignored", text.c_str());
    }
    return 0;
}

double game_window_aspect() {
    static ULONGLONG next = 0;
    static double aspect = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return aspect;
    next = now + 500;
    struct Search {
        HWND best = nullptr;
        long long area = 0;
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM p) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
            RECT r{};
            if (!GetClientRect(hwnd, &r)) return TRUE;
            const long long area = 1ll * (r.right - r.left) * (r.bottom - r.top);
            if (area > s.area) s.best = hwnd, s.area = area;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    RECT r{};
    aspect = search.best && GetClientRect(search.best, &r) && r.right > 0 && r.bottom > 0 ? double(r.right) / r.bottom : 0;
    return aspect;
}

}  // namespace gyro4all::fov
