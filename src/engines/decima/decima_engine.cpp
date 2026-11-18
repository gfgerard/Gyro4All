// Decima engine module: Death Stranding (and its Director's Cut), Horizon Zero Dawn (PC builds), and Nixxes' later ports
// (Horizon Forbidden West; Death Stranding 2, not tested).
//
// Detection: the exe's name, with the engine's data layout next to it: a folder of .bin archives (data\ in Death
// Stranding, Packed_DX12\ in Horizon Zero Dawn); or, whatever the exe is called, Nixxes' streamed package
// (LocalCacheWinGame\package\streaming_graph.core, Horizon Forbidden West).
// Without it no engine matched and the Unreal fallback scanned the game's memory: its guarded reads fault, and
// Death Stranding's crash reporter (a vectored exception handler) sees the first-chance access violation, writes
// error.txt and shows an "Access Violation" window although Gyro4All handled it.
//
// Camera (Death Stranding Director's Cut, MODLOG 2026-11-15): the active camera mode, as the game's own getter finds it
// (the function its script export SetCameraRotationHeading's consumer calls): a global list of players, the one whose
// id is 0, then three pointers. The mode keeps the view's pitch and heading as float radians, read and written by the
// camera's update; adding to them turns the view (pitch: + up; heading: + left). The global, the
// pointer offsets and the angle offsets are read from the game's code by signature, so other builds can match.
//
// Camera (Horizon Zero Dawn, MODLOG 2026-11-17): the local player's soldier, its ThirdPersonPlayerCameraComponent
// (found by RTTI class wherever the run put it, or by its vtable), the component's orbit mode and that mode's rotation
// matrix, turned directly. All reads and writes go through ReadProcessMemory/WriteProcessMemory: the crash reporter
// closes the game on any first-chance access violation. FOV: the component's horizontal degrees.
//
// Camera (Horizon Forbidden West, MODLOG 2026-11-17): the same ThirdPersonPlayerCameraComponent class (found by its
// vtable: one instance), but the view is two float radians in the component itself (pitch, heading; + up, + left),
// their offsets read from the game's code by signature. The component's and the camera entity's matrices are copies
// the game rewrites every frame.
#include "decima.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <thread>
#include <vector>

namespace gyro4all::decima {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool has_hashed_data(const std::wstring& dir) {
    for (const wchar_t* sub : {L"\\data\\*.bin", L"\\Packed_DX12\\*.bin"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + sub).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        return true;
    }
    return false;
}

// Nixxes' Decima ports (Horizon Forbidden West; Death Stranding 2 presumably the same): the streamed package next to
// the exe.
bool nixxes_layout(const std::wstring& dir) {
    return GetFileAttributesW((dir + L"\\LocalCacheWinGame\\package\\streaming_graph.core").c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

std::wstring exe_dir(const std::wstring& exe_path) {
    const size_t slash = exe_path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : exe_path.substr(0, slash);
}

bool decima_layout(const std::wstring& exe_path) {
    const size_t slash = exe_path.find_last_of(L"\\/");
    const std::wstring dir = exe_dir(exe_path);
    if (nixxes_layout(dir)) return true;
    std::wstring stem = lower(slash == std::wstring::npos ? exe_path : exe_path.substr(slash + 1));
    if (const size_t dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
    static const wchar_t* const stems[] = {L"deathstranding", L"horizonzerodawn"};
    return std::any_of(std::begin(stems), std::end(stems), [&](const wchar_t* s) { return stem == s; }) &&
           has_hashed_data(dir);
}

// The camera mode getter: mov rcx,[rip+G]; ... call players_find(G, 0); [rax+A]; [rax+B]-0x20; [rcx+C]-0x20.
constexpr const char* kModeGetter =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 33 D2 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 40 ?? 48 85 C0 74 ?? "
    "48 8B 80 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0 48 0F 44 CA 48 85 C9 74 ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9 48 8D 41 E0";
// The rotation request's consumer: movzx ecx,[rbx+flags]; vmovss xmm3,[rax+pitch]; vmovss xmm4,[rax+heading]; test cl,20h.
constexpr const char* kModeAngles = "0F B6 8B ?? ?? ?? ?? C5 FA 10 98 ?? ?? ?? ?? C5 FA 10 A0 ?? ?? ?? ?? F6 C1 20";

struct Layout {
    uintptr_t players = 0;       // the global holding the player list
    uint32_t a = 0, b = 0, c = 0;  // player -> +a -> +b (-0x20) -> +c (-0x20) = the camera mode
    uint32_t pitch = 0, heading = 0;
};

std::vector<uintptr_t> find(const char* pattern) {
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(pattern, bytes, mask);
    return mem::find_pattern(mem::code_sections(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))), bytes.data(),
                             mask.data(), bytes.size());
}

template <class T>
T at(uintptr_t p) {
    T v{};
    mem::rd(p, v);
    return v;
}

void read_getter(uintptr_t g, Layout& l) {
    l.players = g + 11 + at<int32_t>(g + 7);
    l.a = at<uint8_t>(g + 31);
    l.b = at<uint32_t>(g + 40);
    l.c = at<uint32_t>(g + 65);
}

bool find_layout(Layout& l) {
    const auto getters = find(kModeGetter);
    const auto angles = find(kModeAngles);
    // The getter exists in several identical copies (Death Stranding DC: two): fine as long as they all agree.
    bool agree = !getters.empty();
    if (agree) read_getter(getters[0], l);
    for (size_t i = 1; agree && i < getters.size(); ++i) {
        Layout o;
        read_getter(getters[i], o);
        agree = o.players == l.players && o.a == l.a && o.b == l.b && o.c == l.c;
    }
    if (!agree || angles.size() != 1) {
        log("decima: camera code not found (mode getter %zu match(es)%s, angle reads %zu): gyro off", getters.size(),
            getters.size() > 1 && !agree ? " that differ" : "", angles.size());
        return false;
    }
    const uintptr_t g = getters[0], an = angles[0];
    l.pitch = at<uint32_t>(an + 11);
    l.heading = at<uint32_t>(an + 19);
    log("decima: camera mode getter %s (players %s, +0x%x +0x%x +0x%x), angles +0x%x pitch +0x%x heading (%s)",
        mem::module_offset(g).c_str(), mem::module_offset(l.players).c_str(), l.a, l.b, l.c, l.pitch, l.heading,
        mem::module_offset(an).c_str());
    return l.a && l.b && l.c && l.pitch && l.heading;
}

// The game's getter, with guarded reads: 0 while there is no player or camera mode (loading, front end).
uintptr_t camera_mode(const Layout& l) {
    const uintptr_t list = mem::rdp(l.players);
    if (!list) return 0;
    const uintptr_t items = mem::rdp(list + 0x40);
    int32_t count = 0;
    if (!items || !mem::rd(list + 0x38, count) || count <= 0 || count > 64) return 0;
    uintptr_t player = 0;
    for (int32_t i = 0; i < count && !player; ++i) {
        const uintptr_t p = mem::rdp(items + 8 * static_cast<uintptr_t>(i));
        int32_t id = -1;
        if (p && mem::rd(p + 0x88, id) && id == 0) player = p;
    }
    if (!player) return 0;
    const uintptr_t x = mem::rdp(player + l.a);
    const uintptr_t y = x ? mem::rdp(x + l.b) : 0;
    if (!y) return 0;
    const uintptr_t z = mem::rdp(y - 0x20 + l.c);
    return z ? z - 0x20 : 0;
}

constexpr double kRad = 3.14159265358979323846 / 180.0;

// Horizon Zero Dawn (2020 PC port, MODLOG 2026-11-16): no per-angle floats. The third-person camera mode
// (CameraOrbitFollow, what the script export ThirdPersonPlayerCameraComponent_ExportedGetCurrentCameraMode returns:
// component+0x70) keeps the view as a 3x3 rotation matrix at +0x108, rows right, forward, up, Z up; the game keeps a
// written matrix and turns the view from it (write test: 90 degrees about Z turned the view). The component is reached
// from the local player, as Player_sExportedGetLocalPlayer finds it: a global player list (read from that getter's code
// by signature), PlayerGame +0x68 Soldier +0x238 NetSoldier +0x500 the component. Each object's RTTI class is checked,
// so another layout turns the gyro off instead of writing elsewhere.
constexpr const char* kHzdLocalPlayer =
    "44 8B 1D ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8B 0D ?? ?? ?? ?? 44 8B 15 ?? ?? ?? ?? 41 83 FB 01 75 ?? "
    "41 83 FA 02 75 ?? 4D 85 C9 75 ?? 48 63 05 ?? ?? ?? ?? 48 8B 7C C1 48";
constexpr uint32_t kHzdMatrix = 0x108;
// The component's own view matrix, same row layout (right, forward, up). Usually copied from the orbit mode every frame,
// but held for about two seconds after aiming with L2 starts (bow drawn), while the aim mode's matrix turned with the
// gyro and the view didn't follow (recorded live, 2026-11-17): turned too in that window.
constexpr uint32_t kHzdComponentMatrix = 0xC0;
constexpr uint32_t kHzdComponentFov = 0xE8;  // float, horizontal degrees (see hzd_fov)
constexpr double kHzdPitchLimit = 80;  // degrees up or down the gyro may take the view
constexpr ULONGLONG kHzdHoldMs = 3000;  // how long after a mode change the component's own matrix is turned too
constexpr ULONGLONG kHzdBlendMs = 1500;  // how long the orbit mode the camera left is still turned (its blend)

// Reads with ReadProcessMemory, which fails without an exception. Decima's crash reporter sees every first-chance access
// violation, even one a guarded read catches, and closes the game (Death Stranding shows its "Access Violation" window;
// Horizon Zero Dawn closed while a save loaded and its player objects were half built). Every read of a pointer chain
// in this module goes through here.
bool quiet_read(uintptr_t at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}
template <class T>
bool qrd(uintptr_t at, T& out) {
    return at && quiet_read(at, &out, sizeof out);
}
bool quiet_write(uintptr_t at, const void* in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), in, n, &put) && put == n;
}
uintptr_t qrdp(uintptr_t at) {
    uintptr_t v = 0;
    return qrd(at, v) && mem::plausible(v) ? v : 0;
}

struct HzdLayout {
    uintptr_t players = 0, index = 0;  // the player list global and the local player's index global
};

bool find_hzd_layout(HzdLayout& l) {
    const auto hits = find(kHzdLocalPlayer);
    if (hits.size() != 1) {
        log("decima: Horizon Zero Dawn's local player getter not found (%zu match(es)): gyro off", hits.size());
        return false;
    }
    const uintptr_t g = hits[0];
    l.players = g + 14 + at<int32_t>(g + 10);
    l.index = g + 52 + at<int32_t>(g + 48);
    log("decima: local player getter %s (players %s, index %s)", mem::module_offset(g).c_str(),
        mem::module_offset(l.players).c_str(), mem::module_offset(l.index).c_str());
    return true;
}

// Horizon Forbidden West: the component's view angles, where the camera update loads them as a vector (pitch, roll,
// heading): vmovups [rbx+S],xmm0; vmovsd xmm0,[rbx+P]; vinsertps xmm0,xmm0,[rbx+P+8],20h; vmulps xmm0,[rip+...].
// The block exists twice (MODLOG 2026-11-17): the second loads what the first stores (a derived copy, +0x490 from
// +0x4d0); the source is the block whose angles no block stores.
constexpr const char* kCompAngles =
    "C5 F8 11 83 ?? ?? ?? ?? C5 FB 10 83 ?? ?? ?? ?? C4 E3 79 21 83 ?? ?? ?? ?? 20 C5 F8 59 05";

constexpr const char* kCompClass = ".?AVThirdPersonPlayerCameraComponent@@";
constexpr uint32_t kCompFov = 0xE0;  // float degrees, the camera component's current FOV (see read_comp_frame)

struct CompAngles {
    uint32_t pitch = 0, heading = 0;
};

bool find_comp_angles(CompAngles& l) {
    const auto hits = find(kCompAngles);
    std::vector<uint32_t> stores, loads;
    for (const uintptr_t h : hits) {
        stores.push_back(at<uint32_t>(h + 4));
        loads.push_back(at<uint32_t>(h + 12));
    }
    int source = -1, sources = 0;
    for (size_t i = 0; i < hits.size(); ++i)
        if (std::find(stores.begin(), stores.end(), loads[i]) == stores.end() &&
            at<uint32_t>(hits[i] + 21) == loads[i] + 8) {
            source = static_cast<int>(i);
            ++sources;
        }
    if (sources != 1) {
        log("decima: the camera component's angle code not found (%zu match(es), %d source(s)): gyro off", hits.size(),
            sources);
        return false;
    }
    l.pitch = loads[source];
    l.heading = loads[source] + 8;
    log("decima: camera component angles +0x%x pitch, +0x%x heading (%s)", l.pitch, l.heading,
        mem::module_offset(hits[source]).c_str());
    return l.pitch > 0 && l.pitch < 0x4000;
}

// Death Stranding 2 (Nixxes, MODLOG 2026-11-18): the class above is subclassed (DSThirdPersonPlayerCameraComponent) and
// its +0x470 angles stay still; the view's pitch and heading are radians in the one DSCameraMode, as in Death Stranding
// (DC)'s camera mode (write-tested: both kept, the component's matrix and PlayerGame's copy follow). Read by
// vmovss xmm0,[reg+pitch]; vmovss xmm15,[reg+heading]; vsubss xmm0,xmm0,xmm8; call (two copies).
constexpr const char* kDsModeAngles = "C5 FA 10 ?? ?? ?? ?? ?? C5 7A 10 ?? ?? ?? ?? ?? C4 C1 7A 5C C0 E8";
constexpr const char* kDsModeClass = ".?AVDSCameraMode@@";

bool find_ds_mode_angles(CompAngles& l) {
    const auto hits = find(kDsModeAngles);
    bool agree = !hits.empty();
    for (size_t i = 0; agree && i < hits.size(); ++i) {
        const uint32_t p = at<uint32_t>(hits[i] + 4), h = at<uint32_t>(hits[i] + 12);
        if (i == 0) l.pitch = p, l.heading = h;
        agree = p == l.pitch && h == l.heading && h == p + 4 && p > 0 && p < 0x10000;
    }
    if (!agree) {
        log("decima: DSCameraMode's angle code not found (%zu match(es)%s)", hits.size(), hits.empty() ? "" : " that differ");
        return false;
    }
    log("decima: DSCameraMode angles +0x%x pitch, +0x%x heading (%s, %zu match(es))", l.pitch, l.heading,
        mem::module_offset(hits[0]).c_str(), hits.size());
    return true;
}

// The MSVC RTTI class of `obj` is `name` (".?AV...@@"), with guarded reads.
bool is_class(uintptr_t obj, const char* name) {
    static const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t vt = obj ? qrdp(obj) : 0;
    const uintptr_t col = vt ? qrdp(vt - 8) : 0;
    uint32_t td = 0;
    if (!col || col < exe || !qrd(col + 12, td) || !td) return false;
    char buf[64] = {};
    const size_t n = std::strlen(name) + 1;
    if (n > sizeof buf) return false;
    for (size_t i = 0; i < n; ++i)
        if (!qrd(exe + td + 16 + i, buf[i])) return false;
    return std::memcmp(buf, name, n) == 0;
}

// The orbit camera mode, 0 when the player or that mode isn't there (front end, loading, other camera modes).
// The exe's vtable of an MSVC RTTI class (".?AV...@@", complete object at offset 0), 0 if not found. Reads only the exe's
// own image (type descriptor name, complete object locator, the pointer to it before the vtable).
uintptr_t find_vtable(const char* name) {
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t len = std::strlen(name) + 1;
    uint32_t td = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !td; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const char* b = reinterpret_cast<const char*>(exe + sec[i].VirtualAddress);
        const size_t n = sec[i].Misc.VirtualSize;
        for (size_t o = 16; o + len <= n; o += 8)  // the name follows the descriptor's two pointers (8-aligned)
            if (b[o] == name[0] && std::memcmp(b + o, name, len) == 0) {
                td = uint32_t(sec[i].VirtualAddress + o - 16);
                break;
            }
    }
    if (!td) return 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t b = exe + sec[i].VirtualAddress;
        const size_t n = sec[i].Misc.VirtualSize & ~size_t(7);
        for (size_t o = 0; o + 24 <= n; o += 4) {
            const auto* col = reinterpret_cast<const uint32_t*>(b + o);
            if (col[0] != 1 || col[1] != 0 || col[3] != td) continue;  // signature 1, offset 0, this type
            const uintptr_t col_at = b + o;
            for (int j = 0; j < nt->FileHeader.NumberOfSections; ++j) {
                if (!(sec[j].Characteristics & IMAGE_SCN_MEM_READ) || (sec[j].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                const auto* q = reinterpret_cast<const uintptr_t*>(exe + sec[j].VirtualAddress);
                const size_t m = sec[j].Misc.VirtualSize / 8;
                for (size_t k = 0; k + 1 < m; ++k)
                    if (q[k] == col_at) return reinterpret_cast<uintptr_t>(q + k + 1);
            }
        }
    }
    return 0;
}

// The one object whose vtable is `vt`, searched through the process's private read-write memory with ReadProcessMemory
// (Decima's crash reporter closes the game on any access violation). Seconds for gigabytes: on its own thread. 0 if
// none, or more than one (not "the" object).
uintptr_t find_object_by_vtable(uintptr_t vt, int* instances = nullptr) {
    std::vector<uintptr_t> buf(1 << 17);  // 1 MB
    uintptr_t found = 0;
    int count = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    // Thread stacks (the allocations holding a guard page) are skipped: locals there hold copies of the vtable pointer
    // (Horizon Forbidden West: hits on the stacks while loading, and "several" in game; MODLOG 2026-11-18).
    std::unordered_set<uintptr_t> stacks;
    for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi); ) {
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= a) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD))
            stacks.insert(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
        a = end;
        if (a >= 0x7FFFFFFF0000ull) break;
    }
    for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi); ) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (end <= a) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE &&
            !stacks.count(reinterpret_cast<uintptr_t>(mbi.AllocationBase)))
            for (uintptr_t p = base; p < end; p += buf.size() * 8) {
                const size_t n = std::min<size_t>(buf.size() * 8, end - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got)) continue;
                for (size_t i = 0; i < got / 8; ++i)
                    if (buf[i] == vt && ++count <= 2) found = p + i * 8;
            }
        a = end;
        if (a >= 0x7FFFFFFF0000ull) break;
    }
    if (instances) *instances = count;
    return count == 1 ? found : 0;
}

// The heap search for the HZD camera component when its pointer path isn't found (another layout after loading a save):
// one search at a time, its result taken by the game thread.
struct HzdComponentSearch {
    std::atomic<bool> running{false};
    std::atomic<uintptr_t> result{0};
    const char* cls = kCompClass;  // the class searched for (Death Stranding 2: its DSCameraMode)
    uintptr_t vtable = 0;
    bool vtable_looked = false;
    ULONGLONG next = 0;
    void start() {
        if (running || GetTickCount64() < next) return;
        next = GetTickCount64() + 5000;
        if (!vtable_looked) {
            vtable_looked = true;
            vtable = find_vtable(cls);
            log("decima: %s's vtable %s", cls,
                vtable ? mem::module_offset(vtable).c_str() : "not found: no memory search for the camera");
        }
        if (!vtable) return;
        running = true;
        std::thread([this] {
            const ULONGLONG t0 = GetTickCount64();
            int n = 0;
            const uintptr_t at = find_object_by_vtable(vtable, &n);
            result = at;
            log("decima: memory search for the %s: %s in %llu ms", cls,
                at ? mem::module_offset(at).c_str() : (std::to_string(n) + " instances: none taken").c_str(),
                GetTickCount64() - t0);
            running = false;
        }).detach();
    }
} g_hzd_search;

uintptr_t hzd_camera_mode(const HzdLayout& l, std::string* why = nullptr, uintptr_t* component = nullptr) {
    auto fail = [&](const char* step) {
        if (why) *why = step;
        return uintptr_t(0);
    };
    const uintptr_t list = qrdp(l.players);
    int32_t i = 0;
    if (!list) return fail("no player list");
    if (!qrd(l.index, i) || i < 0 || i > 15) return fail("no local player index");
    const uintptr_t player = qrdp(list + 0x48 + 8 * static_cast<uintptr_t>(i));
    if (!player) return fail("no player");
    const uintptr_t soldier = qrdp(player + 0x68);
    if (!soldier) return fail("no soldier (player+0x68)");
    // The soldier's CameraEntity and its ThirdPersonPlayerCameraComponent, found by class: fixed offsets of one run pointed
    // at unrelated objects in another (soldier+0x2d8 and component pointer +0x600 in one run). The soldier holds several
    // CameraEntity pointers (+0x1d0 and +0x2d8 in one run) and only one holds the component, which is sometimes a pointer
    // and sometimes held inside the entity (+0x840 in one run, with no pointer to it). After loading a save the entity
    // that held it may not anymore: then every CameraEntity of the soldier is searched again. The offsets found are reused
    // while the classes still match.
    static constexpr const char* kEntity = ".?AVCameraEntity@@";
    static constexpr const char* kComp = ".?AVThirdPersonPlayerCameraComponent@@";
    // The path found: soldier+entity_at -> CameraEntity [-> +mid_at, an object of it, when mid_at] -> +comp_at, a pointer
    // to the component or (inside) the component itself. After loading a save the component was in neither place, only
    // pointed to from an object of the CameraEntity (CameraEntity+0x28 -> +0x2b0 in one run).
    struct Path {
        uint32_t entity_at = 0, mid_at = 0, comp_at = 0;
        bool inside = false;
    };
    static Path path;
    auto follow = [&](const Path& p) -> uintptr_t {
        if (!p.entity_at || !p.comp_at) return 0;
        const uintptr_t entity = qrdp(soldier + p.entity_at);
        if (!entity || !is_class(entity, kEntity)) return 0;
        const uintptr_t holder = p.mid_at ? qrdp(entity + p.mid_at) : entity;
        if (!holder) return 0;
        const uintptr_t v = p.inside ? holder + p.comp_at : qrdp(holder + p.comp_at);
        return v && is_class(v, kComp) ? v : 0;
    };
    uintptr_t comp = follow(path);
    static uintptr_t searched = 0;  // the component the memory search found, kept while its class still matches
    if (!comp && searched) {
        if (is_class(searched, kComp)) comp = searched;
        else searched = 0;
    }
    if (!comp) {
        if (const uintptr_t r = g_hzd_search.result.exchange(0); r && is_class(r, kComp)) comp = searched = r;
    }
    if (!comp) {
        static ULONGLONG next_scan = 0;  // a search reads thousands of pointers: at most twice a second
        if (GetTickCount64() < next_scan) return fail("no ThirdPersonPlayerCameraComponent in the CameraEntity");
        next_scan = GetTickCount64() + 500;
        static const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        constexpr uint32_t kSize = 0x1000, kMidSize = 0x400;
        std::vector<uintptr_t> sol(kSize / 8), ent(kSize / 8), mid(kMidSize / 8);
        if (!quiet_read(soldier, sol.data(), kSize)) return fail("soldier unreadable");
        // The component in `holder` (its words in `w`): a pointer to it, or (embedded) it held inside.
        auto search = [&](uintptr_t holder, const std::vector<uintptr_t>& w, bool embedded, Path& p) -> uintptr_t {
            for (uint32_t c = 0; c < w.size(); ++c)
                if (mem::plausible(w[c]) && is_class(w[c], kComp)) return p.comp_at = c * 8, p.inside = false, w[c];
            if (embedded)
                for (uint32_t c = 1; c < w.size(); ++c)
                    if (w[c] > exe && w[c] - exe < 0x10000000 && is_class(holder + c * 8, kComp))
                        return p.comp_at = c * 8, p.inside = true, holder + c * 8;
            return 0;
        };
        bool any_entity = false;
        Path found;
        for (uint32_t e = 0; e < sol.size() && !comp; ++e) {
            if (!mem::plausible(sol[e]) || !is_class(sol[e], kEntity)) continue;
            any_entity = true;
            const uintptr_t entity = sol[e];
            if (!quiet_read(entity, ent.data(), kSize)) continue;
            found = {e * 8, 0, 0, false};
            comp = search(entity, ent, true, found);
            for (uint32_t m = 0; m < ent.size() && !comp; ++m) {  // one object further
                if (!mem::plausible(ent[m]) || ent[m] == entity || !quiet_read(ent[m], mid.data(), kMidSize)) continue;
                found.mid_at = m * 8;
                comp = search(ent[m], mid, false, found);
            }
        }
        if (!any_entity) return fail("no CameraEntity in the soldier");
        if (!comp) {
            g_hzd_search.start();  // the layout isn't one of those known: search the memory for it
            return fail("no ThirdPersonPlayerCameraComponent in the CameraEntity");
        }
        searched = 0;
        path = found;
        if (found.mid_at)
            log("decima: %s at soldier+0x%x, its +0x%x -> +0x%x: %s", kEntity, found.entity_at, found.mid_at, found.comp_at, kComp);
        else
            log("decima: %s at soldier+0x%x, %s %s +0x%x", kEntity, found.entity_at, kComp, found.inside ? "inside at" : "at",
                found.comp_at);
    }
    const uintptr_t mode = qrdp(comp + 0x70);
    if (!mode) return fail("no camera mode (component+0x70)");
    if (!is_class(mode, ".?AVCameraOrbitFollow@@")) return fail("camera mode isn't CameraOrbitFollow");
    if (!is_class(comp, ".?AVThirdPersonPlayerCameraComponent@@")) return fail("component isn't ThirdPersonPlayerCameraComponent");
    if (!is_class(soldier, ".?AVSoldier@@")) return fail("soldier isn't Soldier");
    if (component) *component = comp;
    return mode;
}

// The matrix's rows are unit vectors (a frame the game is using, not one being built).
bool hzd_matrix(uintptr_t mode, float m[9]) {
    for (int i = 0; i < 9; ++i)
        if (!qrd(mode + kHzdMatrix + 4 * i, m[i]) || !std::isfinite(m[i])) return false;
    for (int r = 0; r < 3; ++r) {
        const float* v = m + 3 * r;
        if (std::fabs(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - 1.f) > 0.02f) return false;
    }
    return true;
}

// The render view's FOV (Death Stranding DC, found by a zoom diff; MODLOG 2026-11-15): view objects a few pointers from
// the camera mode hold an inverse projection where tan(vertical FOV / 2) sits 0x40 bytes after its reciprocal and
// 0x28 bytes after tan(horizontal FOV / 2) (third person 45.7 degrees, first person 45.3, the binoculars' R3 zoom 23.3).
// The pointer path between the mode and those objects changes from one run to the next, so they are found by that
// pattern, walking the pointers out of the mode.
//
// That walk follows arbitrary pointers, so it reads with quiet_read.

// t at `p` (tan of half the vertical FOV, below 87 degrees), with 1/t at p - 0x40 and tan of half the horizontal FOV at
// p - 0x28, in the game window's aspect ratio (within 3%). Without the aspect check, square views (shadow or cube
// map projections, 90 degrees while loading) match too.
bool fov_pattern(const float* p, double aspect) {
    const float t = p[0], inv = p[-16], h = p[-10];
    // Written so that a NaN fails (Death Stranding 2: a NaN 1/tan next to a tan and a tan*aspect gave 25.4 degrees).
    if (!std::isfinite(t) || t < 0.05f || t > 0.95f || !(std::fabs(inv * t - 1.f) <= 1e-3f)) return false;
    return aspect > 0 && std::fabs(h / t / aspect - 1) < 0.03;
}

// Breadth-first through the pointers of the mode (3 levels), the first object holding the pattern: its t's address.
uintptr_t find_fov(uintptr_t mode, double aspect) {
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<std::pair<uintptr_t, size_t>> level{{mode, 0x2000}}, next;
    std::unordered_set<uintptr_t> seen;
    std::vector<uint8_t> buf;
    for (int depth = 0; depth <= 3 && !level.empty(); ++depth) {
        next.clear();
        for (const auto& [obj, want] : level) {
            // As much of the object as is readable (an object near the end of its memory region: Horizon Zero Dawn's,
            // where a whole 0x2000-byte read failed and the search found nothing).
            buf.resize(want);
            size_t got = 0;
            while (got < want && quiet_read(obj + got, buf.data() + got, 0x200)) got += 0x200;
            if (got < 0x80) continue;
            const size_t size = got;
            const float* f = reinterpret_cast<const float*>(buf.data());
            for (size_t i = 16; i < size / 4; ++i)
                if (fov_pattern(f + i, aspect)) return obj + 4 * i;
            if (depth == 3) continue;
            for (size_t o = 0; o + 8 <= size; o += 8) {
                uintptr_t v = 0;
                std::memcpy(&v, buf.data() + o, 8);
                if (!mem::plausible(v) || (v >= exe && v < exe + 0x10000000)) continue;
                if (!seen.insert(v).second) continue;
                if (seen.size() >= 20000) return 0;

                next.emplace_back(v, 0x800);
            }
        }
        level.swap(next);
    }
    return 0;
}

// The vertical FOV in degrees from the pattern at `at`, 0 if it's no longer there.
double vertical_fov(uintptr_t at, double aspect) {
    float p[17];
    if (!at || !quiet_read(at - 0x40, p, sizeof p) || !fov_pattern(p + 16, aspect)) return 0;
    const double fov = 2 * std::atan(static_cast<double>(p[16])) / kRad;
    return fov::is_valid(fov) ? fov : 0;
}

class DecimaEngine final : public Engine {
public:
    const char* name() const override { return "Decima"; }
    bool detect(const std::wstring& exe_path) const override { return decima_layout(exe_path); }
    // Nixxes's crash reporter next to the exe (crs-handler.exe, crs-video.exe) isn't the game: the folder layout detects
    // it too, and it would take the session and truncate the game's log.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring exe = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        return _wcsnicmp(exe.c_str(), L"crs-", 4) == 0;
    }
    void configure() override { load_zoom_reference(); }
    void settings_changed() override { load_zoom_reference(); }
    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        if (!searched_) {
            searched_ = true;
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            hzd_ = lower(path).find(L"horizonzerodawn") != std::wstring::npos;
            nixxes_ = !hzd_ && nixxes_layout(exe_dir(path));
            // Nixxes ports: Death Stranding 2's DSCameraMode when the exe has that class and its angle code, else
            // Horizon Forbidden West's camera component.
            const uintptr_t ds_vtable = nixxes_ ? find_vtable(kDsModeClass) : 0;
            ds2_ = ds_vtable && find_ds_mode_angles(comp_angles_);
            if (ds2_) {
                g_hzd_search.cls = kDsModeClass;
                g_hzd_search.vtable = ds_vtable;
                g_hzd_search.vtable_looked = true;
                log("decima: %s's vtable %s", kDsModeClass, mem::module_offset(ds_vtable).c_str());
            }
            found_ = hzd_ ? find_hzd_layout(hzd_layout_)
                     : ds2_ ? true
                     : nixxes_ ? find_comp_angles(comp_angles_)
                               : find_layout(layout_);
        }
        if (hzd_) return read_hzd_frame();
        if (ds2_) return read_ds2_frame();
        if (nixxes_) return read_comp_frame();
        mode_ = found_ ? camera_mode(layout_) : 0;
        float pitch = 0, heading = 0;
        if (mode_ && (!mem::rd(mode_ + layout_.pitch, pitch) || !mem::rd(mode_ + layout_.heading, heading) ||
                      !std::isfinite(pitch) || !std::isfinite(heading) || std::fabs(pitch) > 3.2f))
            mode_ = 0;
        if (mode_ != logged_mode_) {
            logged_mode_ = mode_;
            if (mode_) log("decima: camera mode %s (pitch %.1f, heading %.1f degrees)", mem::module_offset(mode_).c_str(),
                           pitch / kRad, heading / kRad);
            else log("decima: no camera mode");
        }
        if (!mode_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = "camera mode";
        f.zoom_fov = zoom_fov(render_fov(mode_));
        return f;
    }

    // Horizon Zero Dawn: the camera component's horizontal FOV in degrees at +0xE8 (recorded live: 100 unzoomed, 85.7 with
    // the bow drawn, down to 35.9 with Concentration; at 16:9 those are the render view's 67.7, 55.1 and 20.5 vertical).
    double hzd_fov(double aspect) const {
        float h = 0;
        if (!comp_ || !qrd(comp_ + kHzdComponentFov, h) || !std::isfinite(h) || h <= 1 || h >= 179 || aspect <= 0) return 0;
        const double v = fov::vertical_from_horizontal(h, aspect);
        return fov::is_valid(v) ? v : 0;
    }

    // The render view's vertical FOV, found from `from` (the camera mode) and kept while its pattern holds; 0 if not.
    double render_fov(uintptr_t from) {
        const double aspect = fov::game_window_aspect();
        double fov = vertical_fov(fov_at_, aspect);
        if (!fov && (from != fov_mode_ || GetTickCount64() >= next_fov_search_)) {
            const ULONGLONG t0 = GetTickCount64();
            fov_mode_ = from;
            fov_at_ = find_fov(from, aspect);
            next_fov_search_ = GetTickCount64() + 2000;
            fov = vertical_fov(fov_at_, aspect);
            if (fov_searches_ < 20) {
                ++fov_searches_;
                log("decima: FOV %s (%.1f degrees vertical, window aspect %.3f, search %llu ms)",
                    fov ? ("at " + mem::module_offset(fov_at_)).c_str() : "not found", fov, aspect, GetTickCount64() - t0);
            }
        }
        return fov;
    }

    // Degrees from GyroLib (pitch up, yaw right) added to the mode's radians: pitch up is +, heading right is - (tested).
    bool apply(double pitch, double yaw) override {
        if (hzd_) return apply_hzd(pitch, yaw);
        if (nixxes_) return apply_comp(pitch, yaw);  // Death Stranding 2 too: the same angles, in its DSCameraMode
        if (!mode_) return false;
        float p = 0, h = 0;
        if (!mem::rd(mode_ + layout_.pitch, p) || !mem::rd(mode_ + layout_.heading, h)) return false;
        p = static_cast<float>(p + pitch * kRad);
        h = static_cast<float>(h - yaw * kRad);
        return mem::guarded_store(mode_ + layout_.pitch, &p, sizeof p) &&
               mem::guarded_store(mode_ + layout_.heading, &h, sizeof h);
    }

    bool sees_menus() const override { return false; }
    std::string status_scene() const override {
        if (!found_) return searched_ ? "view=none (camera code not found)" : "view=none";
        return mode_ ? "view=default (camera mode " + mem::module_offset(mode_) + ")" : "view=none (no camera mode)";
    }

private:
    // Horizon Forbidden West: the one ThirdPersonPlayerCameraComponent, found by its vtable on a background thread (the
    // same search as Horizon Zero Dawn's fallback), kept while its class still matches; its pitch and heading radians.
    bool comp_angles(uintptr_t comp, float& pitch, float& heading) const {
        return qrd(comp + comp_angles_.pitch, pitch) && qrd(comp + comp_angles_.heading, heading) &&
               std::isfinite(pitch) && std::isfinite(heading) && std::fabs(pitch) < 1.6f && std::fabs(heading) < 1000.f;
    }

    EngineFrame read_comp_frame() {
        EngineFrame f;
        if (comp_ && !is_class(comp_, kCompClass)) comp_ = 0;
        if (!comp_ && found_) {
            if (const uintptr_t r = g_hzd_search.result.exchange(0); r && is_class(r, kCompClass)) comp_ = r;
            else g_hzd_search.start();
        }
        float pitch = 0, heading = 0;
        mode_ = comp_ && comp_angles(comp_, pitch, heading) ? comp_ : 0;
        if (mode_ != logged_mode_) {
            logged_mode_ = mode_;
            if (mode_)
                log("decima: camera component %s (pitch %.1f, heading %.1f degrees)", mem::module_offset(mode_).c_str(),
                    pitch / kRad, heading / kRad);
            else log("decima: no camera component");
        }
        // A new camera mode (L2 aims) blends from the old one for about two seconds, during which the view barely follows
        // the angles (recorded live, MODLOG 2026-11-18); a right-stick touch ends the blend (user test), so the first
        // gyro turn in that window gives one (camera_waits_for_stick). The mode: [[component+0x248]], what the script
        // export ThirdPersonPlayerCameraComponent_ExportedGetCurrCameraMode reads (its resource at +0x20).
        uintptr_t cam_mode = 0;
        if (mode_) {
            const uintptr_t ref = qrdp(mode_ + 0x248);
            cam_mode = ref ? qrdp(ref) : 0;
        }
        if (cam_mode && last_mode_ && cam_mode != last_mode_) {
            hold_until_ = GetTickCount64() + kHzdHoldMs;
            if (++holds_ == 0) holds_ = 1;
        }
        if (cam_mode) last_mode_ = cam_mode;
        if (!mode_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = "camera component";
        // The component's FOV in degrees at +0xE0 (recorded live, MODLOG 2026-11-18: 70 unzoomed, 60 with L2, 25 with
        // Concentration; +0xD8 keeps the one before the last change). Taken as vertical: 70 is close to Horizon Zero
        // Dawn's 67.7 unzoomed render FOV; zoom compensation's tangent ratio is the same either way.
        float v = 0;
        if (qrd(mode_ + kCompFov, v) && std::isfinite(v) && fov::is_valid(v)) f.zoom_fov = zoom_fov(v);
        return f;
    }

    // Death Stranding 2: the one DSCameraMode, found by its vtable on a background thread and kept while its class still
    // matches; its pitch and heading radians (comp_angles_, from kDsModeAngles), turned by apply_comp. FOV as Death
    // Stranding (DC)'s: the render projection found from the mode (recorded: 45.7 vertical, 73.7 horizontal, on foot).
    EngineFrame read_ds2_frame() {
        EngineFrame f;
        if (comp_ && !is_class(comp_, kDsModeClass)) comp_ = 0;
        if (!comp_) {
            if (const uintptr_t r = g_hzd_search.result.exchange(0); r && is_class(r, kDsModeClass)) comp_ = r;
            else g_hzd_search.start();
        }
        float pitch = 0, heading = 0;
        mode_ = comp_ && comp_angles(comp_, pitch, heading) ? comp_ : 0;
        if (mode_ != logged_mode_) {
            logged_mode_ = mode_;
            if (mode_)
                log("decima: DSCameraMode %s (pitch %.1f, heading %.1f degrees)", mem::module_offset(mode_).c_str(),
                    pitch / kRad, heading / kRad);
            else log("decima: no DSCameraMode");
        }
        if (!mode_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = "camera mode";
        f.zoom_fov = zoom_fov(render_fov(mode_));
        return f;
    }

    // GyroLib's degrees (pitch up, yaw right) added to the component's radians: pitch up is +, heading right is -
    // (write-tested: +20 degrees of heading turned the view left), pitch kept within kHzdPitchLimit.
    bool apply_comp(double pitch, double yaw) {
        float p = 0, h = 0;
        if (!mode_ || !comp_angles(mode_, p, h)) return false;
        const double lim = kHzdPitchLimit * kRad;
        p = static_cast<float>(std::clamp(p + pitch * kRad, -lim, lim));
        h = static_cast<float>(h - yaw * kRad);
        return quiet_write(mode_ + comp_angles_.pitch, &p, sizeof p) &&
               quiet_write(mode_ + comp_angles_.heading, &h, sizeof h);
    }

    EngineFrame read_hzd_frame() {
        EngineFrame f;
        std::string why = found_ ? "" : "no local player getter";
        mode_ = found_ ? hzd_camera_mode(hzd_layout_, &why, &comp_) : 0;
        float m[9];
        if (mode_ && !hzd_matrix(mode_, m)) {
            mode_ = 0;
            why = "the camera matrix isn't a rotation";
        }
        if (!mode_ && why != logged_why_) {  // each new reason once: which link of the path is missing
            logged_why_ = why;
            log("decima: no orbit camera: %s", why.c_str());
        }
        if (mode_) logged_why_.clear();
        if (mode_ != logged_mode_) {
            logged_mode_ = mode_;
            if (mode_)
                log("decima: orbit camera %s (heading %.1f, pitch %.1f degrees)", mem::module_offset(mode_).c_str(),
                    std::atan2(m[4], m[3]) / kRad, std::asin(std::clamp(m[5], -1.f, 1.f)) / kRad);
            else log("decima: no orbit camera");
        }
        if (mode_ && last_mode_ && mode_ != last_mode_) {
            left_mode_ = last_mode_;
            left_until_ = GetTickCount64() + kHzdBlendMs;
            hold_until_ = GetTickCount64() + kHzdHoldMs;
            if (++holds_ == 0) holds_ = 1;
        }
        if (mode_) last_mode_ = mode_;
        if (!mode_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = "orbit camera";
        f.zoom_fov = zoom_fov(hzd_fov(fov::game_window_aspect()));
        return f;
    }

    uint32_t camera_waits_for_stick() const override {
        return (hzd_ || nixxes_) && mode_ && GetTickCount64() < hold_until_ ? holds_ : 0;
    }

    // GyroLib's degrees (pitch up, yaw right): the rows turned about Z (yaw), forward and up about the right row (pitch),
    // forward kept within kHzdPitchLimit of level.
    //
    // Aiming (L2) and other changes swap the component's orbit mode for another one and the game blends from the mode it
    // left for about a second: turning only the new mode, the gyro seemed to stop during the blend. The mode left is
    // turned too until the blend is over (as 007 First Light's and Hitman's transition cameras).
    bool apply_hzd(double pitch, double yaw) {
        if (!mode_) return false;
        if (left_mode_ && left_mode_ != mode_ && GetTickCount64() < left_until_ &&
            is_class(left_mode_, ".?AVCameraOrbitFollow@@"))
            turn_hzd_mode(left_mode_, pitch, yaw);
        if (comp_ && GetTickCount64() < hold_until_) turn_hzd_matrix(comp_ + kHzdComponentMatrix, pitch, yaw);
        return turn_hzd_mode(mode_, pitch, yaw);
    }

    bool turn_hzd_mode(uintptr_t mode, double pitch, double yaw) { return turn_hzd_matrix(mode + kHzdMatrix, pitch, yaw); }

    // The rotation matrix at `at` (rows right, forward, up) turned: about Z by yaw, forward and up about the right row by
    // pitch, forward kept within kHzdPitchLimit of level.
    static bool turn_hzd_matrix(uintptr_t at, double pitch, double yaw) {
        float m[9];
        if (!hzd_matrix(at - kHzdMatrix, m)) return false;
        const double cy = std::cos(-yaw * kRad), sy = std::sin(-yaw * kRad);
        double r[9];
        for (int i = 0; i < 3; ++i) {
            const double x = m[3 * i], y = m[3 * i + 1];
            r[3 * i] = x * cy - y * sy;
            r[3 * i + 1] = x * sy + y * cy;
            r[3 * i + 2] = m[3 * i + 2];
        }
        const double now = std::asin(std::clamp(r[5], -1.0, 1.0)) / kRad;
        const double to = std::clamp(now + pitch, -kHzdPitchLimit, kHzdPitchLimit);
        const double cp = std::cos((to - now) * kRad), sp = std::sin((to - now) * kRad);
        for (int k = 0; k < 3; ++k) {
            const double fw = r[3 + k], up = r[6 + k];
            r[3 + k] = fw * cp + up * sp;
            r[6 + k] = up * cp - fw * sp;
        }
        float f[9];
        for (int i = 0; i < 9; ++i) f[i] = static_cast<float>(r[i]);
        return quiet_write(at, f, sizeof f);
    }

    // [Gyro4All] ZoomReferenceFOV= (horizontal degrees); 0 learns the unzoomed FOV: the widest one seen.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_fov_)
            log("decima: zoom reference FOV %s", fov > 0 ? (std::to_string(int(fov)) + " degrees horizontal").c_str()
                                                         : "learnt in game (the widest FOV seen)");
        zoom_reference_fov_ = fov;
    }

    std::optional<ZoomFov> zoom_fov(double fov) {
        if (!fov::is_valid(fov)) return std::nullopt;
        if (fov > widest_ + 0.05) {
            if (fov > logged_widest_ + 1) {
                logged_widest_ = fov;
                log("decima: widest FOV seen %.1f degrees vertical", fov);
            }
            widest_ = fov;
        }
        ZoomFov z;
        z.current_vertical = fov;
        z.reference_vertical = zoom_reference_fov_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_fov_, fov::game_window_aspect())
                                   : widest_;
        if (!fov::is_valid(z.reference_vertical)) return std::nullopt;
        if (std::fabs(fov - logged_fov_) > 1 && fov_logs_ < 50) {
            ++fov_logs_;
            logged_fov_ = fov;
            log("decima: FOV %.1f degrees vertical (unzoomed %.1f)", z.current_vertical, z.reference_vertical);
        }
        return z;
    }

    double zoom_reference_fov_ = -1, widest_ = 0, logged_fov_ = 0, logged_widest_ = 0;
    int fov_logs_ = 0, fov_searches_ = 0;
    uintptr_t fov_at_ = 0, fov_mode_ = 0;
    ULONGLONG next_fov_search_ = 0;
    Layout layout_;
    HzdLayout hzd_layout_;
    std::string logged_why_;
    CompAngles comp_angles_;
    bool searched_ = false, found_ = false, hzd_ = false, nixxes_ = false, ds2_ = false;
    uintptr_t mode_ = 0, logged_mode_ = 0;
    // HZD: the orbit mode the camera left and until when it is still turned with the current one (see apply_hzd).
    uintptr_t left_mode_ = 0, last_mode_ = 0;
    ULONGLONG left_until_ = 0, hold_until_ = 0;
    uintptr_t comp_ = 0;
    uint32_t holds_ = 0;
};

}  // namespace

Engine& engine() {
    static DecimaEngine e;
    return e;
}

}  // namespace gyro4all::decima
