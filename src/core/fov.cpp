#include "fov.hpp"

#include <windows.h>

namespace gyro4all::fov {

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
