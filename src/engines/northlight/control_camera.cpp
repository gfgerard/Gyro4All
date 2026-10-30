// Control's gameplay camera (MODLOG 2026-10-29, found from coregame's exports, the exe's RTTI and camtrace).
//
// GamePlayerCameraMovementComponentState (RTTI ".?AVGamePlayerCameraMovementComponentState@@", vtable exe+0xe2b068 in
// the 2025 DX12 build) keeps the camera's look direction as a unit 3-vector at +0xB8, world +Y up, the camera's local
// +Z. Its per-frame update turns it by the stick (exe+0x3e4630) and copies it normalized to +0x40 (exe+0x3e4165), from
// which the camera entities' view quaternions (CameraComponentState+0x40) follow in the same frame. A write to +0xB8
// stays and turns the view; a write to +0x40 is undone within a frame. +0x250 holds the component's copy of the view
// quaternion (x, y, z, w; the view looks along its +Z).
//
// The game has two instances; the live one is the one whose +0x40 is its +0xB8 normalized and whose view quaternion
// looks the same way (every candidate is logged with what it failed). The vtable is found once from RTTI in the exe;
// the heap scan runs on its own thread, again whenever the component goes away.
//
// The update (exe+0x3e3be0, its only argument the component in rcx; frame time from a global) runs as a job on a worker
// thread of the game's job system, about once per rendered frame. Written from the pump thread, the gyro reached it 0-3
// times per update (391 writes/s for 135 updates/s), so the frames' steps were uneven (MODLOG 2026-10-30: 513 of 1079
// updates under half or over 1.7x the median step, the stick 2 of 1097), and a write between the update's read and write
// of +0xB8 was lost. So the update is hooked: the gyro's turns add up, and the hook applies them to the live component
// right before the game's own update, once per frame on the game's thread. Found by the code that copies +0xB8 to +0x40
// (unique in the exe) and the function around it in the unwind table.
#include "control_camera.hpp"

#include "hooks.hpp"
#include "look_queue.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace gyro4all::northlight::control {

namespace {

constexpr char kClass[] = ".?AVGamePlayerCameraMovementComponentState@@";
constexpr uintptr_t kLookCopy = 0x40, kLook = 0xB8, kViewQuat = 0x250;
// A respawn replaces the camera entity, whose new component is filled in a few seconds later (MODLOG 2026-10-30); a scan
// takes about 200 ms on its own thread and only runs while there is no component.
constexpr ULONGLONG kRescanMs = 1000;

// MSVC RTTI: the vtable of the class with this decorated name, 0 if not found. Type descriptor in .data ({ type_info's
// vtable, spare, name }); complete object locator in .rdata ({ 1, offset 0, cdOffset, type descriptor rva, hierarchy
// rva, self rva }), and the vtable's slot -1 points to it.
uintptr_t rtti_vtable(const char* decorated) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const Section data = exe_section(".data"), rdata = exe_section(".rdata");
    const std::vector<uint8_t> d = copy(data), r = copy(rdata);
    if (d.empty() || r.empty()) return 0;
    const size_t n = std::strlen(decorated) + 1;
    uintptr_t td = 0;
    for (size_t i = 0; i + 16 + n <= d.size() && !td; i += 8)
        if (!std::memcmp(&d[i + 16], decorated, n)) td = data.begin + i;
    if (!td) return 0;
    const auto td_rva = uint32_t(td - base);
    for (size_t i = 0; i + 24 <= r.size(); i += 4) {
        uint32_t col[6];
        std::memcpy(col, &r[i], sizeof col);
        const auto self = uint32_t(rdata.begin + i - base);
        if (col[0] != 1 || col[1] != 0 || col[3] != td_rva || col[5] != self) continue;
        const uint64_t at = rdata.begin + i;
        for (size_t j = 0; j + 8 <= r.size(); j += 8)
            if (!std::memcmp(&r[j], &at, 8)) return rdata.begin + j + 8;
    }
    return 0;
}

// Why c isn't a live component ("" when it is).
std::string check(uintptr_t c, uintptr_t vtable) {
    uintptr_t vt = 0;
    if (!mem::rd(c, vt) || vt != vtable) return "vtable";
    double look[3], copy[3];
    if (!read3(c + kLook, look) || !read3(c + kLookCopy, copy)) return "unreadable";
    const double len = length(look);
    if (std::fabs(len - 1.0) > 0.01) return std::format("look length {:.3f}", len);
    const double dot = (look[0] * copy[0] + look[1] * copy[1] + look[2] * copy[2]) / len;
    if (dot < 0.999) return std::format("copy differs (dot {:.3f})", dot);
    float q[4];
    if (!mem::guarded_copy(q, c + kViewQuat, sizeof q)) return "no view quaternion";
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double qn = x * x + y * y + z * z + w * w;
    if (!std::isfinite(qn) || std::fabs(qn - 1.0) > 0.02) return std::format("view quaternion norm {:.3f}", qn);
    const double fwd[3] = {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
    const double view = (fwd[0] * look[0] + fwd[1] * look[1] + fwd[2] * look[2]) / len;
    if (view < 0.98) return std::format("view looks elsewhere (dot {:.3f})", view);
    return {};
}

std::atomic<uintptr_t> g_vtable{0}, g_found{0};
std::atomic<bool> g_scanning{false}, g_searched{false};
uintptr_t g_component = 0;  // game thread
ULONGLONG g_next_scan = 0;

// lea rdx,[rdi+40h]; movaps xmm0,xmm4; cmpeqps xmm0,[rdx]; movmskps eax,xmm0; cmp eax,0Fh; je rel8; movups [rdx],xmm4
// (0 = any byte): the update storing the normalized look direction to +0x40.
constexpr uint8_t kCopyCode[] = {0x48, 0x8D, 0x57, 0x40, 0x0F, 0x28, 0xC4, 0x0F, 0xC2, 0x02, 0x00, 0x0F,
                                 0x50, 0xC0, 0x83, 0xF8, 0x0F, 0x74, 0x00, 0x0F, 0x11, 0x22};
constexpr size_t kCopyAny = 18;

using Update_t = uintptr_t (*)(void* self);
Update_t g_update = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<uintptr_t> g_live{0};  // the component the hook turns (the game thread's g_component)
LookQueue g_queue;  // the gyro's turns, from Gyro4All's frame to the hooked update

// The update's start, 0 unless the copy code is found exactly once.
uintptr_t find_update() {
    int hits = 0;
    const uintptr_t at = find_unique_code(kCopyCode, sizeof kCopyCode, kCopyAny, hits);
    if (!at) {
        log("northlight: the camera movement update's code matched %d times; the gyro writes the camera from the pump thread", hits);
        return 0;
    }
    return function_start(at);
}

uintptr_t hk_update(void* self) {
    if (reinterpret_cast<uintptr_t>(self) == g_live.load(std::memory_order_relaxed)) {
        double pitch, yaw, up, down;
        if (g_queue.take(pitch, yaw, up, down)) turn(reinterpret_cast<uintptr_t>(self), pitch, yaw, up, down);
    }
    return g_update(self);
}

void install_hook() {
    const uintptr_t update = find_update();
    if (!update) return;
    if (!hooks::hook_function(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&hk_update),
                              reinterpret_cast<void**>(&g_update))) {
        log("northlight: the camera movement update at %s could not be hooked; the gyro writes the camera from the pump thread",
            mem::module_offset(update).c_str());
        return;
    }
    g_hooked = true;
    log("northlight: camera movement update hooked at %s: the gyro turns the camera in the game's own update",
        mem::module_offset(update).c_str());
}

void scan_region(uintptr_t begin, uintptr_t end, uintptr_t vtable, std::vector<uintptr_t>& out) {
    __try {
        for (uintptr_t a = begin; a + 8 <= end && out.size() < 16; a += 8)
            if (*reinterpret_cast<const uintptr_t*>(a) == vtable) out.push_back(a);
    } __except (mem::guard_filter(GetExceptionInformation())) {
    }
}

DWORD WINAPI scan_thread(void*) {
    if (!g_searched) {
        const ULONGLONG start = GetTickCount64();
        g_vtable = rtti_vtable(kClass);
        g_searched = true;
        log("northlight: GamePlayerCameraMovementComponentState vtable %s (%llu ms)",
            g_vtable ? mem::module_offset(g_vtable).c_str() : "not found: gyro as right stick",
            static_cast<unsigned long long>(GetTickCount64() - start));
        if (g_vtable) install_hook();
    }
    const uintptr_t vtable = g_vtable;
    if (!vtable) {
        g_scanning = false;
        return 0;
    }
    const ULONGLONG start = GetTickCount64();
    std::vector<uintptr_t> objects;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && objects.size() < 16;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE)
            scan_region(base, end, vtable, objects);
        a = end;
    }
    uintptr_t found = 0;
    std::string detail;
    for (uintptr_t c : objects) {
        const std::string why = check(c, vtable);
        if (why.empty() && !found) found = c;
        detail += std::format(" {}{}", reinterpret_cast<void*>(c), why.empty() ? (c == found ? " (live)" : " (also live)") : " (" + why + ")");
    }
    static std::string last;
    if (detail != last && !objects.empty())
        log("northlight: %zu camera movement component(s) in %llu ms:%s", objects.size(),
            static_cast<unsigned long long>(GetTickCount64() - start), detail.c_str());
    last = detail;
    g_found = found;
    g_scanning = false;
    return 0;
}

}  // namespace

Search poll() {
    if (g_scanning) return Search::Searching;
    if (g_searched && !g_vtable) return Search::NoVtable;
    if (const uintptr_t c = g_found.exchange(0); c && valid(c)) {
        if (c != g_component) log("northlight: camera movement component %p", reinterpret_cast<void*>(c));
        g_component = c;
        g_live = c;
    }
    if (g_component) return Search::Done;
    const ULONGLONG now = GetTickCount64();
    if (g_searched && now < g_next_scan) return Search::Done;
    g_next_scan = now + kRescanMs;
    g_scanning = true;
    if (HANDLE thread = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    else g_scanning = false;
    return g_searched ? Search::Done : Search::Searching;
}

uintptr_t component() { return g_component; }

void lost() {
    if (g_component) log("northlight: camera movement component %p gone", reinterpret_cast<void*>(g_component));
    g_component = 0;
    g_live = 0;
}

// Only the vtable and the look direction's length: its copies (+0x40, the view quaternion) trail it by a frame after a
// gyro write or while turning fast, so the scan's full check would drop the live component (MODLOG 2026-10-29).
bool valid(uintptr_t c) {
    uintptr_t vt = 0;
    double look[3];
    return c && g_vtable && mem::rd(c, vt) && vt == g_vtable && read3(c + kLook, look) && std::fabs(length(look) - 1.0) < 0.01;
}

bool look(uintptr_t c, double& pitch, double& heading) { return look_angles(c + kLook, pitch, heading); }

bool turn(uintptr_t c, double pitch, double yaw, double max_up, double max_down) {
    return turn_look(c + kLook, pitch, yaw, max_up, max_down);
}

bool hooked() { return g_hooked; }

void add(double pitch, double yaw, double max_up, double max_down) { g_queue.add(pitch, yaw, max_up, max_down); }

double pending_pitch() { return g_queue.pending_pitch(); }

}  // namespace gyro4all::northlight::control
