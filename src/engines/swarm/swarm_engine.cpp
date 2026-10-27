// Swarm Engine (Saber Interactive): Warhammer 40,000: Space Marine 2 (MODLOG 2026-10-26/27). The client exe sits in
// client_pc\root\bin\pc, next to "<game> - Retail.dll" (the in-process server of solo and hosted games, the same code
// built as a DLL); the game is detected by its zip archives two folders up (paks\client\default\default_ssl.pak, the
// compiled SSL scripts). The game's window class is Magnus. The DllMain thread pumps only the splash screen's window
// (D3D11), then stops; the game window (D3D12, through NVIDIA Streamline) is pumped by another thread, which becomes
// the game thread (pump_window_class). The game reads the DualSense itself over HID.
//
// The view (MODLOG 2026-10-27, found with camtrace): the third-person camera controller holds the view's angles as
// `float pitch, yaw` in degrees at +0x130/+0x134 (pitch grows looking down, yaw is the view's heading atan2(x, z) with Y
// up and shrinks turning right, kept in (-180, 180]); +0x08 points to the camera rig's arm node, whose local transform
// the game rebuilds from them every frame (rig root: the character's yaw; arm: yaw relative to it and pitch, plus the
// shoulder offset; then the camera node, its render camera and the render view block, all copies). One controller per
// game; it is found by its vtable, which the controller's constructor stores (kVtableCode), with a heap scan on a thread
// of its own, again whenever it goes away (loading, a new level). Without the vtable the gyro goes to the game as
// right-stick input (the core's blind stick fallback). The game clamps the pitch to the camera mode's limits: the gyro
// stops at them (limit_turn) instead of pushing against them.
#include "swarm.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace gyro4all::swarm {

namespace {

// The controller's constructor: mov dword ptr [rcx+18h],1; lea rax,[rip+vtable]; xor ebx,ebx; mov [rcx],rax;
// mov [rcx+8],rbx; mov rdi,rcx; mov dword ptr [rcx+10h],3 (exe+0x13A1C54 in the 2026-10-26 build, vtable at
// exe+0x362B450, unique). The disp32 at kVtableDisp is relative to the end of the lea, kVtableInsnEnd bytes in.
constexpr uint8_t kVtableCode[] = {0xC7, 0x41, 0x18, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x33, 0xDB,
                                   0x48, 0x89, 0x01, 0x48, 0x89, 0x59, 0x08, 0x48, 0x8B, 0xF9, 0xC7, 0x41, 0x10, 0x03,
                                   0x00, 0x00, 0x00};
constexpr uint8_t kVtableMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF};
constexpr size_t kVtableDisp = 10, kVtableInsnEnd = 14;
// The controller: the rig's arm node, and the view's angles.
constexpr uintptr_t kArmNode = 0x08, kPitch = 0x130, kYaw = 0x134;
// The pitch limits of the on-foot camera (ssl/camera/cam_modes/mgs/cam_idle.sso: minConstrainRotationY 70 up,
// maxConstrainRotationY 75 down). Other modes allow less (sprint 50/50, the game clamps those itself) or more (aiming:
// 89 down), which the gyro gets once the game's own stick has taken the view that far.
constexpr float kMaxUp = 70, kMaxDown = 75;
constexpr ULONGLONG kRescanMs = 5000;

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

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

// The .rdata address the controller's constructor stores as its vtable; 0 unless the pattern is found and all its matches
// agree.
uintptr_t find_vtable() {
    const mem::Range text = exe_section(".text");
    const mem::Range rdata = exe_section(".rdata");
    if (!text.begin || !rdata.begin) return 0;
    constexpr size_t n = sizeof kVtableCode;
    std::vector<uint8_t> chunk(1 << 20);
    uintptr_t found = 0;
    int hits = 0;
    for (uintptr_t at = text.begin; at < text.end; at += chunk.size() - n) {
        const size_t len = std::min<uintptr_t>(chunk.size(), text.end - at);
        if (len < n || !mem::guarded_copy(chunk.data(), at, len)) continue;
        for (size_t i = 0; i + n <= len; ++i) {
            bool match = true;
            for (size_t j = 0; j < n && match; ++j) match = (chunk[i + j] & kVtableMask[j]) == (kVtableCode[j] & kVtableMask[j]);
            if (!match) continue;
            int32_t disp = 0;
            std::memcpy(&disp, &chunk[i + kVtableDisp], 4);
            const uintptr_t target = at + i + kVtableInsnEnd + disp;
            if (target >= rdata.begin && target < rdata.end && target != found) found = target, ++hits;
        }
        if (len < chunk.size()) break;
    }
    if (hits != 1) log("swarm: the camera controller's constructor matched %d different vtables; not used", hits);
    return hits == 1 ? found : 0;
}

// A controller: its vtable, an arm node and sane angles.
bool valid_controller(uintptr_t c, uintptr_t vtable) {
    uintptr_t vt = 0;
    float angles[2] = {};
    if (!mem::rd(c, vt) || vt != vtable || !mem::rdp(c + kArmNode) || !mem::guarded_copy(angles, c + kPitch, sizeof angles))
        return false;
    return std::isfinite(angles[0]) && std::isfinite(angles[1]) && std::fabs(angles[0]) <= 90 && std::fabs(angles[1]) <= 360;
}

// Background heap scan for the controller: every committed private read-write region, for a qword equal to the vtable.
std::atomic<uintptr_t> g_vtable{0}, g_found{0};
std::atomic<bool> g_scanning{false};

uintptr_t scan_region(uintptr_t begin, uintptr_t end, uintptr_t vtable, int& count) {
    uintptr_t first = 0;
    __try {
        for (uintptr_t a = begin; a + 8 <= end; a += 8)
            if (*reinterpret_cast<const uintptr_t*>(a) == vtable && valid_controller(a, vtable)) {
                if (!first) first = a;
                ++count;
            }
    } __except (mem::guard_filter(GetExceptionInformation())) {
    }
    return first;
}

DWORD WINAPI scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    const uintptr_t vtable = g_vtable;
    uint64_t bytes = 0;
    uintptr_t found = 0;
    int count = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            if (const uintptr_t c = scan_region(base, end, vtable, count); c && !found) found = c;
            bytes += mbi.RegionSize;
        }
        a = end;
    }
    if (found)
        log("swarm: camera controller at %p (%d found; %.2f GB scanned in %llu ms)", reinterpret_cast<void*>(found), count,
            double(bytes) / (1024.0 * 1024.0 * 1024.0), static_cast<unsigned long long>(GetTickCount64() - start));
    g_found = found;
    g_scanning = false;
    return 0;
}

double wrap180(double d) {
    d = std::fmod(d + 180.0, 360.0);
    return (d < 0 ? d + 360.0 : d) - 180.0;
}

class SwarmEngine final : public Engine {
public:
    const char* name() const override { return "Swarm"; }

    // <root>\bin\pc\<exe> with the compiled scripts' archive at <root>\paks\client\default\default_ssl.pak.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\..\\..\\paks\\client\\default\\default_ssl.pak");
    }

    const wchar_t* window_class() const override { return L"Magnus"; }
    const wchar_t* pump_window_class() const override { return L"Magnus"; }

    void configure() override {}

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // The default view while the camera controller is there and no mouse cursor is shown; nothing while it isn't (loading,
    // menus without a level). Without the vtable at all: the gyro as right stick.
    EngineFrame read_frame() override {
        if (!searched_) {
            searched_ = true;
            g_vtable = find_vtable();
            log("swarm: camera controller vtable %s", g_vtable ? mem::module_offset(g_vtable).c_str() : "not found: gyro as right stick");
        }
        EngineFrame f;
        const bool was = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was) log("swarm: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        if (!g_vtable) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        find_controller();
        if (!controller_) return f;
        // The game's own pitch (before this frame's gyro write): where its stick took the view widens the gyro's limits.
        if (float p = 0; mem::rd(controller_ + kPitch, p) && std::isfinite(p) && std::fabs(p) <= 90) {
            if (-p > max_up_) max_up_ = -p;
            if (p > max_down_) max_down_ = p;
        }
        f.in_game = true;
        f.view_id = 1;
        f.target = "TpsCameraController";
        return f;
    }

    // Trims the pitch to the limits, so the gyro stops there instead of pushing writes the game clamps back.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        float p = 0;
        if (!controller_ || !mem::rd(controller_ + kPitch, p)) return;
        const double target = std::clamp(p - pitch, double(-max_up_), double(max_down_));
        pitch = p - target;
    }

    // Degrees, the gyro's senses: pitch up, yaw right. The game's pitch grows looking down, its yaw shrinks turning right.
    // No view_angles: the core's stick fallback has nothing to give here (the game ignores a right stick added to its own
    // controller reports), and the game clamping the pitch at a limit would start it.
    bool apply(double pitch, double yaw) override {
        float angles[2];
        if (!controller_ || !mem::guarded_copy(angles, controller_ + kPitch, sizeof angles)) return false;
        angles[0] = std::clamp(float(angles[0] - pitch), -max_up_, max_down_);
        angles[1] = float(wrap180(angles[1] - yaw));
        return mem::guarded_store(controller_ + kPitch, angles, sizeof angles);
    }

    // Only the mouse cursor tells menus apart: GyroLib's "menus only" automatic calibration would never run.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (!g_vtable) return cursor_ ? "view=menu (cursor)" : "view=default (gyro as right stick)";
        if (!controller_) return "view=none (no camera controller)";
        float angles[2] = {};
        mem::guarded_copy(angles, controller_ + kPitch, sizeof angles);
        return std::format("view={} controller={} pitch={:.1f} (limits -{:.0f}..{:.0f}) yaw={:.1f}",
                           cursor_ ? "menu (cursor)" : "default", reinterpret_cast<void*>(controller_), angles[0],
                           max_up_, max_down_, angles[1]);
    }

private:
    // Keeps the controller while it stays valid; else picks up a finished scan or starts one (every kRescanMs at most).
    void find_controller() {
        if (controller_ && valid_controller(controller_, g_vtable)) return;
        if (controller_) {
            log("swarm: camera controller %p gone", reinterpret_cast<void*>(controller_));
            controller_ = 0;
        }
        if (g_scanning) return;
        if (const uintptr_t c = g_found.exchange(0); c && valid_controller(c, g_vtable)) {
            controller_ = c;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < next_scan_) return;
        next_scan_ = now + kRescanMs;
        g_scanning = true;
        if (HANDLE thread = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr)) CloseHandle(thread);
        else g_scanning = false;
    }

    bool searched_ = false;
    bool cursor_ = false;
    uintptr_t controller_ = 0;
    ULONGLONG next_scan_ = 0;
    float max_up_ = kMaxUp, max_down_ = kMaxDown;  // degrees, widened by the game's own stick
};

}  // namespace

Engine& engine() {
    static SwarmEngine e;
    return e;
}

}  // namespace gyro4all::swarm
