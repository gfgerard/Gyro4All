#include "notify.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace gyro4all::notify {
namespace {

constexpr UINT kShowMessage = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr int kMargin = 14;  // text padding inside the box, pixels

std::mutex g_mutex;
std::wstring g_text;            // guarded by g_mutex
ULONGLONG g_hide_at = 0;        // guarded by g_mutex
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_enabled{true}, g_started{false};
HFONT g_font = nullptr;
int g_font_height = 0;

struct GameWindowSearch {
    HWND self, best = nullptr;
    long long best_area = 0;
};

// The game's main window: the largest visible top-level window of this process.
HWND game_window(HWND self) {
    GameWindowSearch search{self};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<GameWindowSearch*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (hwnd == s.self || pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
            RECT r{};
            if (!GetClientRect(hwnd, &r)) return TRUE;
            const long long area = 1ll * (r.right - r.left) * (r.bottom - r.top);
            if (area > s.best_area) s.best = hwnd, s.best_area = area;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

bool game_in_foreground(HWND self) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return fg && fg != self && pid == GetCurrentProcessId();
}

// Font scaled to the game window (about 1/40 of its height, 16-36 px).
void update_font(int client_height) {
    const int height = std::clamp(client_height / 40, 16, 36);
    if (g_font && height == g_font_height) return;
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_font_height = height;
}

// Places the box at the top centre of the game's client area, or hides it.
void place(HWND self) {
    std::wstring text;
    bool expired = false;
    {
        std::lock_guard lock(g_mutex);
        text = g_text;
        expired = GetTickCount64() >= g_hide_at;
    }
    HWND game = game_window(self);
    if (text.empty() || expired || !game || !game_in_foreground(self)) {
        if (IsWindowVisible(self)) ShowWindow(self, SW_HIDE);
        if (expired) KillTimer(self, kTimer);
        return;
    }
    RECT client{};
    GetClientRect(game, &client);
    POINT origin{0, 0};
    ClientToScreen(game, &origin);
    const int game_w = client.right - client.left, game_h = client.bottom - client.top;
    update_font(game_h);

    HDC dc = GetDC(self);
    HGDIOBJ old = SelectObject(dc, g_font);
    RECT measure{0, 0, std::max(200, game_w * 2 / 3), 0};
    DrawTextW(dc, text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(self, dc);
    const int w = measure.right + 2 * kMargin, h = measure.bottom + 2 * kMargin;
    const int x = origin.x + (game_w - w) / 2, y = origin.y + std::max(16, game_h / 20);
    SetWindowPos(self, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(self, nullptr, TRUE);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kShowMessage:
        SetTimer(hwnd, kTimer, 250, nullptr);  // follows the game window and the foreground state
        place(hwnd);
        return 0;
    case WM_TIMER:
        place(hwnd);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        HBRUSH background = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(dc, &r, background);
        DeleteObject(background);
        HBRUSH accent = CreateSolidBrush(RGB(64, 156, 255));  // thin bar on the left: "this is Gyro4All"
        RECT bar{0, 0, 4, r.bottom};
        FillRect(dc, &bar, accent);
        DeleteObject(accent);
        std::wstring text;
        {
            std::lock_guard lock(g_mutex);
            text = g_text;
        }
        HGDIOBJ old = SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 242, 245));
        RECT inner{kMargin, kMargin, r.right - kMargin, r.bottom - kMargin};
        DrawTextW(dc, text.c_str(), -1, &inner, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

DWORD WINAPI notice_thread(void*) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Gyro4AllNotice";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // Layered (semi-transparent), click-through, never activated, no taskbar button.
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"Gyro4All", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        log("notify: window not created (error %lu); on-screen notices off", GetLastError());
        g_enabled = false;
        return 0;
    }
    SetLayeredWindowAttributes(hwnd, 0, 225, LWA_ALPHA);
    g_window = hwnd;
    PostMessageW(hwnd, kShowMessage, 0, 0);  // a notice may already be waiting
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

void set_enabled(bool enabled) { g_enabled = enabled; }

void show(const std::wstring& text, int seconds) {
    if (!g_enabled) return;
    {
        std::lock_guard lock(g_mutex);
        g_text = text;
        g_hide_at = GetTickCount64() + ULONGLONG(seconds) * 1000;
    }
    if (!g_started.exchange(true)) {
        if (HANDLE thread = CreateThread(nullptr, 0, &notice_thread, nullptr, 0, nullptr)) CloseHandle(thread);
        else g_enabled = false;
        return;  // the thread shows the waiting notice once its window exists
    }
    if (HWND hwnd = g_window.load()) PostMessageW(hwnd, kShowMessage, 0, 0);
}

}  // namespace gyro4all::notify
