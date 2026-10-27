// 4A Engine (4A Games): Metro 2033 Redux (Steam) and Metro: Last Light Redux (Epic), Next-Gen Update, metro.exe 1.0.0.4. The
// game is detected by its 4A VFS archives next to the exe (content.vfx and content*.vfs0). metro.exe is SteamStub-
// protected (.bind section) but its import directory is the real one, resolved by the loader: the core's import-table
// hooks work (PeekMessageA, XINPUT1_3 by ordinal). The game reads the DualSense itself over HID (adaptive triggers). The
// game's window class is _uengine_. Menus: the mouse cursor shown (GetCursorInfo) counts as a menu; the pause menu: the
// game clock standing still (see kClockCode); the main menu, a live scene with its own view angles: the game's main-menu
// flag (see kMenuCode).
//
// The view (MODLOG 2026-10-23): the player's view angles are `float pitch, yaw, roll` in radians at
// [[metro.exe+0xFBD820]+0x30]+0x650 in metro.exe 1.0.0.4 (2026-10-23). Yaw grows turning right and doesn't wrap; pitch
// grows looking down; roll is 0. Every other copy (a history buffer, the renderer's matrices, the driver's constant
// buffers) follows from them. The global is found by the code that loads it (kGlobalCode, a RIP-relative load), or, for
// this build only, at its known offset when both objects on the path carry this build's vtables. Neither: the gyro goes
// to the game as right-stick input (the core's blind stick fallback, through the game's own controller reports).
// Metro Exodus Enhanced Edition (MetroExodus.exe, MODLOG 2026-10-25): the same path and the same `float pitch, yaw, roll`,
// at [[global]+0x30]+0x9E0, the global found by its own code (kExodusGlobalCode). The camera object, the engine's
// camera globals and the renderer's matrices are rebuilt from them every frame.
// The pitch stops at the game's own limit (limit_turn), so pushing against it isn't taken for writes the game undid.
#include "foura.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <format>
#include <numbers>

namespace gyro4all::foura {

namespace {

constexpr double kRad = std::numbers::pi / 180.0;

// The path to the angles: the global points to an object whose +0x30 points to the one holding them at +0x650.
constexpr uintptr_t kViewOwner = 0x30, kAngles = 0x650;
// metro.exe 1.0.0.4 (2026-10-23): the global and both objects' vtables, as offsets in the exe.
constexpr uintptr_t kGlobalRva = 0xFBD820, kGlobalVtableRva = 0xB22D20, kOwnerVtableRva = 0xA81F08;
// The game's own pitch limit: straight up or down (90.0 degrees seen with the stick).
constexpr float kMaxPitch = std::numbers::pi_v<float> / 2;

// Code that loads the global and follows the path's first step (0xFF in the mask = byte must match), at
// metro.exe+0x1F4A4B in 1.0.0.4: mov rbx,[rip+global]; mov rbx,[rbx+30h]; test rbx,rbx; je. The disp32 at
// kGlobalDisp is relative to the end of the instruction, kGlobalInsnEnd bytes from the pattern's start. Unique in .text.
constexpr uint8_t kGlobalCode[] = {0x48, 0x8B, 0x1D, 0, 0, 0, 0, 0x48, 0x8B, 0x5B, 0x30, 0x48, 0x85, 0xDB, 0x74};
constexpr uint8_t kGlobalMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr size_t kGlobalDisp = 3, kGlobalInsnEnd = 7;

// Metro Exodus EE: mov rax,[rip+global]; mov rbx,rdx; mov rcx,[rax+30h]; test rcx,rcx (3 matches, MetroExodus.exe+0xA338E6
// first, all the same global, at +0x1696CE0 in the 2026-10 build). Its angles sit at kExodusAngles on the same path.
constexpr uint8_t kExodusGlobalCode[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x48, 0x30, 0x48, 0x85, 0xC9};
constexpr uint8_t kExodusGlobalMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uintptr_t kExodusAngles = 0x9E0;

// Pause: the game clock (float seconds) in a static renderer object stops while the game is paused and runs in
// gameplay. The object: lea rcx,[rip+object]; jmp qword ptr [rax+290h] (metro.exe+0xEE1A7 in 1.0.0.4, unique), else this
// build's offset when it carries this build's vtable. Stopped for kPausedMs while frames go on = paused.
constexpr uint8_t kClockCode[] = {0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0x48, 0xFF, 0xA0, 0x90, 0x02};
constexpr uint8_t kClockMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uintptr_t kClockOwnerRva = 0xF4C430, kClockOwnerVtableRva = 0xE53C88, kClock = 0xBFF0;
constexpr ULONGLONG kPausedMs = 200;

// Main menu: a dword in .data, 1 in the main menu and 0 in a level (MODLOG 2026-10-24, Metro: Last Light Redux). Found
// by the code that sets it: mov dword ptr [rip+flag],1; mov word ptr [rip+..],di; test rsi,rsi (metro.exe+0x73C1AA in
// Last Light Redux 1.0.0.4, flag at +0xF4B838, unique). The disp32 is followed by the 4-byte immediate.
constexpr uint8_t kMenuCode[] = {0xC7, 0x05, 0, 0, 0, 0, 0x01, 0x00, 0x00, 0x00, 0x66, 0x89, 0x3D, 0, 0, 0, 0, 0x48, 0x85, 0xF6};
constexpr uint8_t kMenuMask[] = {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};
constexpr size_t kMenuDisp = 2, kMenuInsnEnd = 10;

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool any_file(const std::wstring& pattern) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return true;
}

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The Redux exe (metro.exe), whose build the fallback offsets belong to; other 4A games (MetroExodus.exe) have other layouts.
bool exe_is_metro() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"metro.exe") == 0;
}

// The exe's section of this name: [begin, end), or {0, 0}.
mem::Range exe_section(const char* name) {
    const uintptr_t base = exe_base();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            return {base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize};
    return {0, 0};
}

// The .data address a RIP-relative instruction refers to, from the code around it (once the exe is decrypted: the first
// frame). 0 unless the pattern is found and all its matches agree.
uintptr_t data_from_code(const uint8_t* code, const uint8_t* mask, size_t n, size_t disp_at, size_t insn_end,
                         const char* what) {
    const mem::Range text = exe_section(".text");
    const mem::Range data = exe_section(".data");
    if (!text.begin || !data.begin) return 0;
    std::vector<uint8_t> chunk(1 << 20);
    uintptr_t found = 0;
    int hits = 0;
    for (uintptr_t at = text.begin; at < text.end; at += chunk.size() - n) {
        const size_t len = std::min<uintptr_t>(chunk.size(), text.end - at);
        if (len < n || !mem::guarded_copy(chunk.data(), at, len)) continue;
        for (size_t i = 0; i + n <= len; ++i) {
            bool match = true;
            for (size_t j = 0; j < n && match; ++j) match = (chunk[i + j] & mask[j]) == (code[j] & mask[j]);
            if (!match) continue;
            int32_t disp = 0;
            std::memcpy(&disp, &chunk[i + disp_at], 4);
            const uintptr_t target = at + i + insn_end + disp;
            if (target >= data.begin && target < data.end && target != found) found = target, ++hits;
        }
        if (len < chunk.size()) break;
    }
    if (hits != 1) {
        log("4a: the code that refers to %s matched %d different addresses; not used", what, hits);
        return 0;
    }
    return found;
}

bool finite(const float* a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

class FourAEngine final : public Engine {
public:
    const char* name() const override { return "4A"; }

    // 4A's VFS archives next to the exe: content.vfx (the index) and content*.vfs0 (the first part of each archive).
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\content.vfx") && any_file(dir + L"\\content*.vfs0");
    }

    const wchar_t* window_class() const override { return L"_uengine_"; }

    void configure() override {}

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // The default view while the view's angles can be read, no mouse cursor is shown and the game isn't in its main menu;
    // nothing otherwise (main menu, loading). Without the global at all: the gyro as right stick.
    EngineFrame read_frame() override {
        if (!searched_) find_global();
        EngineFrame f;
        const bool was = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was) log("4a: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        if (!global_ && !by_offset_) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        float a[3];
        const bool had = angles_ != 0;
        angles_ = view_angles_address(a) ? angles_ : 0;
        if (bool(angles_) != had) log("4a: view angles %s", angles_ ? std::format("at {}", mem::module_offset(angles_)).c_str() : "gone");
        if (!angles_) return f;
        std::memcpy(last_, a, sizeof a);
        if (main_menu()) {
            f.target = "main menu";
            return f;
        }
        f.in_game = true;
        f.paused = paused();
        f.view_id = 1;
        f.target = "view angles";
        return f;
    }

    // The pitch the view has left before the game's limit, in the gyro's sense (up positive, degrees).
    void limit_turn(double& pitch, double& /*yaw*/) override {
        float a[3];
        if (!view_angles_address(a)) return;
        const double up = -a[0] / kRad, top = kMaxPitch / kRad;
        pitch = std::clamp(pitch, -top - up, top - up);
    }

    // Gyro senses (pitch up, yaw right, degrees) to the game's (radians, pitch down, yaw right).
    bool apply(double pitch, double yaw) override {
        float a[3];
        if (!view_angles_address(a)) return false;
        a[0] = std::clamp(float(a[0] - pitch * kRad), -kMaxPitch, kMaxPitch);
        a[1] = float(a[1] + yaw * kRad);
        return mem::guarded_store(angles_, a, 2 * sizeof(float));
    }

    Recenter recenter_support(std::string& detail) override {
        float a[3];
        if (!view_angles_address(a)) {
            detail = "view angles not readable";
            return Recenter::NotYet;
        }
        detail = std::format("pitch {:.1f}", -a[0] / kRad);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float a[3];
        if (!view_angles_address(a)) return false;
        a[0] = float(a[0] * (1.0 - fraction));
        return mem::guarded_store(angles_, a, sizeof(float));
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float a[3];
        if (!view_angles_address(a)) return false;
        pitch = -a[0] / kRad;
        yaw = a[1] / kRad;
        camera = angles_;
        return true;
    }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (!global_ && !by_offset_) return std::format("4a: no camera hook (gyro as right stick), cursor={}", int(cursor_));
        if (!angles_) return std::format("4a: view angles not readable (menu or loading), cursor={}", int(cursor_));
        if (main_menu_) return std::format("4a: main menu, cursor={}", int(cursor_));
        const std::string clock = clock_owner_ ? std::format(", clock {:.1f}{}", clock_value_, paused_ ? " (paused)" : "") : "";
        return std::format("4a: view pitch {:.1f} yaw {:.1f} (deg){}, cursor={}", -last_[0] / kRad, last_[1] / kRad, clock,
                           int(cursor_));
    }

private:
    // Once, on the first frame: the global and the clock's object from the code, else this build's offsets (checked on
    // every read).
    void find_global() {
        searched_ = true;
        global_ = data_from_code(kGlobalCode, kGlobalMask, sizeof kGlobalCode, kGlobalDisp, kGlobalInsnEnd, "the view's global");
        if (!global_) {
            global_ = data_from_code(kExodusGlobalCode, kExodusGlobalMask, sizeof kExodusGlobalCode, kGlobalDisp, kGlobalInsnEnd,
                                     "Metro Exodus's view global");
            if (global_) angles_offset_ = kExodusAngles;
        }
        if (global_) {
            log("4a: view's global at %s (found by its code), angles at +%#llx", mem::module_offset(global_).c_str(),
                static_cast<unsigned long long>(angles_offset_));
        } else if (exe_is_metro()) {
            by_offset_ = true;
            log("4a: view's global taken at metro.exe+%#llx (this build's offset, checked by the objects' vtables)",
                static_cast<unsigned long long>(kGlobalRva));
        } else {
            log("4a: not metro.exe and its view's global not found: no camera hook (gyro as right stick)");
        }
        clock_owner_ = data_from_code(kClockCode, kClockMask, sizeof kClockCode, 3, 7, "the game clock's object");
        if (!clock_owner_ && exe_is_metro() && mem::rdp(exe_base() + kClockOwnerRva) == exe_base() + kClockOwnerVtableRva)
            clock_owner_ = exe_base() + kClockOwnerRva;
        log("4a: game clock %s", clock_owner_ ? std::format("at {} (pause detection)", mem::module_offset(clock_owner_ + kClock)).c_str()
                                              : "not found: no pause detection");
        menu_flag_ = data_from_code(kMenuCode, kMenuMask, sizeof kMenuCode, kMenuDisp, kMenuInsnEnd, "the main-menu flag");
        log("4a: main-menu flag %s", menu_flag_ ? std::format("at {}", mem::module_offset(menu_flag_)).c_str()
                                                : "not found: the main menu turns the gyro like gameplay");
    }

    // The game's main-menu flag is 1 (anything but 0 or 1, or not readable: not the main menu).
    bool main_menu() {
        uint32_t flag = 0;
        const bool was = main_menu_;
        main_menu_ = menu_flag_ && mem::rd(menu_flag_, flag) && flag == 1;
        if (main_menu_ != was) log("4a: %s", main_menu_ ? "main menu" : "left the main menu");
        return main_menu_;
    }

    // The game clock stood still for kPausedMs while frames went on (the clock not readable: not paused).
    bool paused() {
        float clock = 0;
        if (!clock_owner_ || !mem::rd(clock_owner_ + kClock, clock) || !std::isfinite(clock)) return false;
        const ULONGLONG now = GetTickCount64();
        if (clock != clock_value_) clock_value_ = clock, clock_moved_ = now;
        const bool was = paused_;
        paused_ = now - clock_moved_ >= kPausedMs;
        if (paused_ != was) log("4a: %s (game clock %.2f)", paused_ ? "paused" : "unpaused", clock);
        return paused_;
    }

    // The angles' address on the current path, and the three floats; false if the path or the values don't hold.
    bool view_angles_address(float* a) {
        const uintptr_t base = exe_base();
        const uintptr_t global = global_ ? global_ : base + kGlobalRva;
        const uintptr_t first = mem::rdp(global);
        const uintptr_t owner = first ? mem::rdp(first + kViewOwner) : 0;
        if (!owner) return false;
        if (by_offset_) {
            const uintptr_t vt1 = mem::rdp(first), vt2 = mem::rdp(owner);
            if (vt1 != base + kGlobalVtableRva || vt2 != base + kOwnerVtableRva) return false;
        }
        const uintptr_t at = owner + angles_offset_;
        if (!mem::guarded_copy(a, at, 3 * sizeof(float)) || !finite(a) || std::fabs(a[0]) > 1.6f || a[2] != 0.0f)
            return false;
        angles_ = at;
        return true;
    }

    bool searched_ = false;
    bool by_offset_ = false;
    uintptr_t global_ = 0;
    uintptr_t angles_offset_ = kAngles;
    uintptr_t angles_ = 0;
    float last_[3] = {};
    bool cursor_ = false;
    uintptr_t clock_owner_ = 0;
    float clock_value_ = 0;
    ULONGLONG clock_moved_ = 0;
    bool paused_ = false;
    uintptr_t menu_flag_ = 0;
    bool main_menu_ = false;
};

}  // namespace

Engine& engine() {
    static FourAEngine instance;
    return instance;
}

}  // namespace gyro4all::foura
