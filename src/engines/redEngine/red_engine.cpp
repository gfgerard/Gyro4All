// REDengine 4 (CD Projekt RED): Cyberpunk 2077 (games/Cyberpunk2077/MODDING_PLAN.md). The exe sits in <root>\bin\x64
// with the packed content in <root>\archive\pc\content and the engine data in <root>\engine. The GOG and Steam
// builds have no Denuvo and no anti-cheat, and the game is single player. RED4ext (as winmm.dll) and Cyber Engine
// Tweaks (as an ASI) are common in modded installs, so Gyro4All goes in as version.dll there; this module needs neither.
//
// The scene (patch 2.50, MODLOG 2026-11-04), read without calling any game code:
// - The game's global CGameEngine pointer. Its address comes from cyberpunk2077_addresses.json, CD Projekt's own
//   address file shipped next to the exe and updated with every patch (the one RED4ext uses). It is keyed by stable
//   hashes (0x97F209D6 = CGameEngine), and its "Linker map timestamp" must equal the exe's PE timestamp. The file is
//   about 150 MB, so a thread started in configure() looks the address up while the game starts.
// - CGameEngine +0x348 framework -> +0x10 GameInstance -> +0x38 DynArray<Handle<IScriptable>> of the game systems
//   (RED4ext.SDK's layout). The camera system is the one whose RTTI class is gameCameraSystem.
// - gameCameraSystem keeps the active camera's data, as its natives read it (handlers in the exe: GetActiveCameraData
//   copies it from +0x500, GetActiveCameraFOV reads +0x520, GetActiveCameraForward rotates by +0x510):
//   +0x500 int32[3] world position (fixed point), +0x510 float[4] orientation quaternion (i, j, k, r), +0x520 float FOV.
//   Z up, Y forward.
// - RTTI: every IScriptable has its CClass* at +0x30, or its vtable's first entry (GetNativeType) is a "lea/mov rax,
//   [rip+x]; ret" getter decoded here without calling it. A CClass has its parent at +0x10 and its name at +0x18 as a
//   CName: the FNV-1a 64 hash of the class name, so classes are compared by hash.
//
// The camera kinds (classes from RED4ext.SDK and the exe's RTTI names):
// - on foot: gameFPPCameraComponent on the PlayerPuppet;
// - car / motorcycle in third person: vehicleTPPCameraComponent, whose owner entity is a vehicleCarBaseObject (or another
//   vehicleBaseObject) or a vehicleBikeBaseObject; in first person: the player's FPP camera while a vehicle camera exists;
// - cinematics: any other active camera (scene cameras, vehicleCinematicCameraComponent, gameWorldSpaceBlendCamera
//   blends, photo mode's free camera): no gyro.
// Which component is active is found by matching: the active camera data's orientation against the world orientation
// stored in each candidate camera component (found by scanning the camera system and the player's components). The
// offsets where each class keeps it are learnt at runtime and logged.
//
// In first person the gyro writes the view's own sources (constants below): on foot the body's yaw and the look node's
// pitch; seated in a vehicle, only with a weapon drawn, the look node's yaw and pitch (off while driving). The vehicles' third-person cameras aren't matched yet (the
// view counts as a cinematic there), and nothing turns in cinematics. The look node is written from the game thread while the animation graph runs on worker
// threads, so a frame's turn can rarely be lost to the game's own update of it.
#include "red.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gyro4all::red {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

constexpr uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : s) h = (h ^ uint8_t(c)) * 0x100000001B3ull;
    return h;
}

// Class names compared by CName hash; the table names them in the log.
struct Known {
    const char* name;
    uint64_t hash;
};
#define RED_CLASS(n) Known{n, fnv1a64(n)}
constexpr Known kClasses[] = {
    RED_CLASS("gameCameraSystem"),
    RED_CLASS("gamePlayerSystem"),
    RED_CLASS("entEntity"),
    RED_CLASS("PlayerPuppet"),
    RED_CLASS("vehicleBaseObject"),
    RED_CLASS("vehicleBikeBaseObject"),
    RED_CLASS("vehicleCarBaseObject"),
    RED_CLASS("entIComponent"),
    RED_CLASS("entBaseCameraComponent"),
    RED_CLASS("gameFPPCameraComponent"),
    RED_CLASS("gameTPPCameraComponent"),
    RED_CLASS("vehicleTPPCameraComponent"),
    RED_CLASS("vehicleCinematicCameraComponent"),
    RED_CLASS("gameCameraComponent"),
    RED_CLASS("gameWorldSpaceBlendCamera"),
    RED_CLASS("gameFreeCameraComponent"),
    RED_CLASS("gamePhotoModeBackgroundCameraComponent"),
    RED_CLASS("entVirtualCameraComponent"),
    RED_CLASS("entAnimatedComponent"),
    RED_CLASS("moveComponent"),
};
#undef RED_CLASS
constexpr uint64_t h(std::string_view s) { return fnv1a64(s); }

// CD Projekt's address file: hash of CGameEngine's global pointer (RED4ext.SDK Detail/AddressHashes.hpp).
constexpr uint32_t kHashCGameEngine = 0x97F209D6;

// Layouts (RED4ext.SDK, checked against the 2.50 exe where noted above).
constexpr uintptr_t kEngineFramework = 0x348, kFrameworkGameInstance = 0x10, kGameInstanceSystems = 0x38;
constexpr uintptr_t kScriptableType = 0x30, kClassParent = 0x10, kClassName = 0x18;
constexpr uintptr_t kCameraPosition = 0x500, kCameraOrientation = 0x510, kCameraFov = 0x520;
constexpr uintptr_t kCameraSystemSize = 0x780, kPlayerSystemSize = 0x70, kEntitySize = 0x160;
constexpr uintptr_t kComponentSize = 0x1E0;  // entBaseCameraComponent
// entBaseCameraComponent.fov (its RTTI property, checked on 2.50): the camera's own FOV in the active camera data's
// unit, which the active FOV (+0x520) narrows from while aiming (both 69 at the 90 FOV setting, 56.9 aiming a pistol).
constexpr uintptr_t kComponentFov = 0x128;
// The camera system's zoom factor, applied on top of the FOV: 1 except through scopes (5.25 with a sniper rifle's), where
// the FOV itself stays put. Taken as a focal-length factor: the view is 2 atan(tan(fov / 2) / zoom).
constexpr uintptr_t kCameraZoom = 0x53C;

// The on-foot view's sources (patch 2.50, found with camtrace and write breakpoints, MODLOG 2026-11-04):
// - yaw: the player's body orientation, a quaternion the mover update reads back every frame. PlayerPuppet components
//   (entEntity +0xA0, DynArray<Handle<IComponent>>) -> moveComponent +0x160 (a native object, no RTTI) +0x30 (i, j, k, r).
//   The mover takes it only while moveComponent +0x154 != 0 and +0x140 != 0.
// - pitch: a float in degrees in the state of the player's look node in the animation graph, which the game integrates
//   (state +0x1B0 += input, clamped to the node's limits; exe+0xa93d7c) and the FPP camera reads back through the
//   graph's outputs (gameFPPCameraComponent +0x430). Path: the FPP camera's +0x380 (the player's entAnimatedComponent)
//   +0x240 (another entAnimatedComponent) +0x270 (the graph instance) +0x28 -> pointer -> + 0xDA0 = the state. Checked
//   every frame: state +0x1A8 is the camera's FOV, +0x1B0 matches the FPP camera's +0x430, and +0x1C0 = +0x1B0 + +0x1B8.
constexpr uintptr_t kEntityComponents = 0xA0;
constexpr uintptr_t kMoverActive = 0x140, kMoverMode = 0x154, kMoverBody = 0x160, kBodyOrientation = 0x30;
constexpr uintptr_t kFppAnimated = 0x380, kFppPitchOut = 0x430, kFppPitchMin = 0x3E0, kFppPitchMax = 0x3E4;
constexpr uintptr_t kAnimatedCamera = 0x240, kAnimatedGraph = 0x270, kGraphVariables = 0x28, kLookState = 0xDA0;
constexpr uintptr_t kLookFov = 0x1A8, kLookPitch = 0x1B0, kLookPitchExtra = 0x1B8, kLookPitchTotal = 0x1C0;
// Seated in a vehicle in first person the body follows the vehicle and the look node also turns the head: its yaw
// (+0x1AC, the pitch's twin; 0 on foot) with +0x1BC = +0x1AC + +0x1B4, read back at FPP +0x42C. It goes down to the right,
// stays within +-177.5, and the game brings it back to 0 within about 2 s when the stick is let go. Seated: moveComponent
// +0x158 is 0 (1 on foot) and FPP +0x400 is 1 (0 on foot).
constexpr uintptr_t kLookYaw = 0x1AC, kLookYawExtra = 0x1B4, kLookYawTotal = 0x1BC, kFppYawOut = 0x42C;
constexpr uintptr_t kMoverBodyFollowsInput = 0x158, kFppSeated = 0x400;
constexpr uintptr_t kFppSeatedCombatA = 0x454, kFppSeatedCombatB = 0x460;  // 10 / 50 driving, 0 with a weapon out
constexpr int kSeatedNudgeFrames = 6;   // stick frames shown when a weapon is drawn in the seat
constexpr float kSeatedNudge = 0.2f;    // their deflection, alternating sides
constexpr float kRecentreMinYaw = 3, kRecentreStep = 0.15f;  // the look yaw going back to 0 on its own (degrees/frame)
constexpr int kNudgeCooldownFrames = 20;
constexpr float kLookYawLimit = 177.5f;

constexpr double kPi = 3.14159265358979323846;

uintptr_t g_base = 0, g_end = 0;  // the exe's image
std::atomic<uintptr_t> g_engine_global{0};  // address of the CGameEngine* global, 0 until resolved (or never)

bool in_exe(uintptr_t p) { return p >= g_base && p < g_end; }

// Reads that never fault. Cyberpunk installs a vectored exception handler (the exe imports AddVectoredExceptionHandler)
// that sees an access violation before any __except does and ends the game with its crash report: the first scan's
// guarded reads of stray pointers crashed it (MODLOG 2026-11-04). So every range is checked with VirtualQuery first
// (committed, readable, not a guard page). The regions checked are cached for one frame only (clear_regions), since
// the game can free memory between frames.
struct Region {
    uintptr_t begin, end;
};
Region g_regions[64];
unsigned g_region_count = 0, g_region_next = 0;

void clear_regions() { g_region_count = g_region_next = 0; }

bool readable(uintptr_t p, size_t n) {
    if (!p || p + n < p) return false;
    if (in_exe(p) && in_exe(p + n - 1)) return true;
    for (unsigned i = 0; i < g_region_count; ++i)
        if (p >= g_regions[i].begin && p + n <= g_regions[i].end) return true;
    uintptr_t at = p;
    while (at < p + n) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect;
        if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) || !(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                              PAGE_EXECUTE_WRITECOPY)))
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = begin + mbi.RegionSize;
        g_regions[g_region_next] = {begin, end};
        g_region_next = (g_region_next + 1) % 64;
        if (g_region_count < 64) ++g_region_count;
        at = end;
    }
    return true;
}

bool copy(void* dst, uintptr_t src, size_t n) { return readable(src, n) && mem::guarded_copy(dst, src, n); }

template <class T>
bool rd(uintptr_t address, T& out) {
    return copy(&out, address, sizeof(T));
}

uintptr_t rdp(uintptr_t address) {
    uintptr_t v = 0;
    return mem::plausible(address) && rd(address, v) && mem::plausible(v) ? v : 0;
}

// Writes go only to committed read-write pages (checked every time: two small writes a frame), for the same reason.
bool store(uintptr_t dst, const void* src, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!dst || !VirtualQuery(reinterpret_cast<void*>(dst), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
        dst + n > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
        return false;
    return mem::guarded_store(dst, src, n);
}

void init_image() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
}

// --- the address file ---

// The RVA of entry `hash` in cyberpunk2077_addresses.json ("offset": "SSSS:OOOOOOOO", a 1-based section and an offset
// in it), 0 if the file is missing, is for another build or lacks the entry. Plain text search: the file is one long
// JSON line; full parsing isn't needed.
uintptr_t address_from_file(const std::wstring& path, uint32_t hash) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log("red: %ls not found: no scene (gyro as right stick)", path.c_str());
        return 0;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const HANDLE map = size.QuadPart > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    const char* text = map ? static_cast<const char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    uintptr_t rva = 0;
    if (text) {
        const std::string_view all(text, size_t(size.QuadPart));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        // "Linker map timestamp": "6ad55daf (...)" must be the exe's own build.
        char stamp[64];
        snprintf(stamp, sizeof stamp, "\"Linker map timestamp\": \"%08lx", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        if (all.substr(0, 4096).find(stamp) == std::string_view::npos) {
            log("red: %ls is for another build of the game (exe timestamp %08lx): no scene (gyro as right stick)",
                path.c_str(), static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        } else {
            char key[48];
            snprintf(key, sizeof key, "\"hash\": \"%lu\",", static_cast<unsigned long>(hash));
            const size_t at = all.find(key);
            const size_t off = at == std::string_view::npos ? at : all.find("\"offset\": \"", at);
            unsigned section = 0, offset = 0;
            if (off != std::string_view::npos && off - at < 512 &&
                sscanf_s(text + off + 11, "%4x:%8x", &section, &offset) == 2 && section >= 1 &&
                section <= nt->FileHeader.NumberOfSections) {
                const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt) + (section - 1);
                rva = sec->VirtualAddress + offset;
            } else {
                log("red: hash %08x not in %ls: no scene (gyro as right stick)", hash, path.c_str());
            }
        }
        UnmapViewOfFile(text);
    }
    if (map) CloseHandle(map);
    CloseHandle(file);
    return rva;
}

DWORD WINAPI resolve_thread(void* param) {
    std::wstring* path = static_cast<std::wstring*>(param);
    const uintptr_t rva = address_from_file(*path, kHashCGameEngine);
    if (rva && in_exe(g_base + rva)) {
        g_engine_global = g_base + rva;
        log("red: CGameEngine global at %s (address file)", mem::module_offset(g_base + rva).c_str());
    }
    delete path;
    return 0;
}

// --- RTTI ---

const char* class_name(uint64_t hash) {
    for (const Known& k : kClasses)
        if (k.hash == hash) return k.name;
    return nullptr;
}

std::string describe_class(uint64_t hash) {
    if (const char* n = class_name(hash)) return n;
    char buf[32];
    snprintf(buf, sizeof buf, "class %016llx", static_cast<unsigned long long>(hash));
    return buf;
}

// A CClass: its vtable in the exe and a name.
bool is_class(uintptr_t type) {
    uint64_t name = 0;
    return type && mem::plausible(type) && in_exe(rdp(type)) && rd(type + kClassName, name) && name != 0;
}

// What a GetNativeType-style getter returns, decoded from its code without calling it: "lea rax,[rip+d]; ret" (the
// class object) or "mov rax,[rip+d]; ret" (a static pointer to it), through at most one "jmp rel32" thunk.
uintptr_t getter_result(uintptr_t fn) {
    for (int hop = 0; hop < 2 && in_exe(fn); ++hop) {
        uint8_t b[8];
        if (!copy(b, fn, sizeof b)) return 0;
        if (b[0] == 0xE9) {
            int32_t rel;
            memcpy(&rel, b + 1, 4);
            fn = fn + 5 + intptr_t(rel);
            continue;
        }
        if (b[0] == 0x48 && (b[1] == 0x8D || b[1] == 0x8B) && b[2] == 0x05 && b[7] == 0xC3) {
            int32_t d;
            memcpy(&d, b + 3, 4);
            const uintptr_t at = fn + 7 + intptr_t(d);
            return b[1] == 0x8D ? at : rdp(at);
        }
        return 0;
    }
    return 0;
}

// An object's RTTI class (0 = not an IScriptable this module can read). Cached per vtable for objects without their own
// type pointer.
uintptr_t type_of(uintptr_t obj) {
    static std::unordered_map<uintptr_t, uintptr_t> by_vtable;
    if (!obj || !mem::plausible(obj)) return 0;
    const uintptr_t vt = rdp(obj);
    if (!in_exe(vt)) return 0;
    if (const uintptr_t t = rdp(obj + kScriptableType); t && is_class(t)) return t;
    if (const auto it = by_vtable.find(vt); it != by_vtable.end()) return it->second;
    uintptr_t t = getter_result(rdp(vt));
    if (!is_class(t)) t = 0;
    if (by_vtable.size() < 4096) by_vtable.emplace(vt, t);
    return t;
}

uint64_t name_of(uintptr_t type) {
    uint64_t n = 0;
    return type && rd(type + kClassName, n) ? n : 0;
}

// The object's class is `hash` or derives from it.
bool is_a(uintptr_t obj, uint64_t hash) {
    uintptr_t t = type_of(obj);
    for (int depth = 0; t && depth < 32; ++depth) {
        if (name_of(t) == hash) return true;
        t = rdp(t + kClassParent);
        if (t && !is_class(t)) return false;
    }
    return false;
}

uint64_t class_hash(uintptr_t obj) { return name_of(type_of(obj)); }

// Calls f(instance) for each handle of a DynArray<Handle<T>> at `arr` ({entries, capacity, size}); false if `arr` doesn't
// look like one.
template <class F>
bool each_handle(uintptr_t arr, uint32_t max_size, F&& f) {
    const uintptr_t entries = rdp(arr);
    uint32_t cap = 0, size = 0;
    if (!entries || !rd(arr + 8, cap) || !rd(arr + 12, size) || size == 0 || size > cap || size > max_size)
        return false;
    for (uint32_t i = 0; i < size; ++i)
        if (const uintptr_t obj = rdp(entries + 16ull * i)) f(obj);
    return true;
}

// --- the scene ---

uintptr_t game_instance() {
    const uintptr_t global = g_engine_global.load(std::memory_order_relaxed);
    const uintptr_t engine = global ? rdp(global) : 0;
    const uintptr_t framework = engine ? rdp(engine + kEngineFramework) : 0;
    return framework ? rdp(framework + kFrameworkGameInstance) : 0;
}

// The game system of class `hash`, 0 if none (front end before the game instance exists).
uintptr_t game_system(uint64_t hash) {
    const uintptr_t gi = game_instance();
    uintptr_t found = 0;
    if (gi) each_handle(gi + kGameInstanceSystems, 1024, [&](uintptr_t sys) {
        if (!found && class_hash(sys) == hash) found = sys;
    });
    return found;
}

struct ActiveCamera {
    int32_t position[3];
    float q[4];  // i, j, k, r
    float fov;
};

bool read_active(uintptr_t camera_system, ActiveCamera& out) {
    if (!camera_system || !rd(camera_system + kCameraPosition, out.position) ||
        !rd(camera_system + kCameraOrientation, out.q) || !rd(camera_system + kCameraFov, out.fov))
        return false;
    const double n = double(out.q[0]) * out.q[0] + double(out.q[1]) * out.q[1] + double(out.q[2]) * out.q[2] +
                     double(out.q[3]) * out.q[3];
    return std::isfinite(n) && fabs(n - 1.0) < 0.01;
}

// Heading (degrees, grows to the right) and pitch (degrees, up) of the quaternion's forward (+Y).
void angles(const float (&q)[4], double& pitch, double& yaw) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double fx = 2 * (x * y - w * z), fy = 1 - 2 * (x * x + z * z), fz = 2 * (y * z + w * x);
    pitch = asin(std::fmax(-1.0, std::fmin(1.0, fz))) * 180 / kPi;
    yaw = atan2(fx, fy) * 180 / kPi;
}

bool same_orientation(const float (&a)[4], const float (&b)[4]) {
    const double dot = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2] + double(a[3]) * b[3];
    return fabs(dot) > 0.9995;  // within about 3.6 degrees: the copy may be a frame older than the component
}

enum class Kind { Unknown, OnFoot, Car, Motorcycle, Cinematic };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::OnFoot: return "on foot";
        case Kind::Car: return "car";
        case Kind::Motorcycle: return "motorcycle";
        case Kind::Cinematic: return "cinematic";
        default: return "unknown";
    }
}

// A camera component the scan found: what it is and who owns it.
struct Candidate {
    uintptr_t component = 0;
    uint64_t cls = 0;
    uintptr_t owner = 0;
    uint64_t owner_cls = 0;
    std::string from;  // where the scan found it, for the log
};

// The entity that owns a component: an entEntity pointer in the component's IComponent part.
uintptr_t owner_of(uintptr_t component) {
    for (uintptr_t off = 0x40; off < 0x90; off += 8) {
        const uintptr_t p = rdp(component + off);
        if (p && p != component && is_a(p, h("entEntity"))) return p;
    }
    return 0;
}

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

class RedEngine final : public Engine {
public:
    const char* name() const override { return "REDengine"; }

    // Cyberpunk 2077: <root>\bin\x64\<exe> with <root>\archive\pc\content and <root>\engine\config.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        const std::wstring root = dir + L"\\..\\..";
        return exists(root + L"\\archive\\pc\\content") && exists(root + L"\\engine\\config");
    }

    // The crash reporter next to the exe isn't the game.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring exe = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        return _wcsicmp(exe.c_str(), L"REDEngineErrorReporter.exe") == 0;
    }

    const wchar_t* window_class() const override { return L"W2ViewportClass"; }

    void configure() override {
        init_image();
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L"\\/")) + L"\\cyberpunk2077_addresses.json";
        auto* param = new std::wstring(path);
        if (HANDLE t = CreateThread(nullptr, 0, &resolve_thread, param, 0, nullptr)) {
            CloseHandle(t);
        } else {
            delete param;
            log("red: can't start the address lookup (error %lu): no scene (gyro as right stick)", GetLastError());
        }
    }

    std::vector<ViewInfo> views() const override {
        return {{1, "On foot", 0, true}, {2, "Car", 1, true}, {3, "Motorcycle", 1, true}};
    }

    EngineFrame read_frame() override {
        clear_regions();
        EngineFrame f;
        const bool was_cursor = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was_cursor) log("red: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        f.in_game = true;

        camera_system_ = game_system(h("gameCameraSystem"));
        const bool active = read_active(camera_system_, active_);
        if (active != have_active_) {
            log("red: active camera data %s", active ? "found (camera system +0x500)" : "gone");
            have_active_ = active;
        }
        Kind kind = Kind::Unknown;
        if (active) kind = classify();
        else candidates_.clear();
        if (kind != kind_) {
            log("red: camera %s -> %s%s", kind_name(kind_), kind_name(kind), active_desc_.c_str());
            kind_ = kind;
        }
        switch (kind) {
            case Kind::Cinematic: f.view_id = 0; break;
            case Kind::Car: f.view_id = 2; break;
            case Kind::Motorcycle: f.view_id = 3; break;
            default: f.view_id = 1; break;  // on foot, or not identified (yet)
        }
        // In first person (on foot, or seated in a vehicle) the gyro writes the view's sources; in the vehicles' third
        // person cameras (not found yet) it is the right stick.
        const std::string why = fpp_ ? resolve_sources() : std::string("not in first person");
        sources_ok_ = why.empty();
        if (why != sources_why_) {
            if (sources_ok_ && seated_)
                log("red: writing the view: seated, look yaw and pitch at %s (%.2f)", mem::module_offset(look_).c_str(),
                    look_pitch());
            else if (sources_ok_)
                log("red: writing the view: body yaw at %s, look pitch at %s (%.2f)", mem::module_offset(body_).c_str(),
                    mem::module_offset(look_).c_str(), look_pitch());
            else if (fpp_)
                log("red: can't write the first-person view (%s): gyro as right stick", why.c_str());
            sources_why_ = why;
        }
        f.no_camera = !sources_ok_;
        // Seated in a vehicle (first person): no gyro and no stick fallback while driving; with a weapon drawn (aiming
        // from the seat) the look yaw and pitch are written, never the stick.
        const bool drawn = fpp_ && seated_ && seated_weapon_drawn();
        if (fpp_ && seated_ && drawn != logged_drawn_) {
            log("red: seated, weapon %s: gyro %s", drawn ? "drawn" : "holstered", drawn ? "on" : "off");
            logged_drawn_ = drawn;
            nudge_frames_ = drawn ? kSeatedNudgeFrames : 0;
        }
        // Seated with a weapon drawn, the game recentres the look yaw past some angle until it sees the right stick,
        // and again after the vehicle accelerates: show it a few frames of stick (alternating sides so the view doesn't
        // turn) when the weapon is drawn and whenever the yaw starts going back to the centre on its own.
        if (drawn && sources_ok_) {
            float now = 0;
            if (rd(look_ + kLookYaw, now)) {
                if (nudge_cooldown_ > 0) --nudge_cooldown_;
                if (have_last_yaw_ && std::fabs(last_yaw_) > kRecentreMinYaw &&
                    std::fabs(now) < std::fabs(last_yaw_) - kRecentreStep && now * last_yaw_ >= 0 && nudge_frames_ == 0 &&
                    nudge_cooldown_ == 0) {
                    nudge_frames_ = kSeatedNudgeFrames;
                    nudge_cooldown_ = kNudgeCooldownFrames;
                    if (recentre_logs_ < 50) {
                        ++recentre_logs_;
                        log("red: seated aim recentring (look yaw %.2f -> %.2f): stick nudge", last_yaw_, now);
                    }
                }
                last_yaw_ = now;
                have_last_yaw_ = true;
            }
        } else {
            have_last_yaw_ = false;
        }
        if (drawn && sources_ok_ && nudge_frames_ > 0) {
            f.stick_nudge_x = (nudge_frames_ % 2) ? kSeatedNudge : -kSeatedNudge;
            --nudge_frames_;
        }
        if (fpp_ && seated_ && !(drawn && sources_ok_)) {
            f.view_id = 0;
            f.no_camera = false;
        }
        if (kind != Kind::Cinematic)
            if (auto z = zoom_fov()) f.zoom_fov = *z;
        f.target = std::string(kind_name(kind)) + active_desc_;
        if (++frames_ % 600 == 0 && active)
            log("red: active camera fov %.2f, %s", active_.fov, f.target.c_str());
        return f;
    }

    // Keeps the pitch within the FPP camera's limits (the look node clamps to its own, the same +-80 in 2.50).
    void limit_turn(double& pitch, double& yaw) override {
        if (!sources_ok_) return;
        if (seated_ && yaw != 0) {
            float now = 0;
            if (rd(look_ + kLookYaw, now)) {  // the yaw goes down to the right
                if (yaw > 0) yaw = std::fmax(0.0, std::fmin(yaw, double(now) + kLookYawLimit));
                else yaw = std::fmin(0.0, std::fmax(yaw, double(now) - kLookYawLimit));
            }
        }
        if (pitch == 0) return;
        float lo = -80, hi = 80;
        if (!rd(fpp_ + kFppPitchMin, lo) || !rd(fpp_ + kFppPitchMax, hi) || !(lo >= -90 && lo < 0 && hi > 0 && hi <= 90))
            lo = -80, hi = 80;
        const double now = look_pitch();
        if (pitch > 0) pitch = std::fmax(0.0, std::fmin(pitch, hi - now));
        else pitch = std::fmin(0.0, std::fmax(pitch, lo - now));
    }

    bool apply(double pitch, double yaw) override {
        if (!sources_ok_) return false;
        bool ok = true;
        if (yaw != 0 && seated_) {
            float y = 0;
            ok = rd(look_ + kLookYaw, y);
            if (ok) {
                y = float(y - yaw);
                ok = store(look_ + kLookYaw, &y, sizeof y);
                if (ok) last_yaw_ = y;  // the recentring check compares the next frame against our write
            }
        } else if (yaw != 0) {
            float q[4];
            ok = rd(body_ + kBodyOrientation, q);
            if (ok) {
                // A turn to the right is a negative rotation about Z (up), applied in world space.
                const double half = -yaw * kPi / 360, s = sin(half), c = cos(half);
                double r[4] = {c * q[0] - s * q[1], c * q[1] + s * q[0], c * q[2] + s * q[3], c * q[3] - s * q[2]};
                const double n = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
                const float out[4] = {float(r[0] / n), float(r[1] / n), float(r[2] / n), float(r[3] / n)};
                ok = n > 0.5 && store(body_ + kBodyOrientation, out, sizeof out);
            }
        }
        if (pitch != 0 && ok) {
            const float p = float(look_pitch() + pitch);
            ok = store(look_ + kLookPitch, &p, sizeof p);
        }
        return ok;
    }

    // No angles for the core's stick fallback: the sources written are checked here every frame (resolve_sources),
    // and where they aren't found the fallback runs blind anyway. With angles, the frames right after GyroLib's panel
    // closed (the camera data not updated yet) counted as undone writes and moved the on-foot view to the stick for
    // good (MODLOG 2026-11-04).
    bool view_angles(double& /*pitch*/, double& /*yaw*/, uintptr_t& /*camera*/) override { return false; }

    // Only the mouse cursor tells menus apart for now.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (cursor_) return "view=menu (cursor)";
        return std::string("view=") + kind_name(kind_) + active_desc_ +
               (!sources_ok_ ? " (gyro as right stick)" : seated_ ? " (seated: look yaw + pitch)" : " (body yaw + look pitch)");
    }

private:
    // Seated with a weapon out: two FPP camera values (not reflected) that are 10 / 50 while driving and 0 with a
    // weapon drawn in the seat (sound-cued sampling, confirmed in game; MODLOG 2026-11-04).
    bool seated_weapon_drawn() const {
        float a = 1, b = 1;
        return rd(fpp_ + kFppSeatedCombatA, a) && rd(fpp_ + kFppSeatedCombatB, b) && a == 0 && b == 0;
    }

    float look_pitch() const {
        float p = 0;
        return rd(look_ + kLookPitch, p) ? p : 0;
    }

    // Finds and checks the on-foot view's sources (constants above); "" when both can be written, else why not.
    std::string resolve_sources() {
        body_ = look_ = 0;
        seated_ = false;
        if (!fpp_ || !is_a(fpp_owner_, h("PlayerPuppet"))) return "no player FPP camera";
        // Yaw: the mover's body orientation.
        if (fpp_owner_ != mover_owner_ || class_hash(mover_) != h("moveComponent")) {
            mover_ = 0;
            mover_owner_ = fpp_owner_;
            each_handle(fpp_owner_ + kEntityComponents, 2048, [&](uintptr_t c) {
                if (!mover_ && class_hash(c) == h("moveComponent")) mover_ = c;
            });
        }
        if (!mover_) return "no moveComponent";
        uint32_t mode = 0;
        uint8_t mover_on = 0;
        uint32_t follows = 0, seated = 0;
        if (!rd(mover_ + kMoverBodyFollowsInput, follows) || !rd(fpp_ + kFppSeated, seated) || follows > 1 || seated > 1 ||
            follows == seated)
            return "neither on foot nor seated";
        seated_ = seated != 0;
        const uintptr_t look = look_node();
        if (!look) return "no animation graph";
        if (seated_) {
            float yaw = 0, extra = 0, total = 0, out = 0;
            if (!rd(look + kLookYaw, yaw) || !rd(look + kLookYawExtra, extra) || !rd(look + kLookYawTotal, total) ||
                !rd(fpp_ + kFppYawOut, out))
                return "the look node can't be read";
            if (!(fabs(yaw) <= kLookYawLimit + 1)) return "the look yaw is out of range";
            if (look != trusted_seated_look_) {
                if (!(fabs(yaw + extra - total) < 0.05f && fabs(total - out) < 10)) return "the look yaw isn't where expected";
                trusted_seated_look_ = look;
            }
            if (const std::string why = check_look_pitch(look); !why.empty()) return why;
            look_ = look;
            return "";
        }
        if (!rd(mover_ + kMoverMode, mode) || !rd(mover_ + kMoverActive, mover_on) || !mode || !mover_on)
            return "the mover doesn't take the body orientation now";
        const uintptr_t body = rdp(mover_ + kMoverBody);
        float q[4];
        if (!body || !rd(body + kBodyOrientation, q)) return "no body orientation";
        // The value checks run once per object found: right after our own write the camera (and the FPP pitch below)
        // still hold last frame's view, so checking them every frame failed on every frame we turned the view and
        // flipped between this and the stick each frame. Once trusted, only the structure is checked.
        double body_pitch, body_yaw, cam_pitch, cam_yaw;
        angles(q, body_pitch, body_yaw);
        const double n = double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3];
        if (!(fabs(n - 1) < 0.01) || fabs(body_pitch) > 1) return "the body orientation isn't a heading";
        if (body != trusted_body_) {
            angles(active_.q, cam_pitch, cam_yaw);
            double dyaw = fmod(fabs(body_yaw - cam_yaw), 360.0);
            if (dyaw > 180) dyaw = 360 - dyaw;
            if (dyaw > 5) return "the body doesn't face the camera's way";
            trusted_body_ = body;
        }
        if (const std::string why = check_look_pitch(look); !why.empty()) return why;
        body_ = body;
        look_ = look;
        return "";
    }

    // The look node's state in the player's animation graph (path above), 0 if not there.
    uintptr_t look_node() const {
        const uintptr_t animated = rdp(fpp_ + kFppAnimated);
        const uintptr_t camera_animated = animated && is_a(animated, h("entAnimatedComponent"))
                                              ? rdp(animated + kAnimatedCamera) : 0;
        if (!camera_animated || !is_a(camera_animated, h("entAnimatedComponent"))) return 0;
        const uintptr_t graph = rdp(camera_animated + kAnimatedGraph);
        const uintptr_t vars = graph ? rdp(graph + kGraphVariables) : 0;
        const uintptr_t base = vars ? rdp(vars) : 0;
        return base ? base + kLookState : 0;
    }

    // The look node's pitch: readable, in range and (once per node) matching the camera; "" when it can be written.
    std::string check_look_pitch(uintptr_t look) {
        float fov = 0, pitch = 0, extra = 0, total = 0, out = 0;
        if (!rd(look + kLookFov, fov) || !rd(look + kLookPitch, pitch) || !rd(look + kLookPitchExtra, extra) ||
            !rd(look + kLookPitchTotal, total) || !rd(fpp_ + kFppPitchOut, out))
            return "the look node can't be read";
        if (!(pitch >= -90 && pitch <= 90)) return "the look pitch is out of range";
        if (look != trusted_look_) {
            if (!(fabs(fov - active_.fov) < 0.5f && fabs(pitch - out) < 2 && fabs(pitch + extra - total) < 0.05f))
                return "the look node isn't where expected";
            trusted_look_ = look;
        }
        return "";
    }

    // Finds the camera components: references in the camera system (pointers, handles and arrays of handles) and the
    // player's own components. Logged when the set changes.
    void scan() {
        std::vector<Candidate> found;
        const auto consider = [&](uintptr_t p, const std::string& from) {
            if (!p || !is_a(p, h("entBaseCameraComponent"))) return;
            for (const Candidate& c : found)
                if (c.component == p) return;
            Candidate c{p, class_hash(p), owner_of(p), 0, from};
            c.owner_cls = class_hash(c.owner);
            found.push_back(c);
        };
        const auto scan_object = [&](uintptr_t obj, uintptr_t size, const char* what) {
            char from[64];
            for (uintptr_t off = 0x40; off + 8 <= size; off += 8) {
                snprintf(from, sizeof from, "%s+0x%llx", what, static_cast<unsigned long long>(off));
                consider(rdp(obj + off), from);
                each_handle(obj + off, 256, [&](uintptr_t e) { consider(e, std::string(from) + "[]"); });
            }
        };
        // The player first (a PlayerPuppet handle in the player system, then its components, an array of handles), so
        // its FPP camera comes first in the list; then the camera system's references.
        player_ = 0;
        if (const uintptr_t ps = game_system(h("gamePlayerSystem"))) {
            for (uintptr_t off = 0x48; off + 8 <= kPlayerSystemSize && !player_; off += 8)
                if (const uintptr_t p = rdp(ps + off); p && is_a(p, h("PlayerPuppet"))) player_ = p;
        }
        if (player_) scan_object(player_, kEntitySize, "player");
        scan_object(camera_system_, kCameraSystemSize, "camera system");

        bool changed = found.size() != candidates_.size();
        for (size_t i = 0; !changed && i < found.size(); ++i) changed = found[i].component != candidates_[i].component;
        if (changed) {
            log("red: %zu camera component(s)%s", found.size(), player_ ? "" : " (no player)");
            for (const Candidate& c : found)
                log("red:   %s at %s, owner %s, from %s", describe_class(c.cls).c_str(), mem::module_offset(c.component).c_str(),
                    c.owner ? describe_class(c.owner_cls).c_str() : "none", c.from.c_str());
        }
        candidates_ = std::move(found);
    }

    // The offset where a component of this class stores the orientation the active camera data copies, learnt by
    // matching (16-byte aligned quaternions in the component); 0 = not found.
    uintptr_t orientation_offset(const Candidate& c) {
        if (const auto it = orientation_at_.find(c.cls); it != orientation_at_.end()) {
            float q[4];
            if (it->second && rd(c.component + it->second, q) && same_orientation(q, active_.q)) return it->second;
            return 0;
        }
        for (uintptr_t off = 0x90; off + 16 <= kComponentSize; off += 0x10) {
            float q[4];
            if (!rd(c.component + off, q)) break;
            const double n = double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3];
            if (fabs(n - 1.0) < 0.01 && same_orientation(q, active_.q)) {
                orientation_at_[c.cls] = off;
                log("red: %s keeps the active camera's orientation at +0x%llx", describe_class(c.cls).c_str(),
                    static_cast<unsigned long long>(off));
                return off;
            }
        }
        return 0;
    }

    // The current and unzoomed FOV for GyroLib's zoom compensation: the active camera data's FOV and the active camera
    // component's own (entBaseCameraComponent.fov). Both are reported as vertical (not verified against the rendered
    // view: the 70-100 FOV setting is not one of them; 80 gives 60 and 90 gives 69). Changes are logged (a few).
    std::optional<ZoomFov> zoom_fov() {
        float reference = 0;
        if (!matched_ || !rd(matched_ + kComponentFov, reference)) return std::nullopt;
        float zoom = 1;
        if (!rd(camera_system_ + kCameraZoom, zoom) || !(zoom >= 1 && zoom <= 100)) zoom = 1;
        const double current = atan(tan(active_.fov * kPi / 360) / zoom) * 360 / kPi;
        if (!fov::is_valid(current) || !fov::is_valid(reference)) return std::nullopt;
        if ((fabs(current - logged_fov_) > 2 || fabs(reference - logged_reference_) > 0.05) && fov_logs_ < 200) {
            ++fov_logs_;
            log("red: FOV %.2f (camera %.2f, zoom x%.2f; unzoomed %.2f)", current, double(active_.fov), double(zoom),
                double(reference));
            logged_fov_ = current;
            logged_reference_ = reference;
        }
        return ZoomFov{current, reference};
    }

    // Which camera is active, by matching its orientation; the scan runs every second and whenever nothing matches.
    Kind classify() {
        if (!scanned_ || frames_ - last_scan_ >= (matched_ ? 60u : 30u)) {
            scan();
            scanned_ = true;
            last_scan_ = frames_;
        }
        const Candidate* hit = nullptr;
        bool vehicle_camera = false;
        for (const Candidate& c : candidates_) {
            // Unverified: a vehicle camera may also be listed while the player is on foot; the log will tell.
            if (c.owner && is_a(c.owner, h("vehicleBaseObject"))) vehicle_camera = true;
            // Cameras that look the same way all match: the first one wins, and the player's are listed first.
            if (!hit && orientation_offset(c)) hit = &c;
        }
        matched_ = hit ? hit->component : 0;
        const bool fpp = hit && hit->cls == h("gameFPPCameraComponent");
        fpp_ = fpp ? hit->component : 0;
        fpp_owner_ = fpp ? hit->owner : 0;
        if (!hit) {
            active_desc_.clear();
            // Until some camera has matched once, an unmatched one is more likely a missing scan than a cinematic.
            return orientation_at_.empty() ? Kind::Unknown : Kind::Cinematic;
        }
        active_desc_ = " (" + describe_class(hit->cls) + (hit->owner ? " on " + describe_class(hit->owner_cls) : "") + ")";
        const auto vehicle_kind = [&](uintptr_t vehicle) {
            return is_a(vehicle, h("vehicleBikeBaseObject")) ? Kind::Motorcycle : Kind::Car;
        };
        if (hit->cls == h("vehicleTPPCameraComponent")) return vehicle_kind(hit->owner);
        if (hit->cls == h("gameFPPCameraComponent")) {
            if (!vehicle_camera) return Kind::OnFoot;
            // First person in a vehicle: the vehicle whose camera is listed.
            for (const Candidate& c : candidates_)
                if (c.owner && is_a(c.owner, h("vehicleBaseObject"))) return vehicle_kind(c.owner);
        }
        return Kind::Cinematic;
    }

    bool cursor_ = false;
    double logged_fov_ = 0, logged_reference_ = 0;
    int fov_logs_ = 0;
    bool have_active_ = false;
    ActiveCamera active_{};
    uintptr_t camera_system_ = 0, player_ = 0, matched_ = 0;
    std::vector<Candidate> candidates_;
    std::unordered_map<uint64_t, uintptr_t> orientation_at_;  // class hash -> offset of its world orientation
    Kind kind_ = Kind::Unknown;
    std::string active_desc_;
    uint64_t frames_ = 0, last_scan_ = 0;
    bool scanned_ = false;
    // The on-foot sources: the active FPP camera and its owner, the player's mover, the body and the look node state.
    uintptr_t fpp_ = 0, fpp_owner_ = 0, mover_ = 0, mover_owner_ = 0, body_ = 0, look_ = 0;
    uintptr_t trusted_body_ = 0, trusted_look_ = 0, trusted_seated_look_ = 0;
    bool logged_drawn_ = false;
    int nudge_frames_ = 0, nudge_cooldown_ = 0, recentre_logs_ = 0;
    float last_yaw_ = 0;
    bool have_last_yaw_ = false;
    bool seated_ = false;  // seated in a vehicle in first person: the look node turns the head  // sources whose values matched the camera once
    bool sources_ok_ = false;
    std::string sources_why_ = "-";  // why the sources can't be written ("" = they can), logged when it changes
};

}  // namespace

Engine& engine() {
    static RedEngine e;
    return e;
}

}  // namespace gyro4all::red
