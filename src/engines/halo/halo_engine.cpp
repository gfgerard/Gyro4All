// Halo: The Master Chief Collection (Xbox app: MCCWinStore-Win64-Shipping.exe; Steam: MCC-Win64-Shipping.exe, in
// mcc\binaries\win64). The exe is an Unreal Engine 4 shell (menus, rendering, input); each game is a native Blam! engine
// in its own DLL (halo1\halo1.dll ... halo4\halo4.dll), loaded into the same process when picked from the menu and
// unloaded when leaving it. The shell's PlayerController keeps a DefaultPawn while a game runs, so the Unreal module
// would take every game for a main menu and keep the gyro off (MODLOG 2026-11-03). Detected before Unreal by the game
// folders above the exe's.
//
// The view: every game keeps the local players' desired yaw and pitch (radians, Z up: yaw grows to the left, pitch up)
// in its player-control globals, an array of per-player entries that the player-control update turns by the input's
// per-tick deltas, wraps and clamps to +-1.49226 (MODLOG 2026-11-03). Halo CE and Halo 2 keep the globals' pointer in a
// global; Halo 3 and later in the DLL's thread-local block of the threads that run the game, found by looking at every
// thread's block. The update's code is found by signature whenever its DLL loads, so MCC updates that keep that code
// keep working. Written there, the gyro turns the player like the stick does: the game's pitch limit, vehicles and aim
// assist still apply. Halo 4's engine (Halo 4, Halo 2: Anniversary multiplayer) hashes its game state on checkpoints, and
// a write from the main thread racing its own thread breaks the hash (a fatal error on loading a checkpoint, MODLOG
// 2026-11-04): there the turn is queued and written at the start of the player-control update, on the game's thread (an
// inline hook on that function). A game whose code doesn't match gets the gyro as right-stick input (the core's blind
// stick fallback, through the XInput translation). Menus: the mouse cursor, and MCC's own flag for the pause menu and
// its other overlays that hold the game (the shell's, the same for every game; MODLOG 2026-11-04).
//
// Zoom compensation: the rendered FOV (horizontal radians; a zoom divides the angle itself by its magnification), found
// by signature like the view, against ZoomReferenceFOV= or else the game's own unzoomed FOV: in Halo CE MCC's FOV setting
// for the camera in use (first or third person: the game keeps both, in radians), in Halo 2 the current FOV divided by the
// camera's zoom scale (1 / magnification, kept next to it). Halo 3 keeps a vertical FOV, MCC's setting times 9/14 (in
// degrees converted with pi = 3.14), and MCC's settings in degrees in the local players' profiles; Halo 3: ODST the same.
// Halo: Reach, Halo 4 and Halo 2: Anniversary multiplayer keep 2 atan(a tan(b v)) of that FOV v, converted back with a
// and b fitted to three measurements.
#include "halo.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <format>
#include <mutex>
#include <numbers>

namespace gyro4all::halo {

namespace {

constexpr double kRad = std::numbers::pi / 180.0;
constexpr double kMaxPitch = 1.49226;  // radians: every game's own clamp in its player-control update

// One game's player control (MCC build of 2025-08, MODLOG 2026-11-03).
struct Game {
    const wchar_t* dll;
    const char* name;
    // The player-control update, from loading the globals' pointer to reading entry 0's unit (?? = any byte). With
    // `global` it starts with `mov rdi, [rip+disp]` on the global that holds the pointer; else the pointer is at
    // tls_slot in the DLL's thread-local block. The slot and the offsets below are in these bytes, so a match checks
    // them.
    const char* control;
    const char* turn;  // its `yaw += delta` on entry 0 (addss, then movss back)
    bool global;
    uint32_t tls_slot;
    uintptr_t unit, yaw;  // in entry 0 (the first local player): its unit's handle (-1: none), the desired yaw; pitch follows
    // The camera update's code that takes the address of the rendered FOV (horizontal radians), or of a struct that holds
    // it at fov_at, with a `lea r64,[rip+disp]` at fov_lea; none: no zoom compensation. zoom_scale: the camera's zoom
    // scale (1 / magnification, float) at that offset from the lea's target (0: none).
    const char* fov = nullptr;
    uint32_t fov_lea = 0, fov_at = 0, zoom_scale = 0;
    // The code that clears MCC's first- and third-person FOV settings at start: an instruction with a rip operand on
    // each, their displacements at these offsets (the operand ends the instruction). fov_third 0: a `lea r64,[rip+disp]`
    // at fov_first instead, to a struct holding both (first, then third) at fov_settings_at.
    const char* fov_settings = nullptr;
    uint32_t fov_first = 0, fov_third = 0, fov_settings_at = 0;
    float fov_settings_scale = 1;  // the settings' unit in the camera's: radians in Halo CE
    bool fov_vertical = false;     // the camera's FOV is vertical (else horizontal)
    // The camera keeps v' = 2 atan(a tan(b v)) of the vertical FOV v (Halo: Reach; measured, not exact): converted back.
    float fov_fit_a = 0, fov_fit_b = 0;
    // The game hashes its game state when it saves a checkpoint and checks the hash when it loads one, on its own thread
    // (Halo 4's engine: "game_state: failed to verify hash for in-place gamestate", a fatal error). A camera write from
    // the main thread that lands in between breaks it, so the turn is written from a hook at the start of the
    // player-control update instead, on the game's thread.
    bool write_in_update = false;
};

// MCC's shell (the exe): the function that sets one byte when any of its UI flags that hold the game is on (the pause
// menu's among them), else clears it. The byte is the `mov byte ptr [rip+disp], 0` at +0x3f (one match in the Xbox app's
// build of 2025-08).
constexpr const char* kShellPaused =
    "80 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 80 3D ?? "
    "?? ?? ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 83 3D ?? ?? ?? ?? 00 7F ?? C6 05 ?? ?? ?? ?? 00 EB ?? 33 D2 C6 05 ?? ?? "
    "?? ?? 01";
constexpr uint32_t kShellPausedAt = 0x3F;

constexpr Game kGames[] = {
    {L"halo1.dll", "Halo CE",
     "48 8B 3D ?? ?? ?? ?? 33 F6 4C 8B 0D ?? ?? ?? ?? 4C 8B 15 ?? ?? ?? ?? 0F 29 70 B8 0F 28 F1 0F 29 78 A8 44 0F 29 40 "
     "98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF 44 0F 29 98 68 FF FF FF 44 0F 29 A0 58 FF FF FF 44 0F 29 B0 48 FF FF FF "
     "44 0F 29 B8 38 FF FF FF 44 0F 28 FA 48 63 C1 48 6B D8 58 48 8B 05 ?? ?? ?? ?? 48 89 B5 A8 00 00 00 48 63 90 14 01 "
     "00 00 85 D2 74 ?? 48 8B C2 49 2B C1 49 03 C2 48 89 85 A8 00 00 00 44 8B 5C 3B 10",
     "F3 0F 58 74 3B 1C F3 0F 11 74 3B 1C", true, 0, 0x10, 0x1C,
     "4C 8B C2 0F 29 7C 24 30 48 8B FA 0F 28 FB 48 8B D1 48 8D 0D ?? ?? ?? ?? E8", 17, 0, 0,
     "44 21 35 ?? ?? ?? ?? 44 8D 43 F8 44 21 35 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 33 D2 4C 89 35 ?? ?? ?? ?? 4C 89 35 ?? "
     "?? ?? ??",
     3, 14},
    {L"halo2.dll", "Halo 2",
     "48 8B 3D ?? ?? ?? ?? 4C 89 60 E0 4C 89 68 D8 4C 89 70 D0 4C 89 78 C8 45 33 FF 0F 29 70 B8 45 8B E7 0F 29 78 A8 0F "
     "28 F1 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 A0 58 FF FF FF 44 0F 29 A8 48 FF FF FF 44 0F 29 B0 38 FF FF FF 44 0F "
     "29 B8 28 FF FF FF 48 63 C1 48 69 D8 B8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 63 90 F4 00 00 00 83 FA FF 74 ?? 85 D2 79 "
     "?? 44 8B E2 41 0F BA F4 1F 4C 03 25 ?? ?? ?? ?? EB ?? 4C 8B E2 4C 03 25 ?? ?? ?? ?? 8B 44 3B 18",
     "F3 41 0F 58 76 38 F3 0F 10 3D ?? ?? ?? ?? 44 0F 29 94 24 B0 01 00 00 44 0F 29 9C 24 A0 01 00 00 F3 41 0F 11 76 38",
     true, 0, 0x18, 0x38,
     // The camera update, over the local players' cameras (stride 0x368): player 0's FOV at +0x3c, zoom scale at +0x54.
     "48 81 EC A0 00 00 00 F3 0F 10 0D ?? ?? ?? ?? 4C 8D 35 ?? ?? ?? ?? F3 0F 59 C8 48 8D 2D ?? ?? ?? ?? 33 F6", 15, 0x3C,
     0x54},
    {L"groundhog.dll", "Halo 2: Anniversary multiplayer",
     "65 48 8B 04 25 58 00 00 00 4C 8D 05 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? 45 33 C9 F3 44 0F 10 25 ?? ?? ?? ?? 44 "
     "0F 28 F2 4C 63 F1 44 0F 28 D9 8B 0D ?? ?? ?? ?? 49 8B DE BF B8 00 00 00 48 C1 E3 09 F3 0F 11 5D 88 4C 8B 3C C8 48 "
     "8B 05 ?? ?? ?? ?? 4C 89 7C 24 78 4A 8B 3C 3F 8B 90 A8 05 00 00 8B C2 48 89 55 A0 C1 E8 1C 48 8D 55 F0 8B 8C 1F 20 "
     "01 00 00",
     "F3 0F 58 84 1F 38 01 00 00 41 0F 28 CD E8 ?? ?? ?? ?? 44 0F 2F D0 F3 0F 11 84 1F 38 01 00 00", false, 0xB8, 0x120,
     0x138,
     // Halo 4's engine: Halo 4's view and profile patterns, the FOV settings at +0xa4.
     "48 83 EC 50 48 8B D9 0F 29 74 24 40 4C 8D 2D ?? ?? ?? ?? 49 63 E8 49 8B CD 48 63 F2 45 8B F9", 12, 0x28, 0,
     "48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8", 18, 0, 0xA4,
     3.14f / 280, true, 0.6725665f, 0.6814241f, true},
    {L"halo3.dll", "Halo 3",
     "65 48 8B 04 25 58 00 00 00 4C 63 E1 49 69 DC 10 01 00 00 48 8B 34 D0 B8 C0 00 00 00 48 89 74 24 30 48 8B 3C 30 48 "
     "8B 05 ?? ?? ?? ?? 8B 88 08 04 00 00 85 C9 75 ?? 45 33 ED EB ?? 48 8B 05 ?? ?? ?? ?? 4C 8D 2C 88 F3 44 0F 10 2D ?? "
     "?? ?? ?? 48 8D 54 24 50 8B 8C 3B C0 00 00 00",
     "F3 0F 58 B4 3B D4 00 00 00 4C 8B 3D ?? ?? ?? ?? 41 83 C8 FF 4C 8B 35 ?? ?? ?? ?? B8 38 00 00 00 F3 0F 11 B4 3B D4 "
     "00 00 00",
     false, 0xC0, 0xC0, 0xD4,
     // The camera copied for rendering (its vertical FOV at +0x2c), and the local players' profiles (0xd78 bytes each)
     // with player 0's FOV settings in degrees at +0x74.
     "48 8D 0D ?? ?? ?? ?? 48 03 D3 E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 49 81 C0 "
     "A0 00 00 00 48 03 D3 0F 57 DB",
     0, 0x2C, 0,
     "48 83 EC 38 48 C7 44 24 20 FE FF FF FF 4C 8D 0D ?? ?? ?? ?? BA 78 0D 00 00 41 B8 04 00 00 00 48 8D 0D ?? ?? ?? ?? E8 "
     "?? ?? ?? ?? 90 48 83 C4 38 C3",
     31, 0, 0x74, 3.14f / 280, true},
    {L"halo3odst.dll", "Halo 3: ODST",
     "65 48 8B 04 25 58 00 00 00 4C 63 E1 BF D8 00 00 00 49 69 DC C0 02 00 00 4C 8B 2C D0 48 8B 05 ?? ?? ?? ?? 4A 8B 3C "
     "2F 8B 88 18 04 00 00 85 C9 75 ?? 48 83 64 24 30 00 EB ?? 48 8B 05 ?? ?? ?? ?? 48 8D 04 88 48 89 44 24 30 F3 44 0F "
     "10 2D ?? ?? ?? ?? 48 8D 54 24 50 8B 8C 1F C0 00 00 00",
     "F3 0F 58 B4 1F D4 00 00 00 4C 8B 3D ?? ?? ?? ?? 83 CE FF 4C 8B 35 ?? ?? ?? ?? B8 20 00 00 00 F3 0F 11 B4 1F D4 00 "
     "00 00",
     false, 0xD8, 0xC0, 0xD4,
     // Like Halo 3: the local players' render views (stride 0x2810, set up at start; player 0's vertical FOV at +0x34)
     // and profiles (0xeb0 bytes each; FOV settings in degrees at +0x78).
     "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 10 28 00 00 48 "
     "83 EF 01",
     10, 0x34, 0,
     "48 83 EC 38 48 C7 44 24 20 FE FF FF FF 4C 8D 0D ?? ?? ?? ?? BA B0 0E 00 00 41 B8 04 00 00 00 48 8D 0D ?? ?? ?? ?? E8 "
     "?? ?? ?? ?? 90 48 83 C4 38 C3",
     31, 0, 0x78, 3.14f / 280, true},
    {L"haloreach.dll", "Halo: Reach",
     "65 48 8B 04 25 58 00 00 00 48 8D 55 A0 F3 44 0F 10 3D ?? ?? ?? ?? 44 0F 28 E2 F3 44 0F 10 2D ?? ?? ?? ?? 0F 28 F1 "
     "48 63 F1 45 0F 28 D7 8B 0D ?? ?? ?? ?? BF D8 00 00 00 F3 0F 11 5C 24 50 48 8D 1C 76 4C 8B 34 C8 48 8D 0D ?? ?? ?? "
     "?? 48 8B 05 ?? ?? ?? ?? 48 C1 E3 07 4C 89 74 24 30 4A 8B 3C 37 44 8B A0 18 04 00 00 41 8B C4 48 C1 E8 1C 4C 8B 2C "
     "C1 8B 8C 1F 80 00 00 00",
     "F3 0F 58 94 1F 94 00 00 00 41 83 CB FF B8 10 00 00 00 F3 0F 11 94 1F 94 00 00 00", false, 0xD8, 0x80, 0x94,
     // The view copied for the frame (its FOV at +0x28, 2 atan(a tan(b v)) of Halo 3's vertical FOV v), and MCC's settings
     // (degrees) in the local players' profiles (set up at start), player 0's at +0x8c.
     "49 8D 97 D0 F5 FF FF 45 8B C6 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 8B 2D ?? ?? ?? ??", 10, 0x28, 0,
     "48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8", 18, 0, 0x8C,
     3.14f / 280, true, 0.6725665f, 0.6814241f},
    {L"halo4.dll", "Halo 4",
     "65 48 8B 04 25 58 00 00 00 4C 8D 05 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? 45 33 C9 F3 44 0F 10 25 ?? ?? ?? ?? 44 "
     "0F 28 F2 4C 63 F1 44 0F 28 D9 8B 0D ?? ?? ?? ?? 49 69 DE F0 01 00 00 F3 0F 11 5D 88 4C 8B 3C C8 48 8B 05 ?? ?? ?? "
     "?? BF B0 00 00 00 4C 89 7C 24 78 8B 90 A8 05 00 00 4A 8B 3C 3F 8B C2 C1 E8 1C 48 89 55 A0 48 8D 55 F0 8B 8C 1F 20 "
     "01 00 00",
     "F3 0F 58 84 1F 38 01 00 00 41 0F 28 CD E8 ?? ?? ?? ?? 44 0F 2F D0 F3 0F 11 84 1F 38 01 00 00", false, 0xB0, 0x120,
     0x138,
     // Like Reach: the view copied for the frame (FOV at +0x28) and the profiles' setup (0xcc0 bytes each), the FOV
     // settings at +0x9c.
     "48 83 EC 50 48 8B D9 0F 29 74 24 40 4C 8D 2D ?? ?? ?? ?? 49 63 E8 49 8B CD 48 63 F2 45 8B F9", 12, 0x28, 0,
     "48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8", 18, 0, 0x9C,
     3.14f / 280, true, 0.6725665f, 0.6814241f, true},
};

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

// The module's loaded headers.
bool nt_headers(HMODULE module, IMAGE_NT_HEADERS64& nt, uintptr_t& at) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos;
    if (!mem::rd(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || !mem::rd(base + dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    at = base + dos.e_lfanew;
    return true;
}

// The module's executable sections.
std::vector<mem::Range> code_sections(HMODULE module) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    std::vector<mem::Range> text;
    IMAGE_NT_HEADERS64 nt;
    uintptr_t at = 0;
    if (!nt_headers(module, nt, at)) return text;
    const uintptr_t first = at + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER s;
        if (!mem::rd(first + i * sizeof s, s)) return {};
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) text.push_back({base + s.VirtualAddress, base + s.VirtualAddress + s.Misc.VirtualSize});
    }
    return text;
}

// The DLL's thread-local storage index (its implicit TLS: the slot in every thread's ThreadLocalStoragePointer), or -1.
int64_t tls_index(HMODULE module) {
    IMAGE_NT_HEADERS64 nt;
    uintptr_t at = 0;
    if (!nt_headers(module, nt, at)) return -1;
    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    IMAGE_TLS_DIRECTORY64 tls;
    uint32_t index = 0;
    if (!dir.VirtualAddress || !mem::rd(reinterpret_cast<uintptr_t>(module) + dir.VirtualAddress, tls) || !mem::rd(tls.AddressOfIndex, index))
        return -1;
    return index;
}

// "48 8B ?? ..." as bytes and a mask (0x00 = any byte).
void parse_pattern(const char* text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        const bool any = p[0] == '?';
        bytes.push_back(any ? 0 : uint8_t(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        mask.push_back(any ? 0 : 0xFF);
        p += 2;
    }
}

// Where the pattern matches in the module's code; 0 unless exactly once.
uintptr_t find_code(HMODULE module, const char* pattern, const char* what) {
    std::vector<uint8_t> code, mask;
    parse_pattern(pattern, code, mask);
    const size_t n = code.size();
    std::vector<uint8_t> chunk(1 << 20);
    uintptr_t found = 0;
    int hits = 0;
    for (const mem::Range& r : code_sections(module)) {
        for (uintptr_t at = r.begin; at < r.end; at += chunk.size() - n) {
            const size_t len = std::min<uintptr_t>(chunk.size(), r.end - at);
            if (len < n || !mem::guarded_copy(chunk.data(), at, len)) continue;
            for (const uint8_t* p = chunk.data(); (p = static_cast<const uint8_t*>(std::memchr(p, code[0], chunk.data() + len - n + 1 - p)));
                 ++p) {
                size_t j = 1;
                while (j < n && (p[j] & mask[j]) == code[j]) ++j;
                if (j == n && at + (p - chunk.data()) != found) found = at + (p - chunk.data()), ++hits;
            }
            if (len < chunk.size()) break;
        }
    }
    if (hits != 1) {
        log("halo: %s matched %d times; not used", what, hits);
        return 0;
    }
    log("halo: %s at %s", what, mem::module_offset(found).c_str());
    return found;
}

// Undocumented but stable: NtQueryInformationThread's ThreadBasicInformation (class 0).
struct ThreadBasicInformation {
    LONG exit_status;
    void* teb;
    void* client_id[2];
    ULONG_PTR affinity_mask;
    LONG priority, base_priority;
};
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);

// The value at `slot` in the DLL's thread-local block of every thread of the process that has one set.
std::vector<uintptr_t> tls_values(uint32_t index, uint32_t slot) {
    static const auto query =
        reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    std::vector<uintptr_t> values;
    if (!query) return values;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return values;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!thread) continue;
        ThreadBasicInformation tbi{};
        if (query(thread, 0, &tbi, sizeof tbi, nullptr) >= 0 && tbi.teb) {
            // TEB.ThreadLocalStoragePointer (x64)
            const uintptr_t blocks = mem::rdp(reinterpret_cast<uintptr_t>(tbi.teb) + 0x58);
            const uintptr_t block = blocks ? mem::rdp(blocks + index * 8ull) : 0;
            const uintptr_t value = block ? mem::rdp(block + slot, 4) : 0;
            if (value && std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
        }
        CloseHandle(thread);
    }
    CloseHandle(snap);
    return values;
}

// Writes on the game's thread (Game::write_in_update): the main thread queues the turn, a hook at the start of the
// player-control update adds it to the player's angles there. The hook enters through a thunk that keeps the update's
// argument registers (some are floats) around the call.
std::mutex g_pending_mutex;
double g_pending_yaw = 0, g_pending_pitch = 0;  // radians, Halo's senses
std::atomic<uintptr_t> g_write_at{0};           // the player's desired yaw (pitch follows), 0 = none
std::atomic<DWORD> g_update_thread{0};          // the thread that last ran the update
void* g_hooked = nullptr;                       // the hooked update
uint8_t* g_thunk = nullptr;

bool valid_angles(const float* a) {
    return std::isfinite(a[0]) && std::isfinite(a[1]) && std::abs(a[0]) <= 7.0f && std::abs(a[1]) <= 1.6f;
}

void on_update() {
    g_update_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    double yaw, pitch;
    {
        std::lock_guard lock(g_pending_mutex);
        yaw = g_pending_yaw, pitch = g_pending_pitch;
        g_pending_yaw = g_pending_pitch = 0;
    }
    const uintptr_t at = g_write_at.load();
    float a[2];
    if ((yaw == 0 && pitch == 0) || !at || !mem::guarded_copy(a, at, sizeof a) || !valid_angles(a)) return;
    const float b[2] = {float(std::remainder(a[0] + yaw, 2 * std::numbers::pi)),
                        float(std::clamp(a[1] + pitch, -kMaxPitch, kMaxPitch))};
    mem::guarded_store(at, b, sizeof b);
}

// push rcx/rdx/r8/r9, save xmm0-3, call on_update, restore them, jump to the trampoline.
uint8_t* make_thunk() {
    static const uint8_t code[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68,  // push rcx, rdx, r8, r9; sub rsp, 0x68
        0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30,  // movups [rsp+0x20], xmm0; [rsp+0x30], xmm1
        0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50,  // movups [rsp+0x40], xmm2; [rsp+0x50], xmm3
        0xFF, 0x15, 0x2C, 0x00, 0x00, 0x00,                          // call [rip+0x2c] (0x24 -> 0x50)
        0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30,  // movups xmm0, [rsp+0x20]; xmm1, [rsp+0x30]
        0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50,  // movups xmm2, [rsp+0x40]; xmm3, [rsp+0x50]
        0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,  // add rsp, 0x68; pop r9, r8, rdx, rcx
        0xFF, 0x25, 0x10, 0x00, 0x00, 0x00,                          // jmp [rip+0x10] (0x48 -> 0x58)
    };
    static_assert(sizeof code == 0x48);
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    std::memcpy(t, code, sizeof code);
    void* call = reinterpret_cast<void*>(&on_update);
    std::memcpy(t + 0x50, &call, 8);
    std::memset(t + 0x58, 0, 8);  // the trampoline, set when hooked
    return t;
}

// The start of the function that holds `code` (its unwind entry, through any chained ones), or 0.
uintptr_t function_start(uintptr_t code) {
    DWORD64 base = 0;
    const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(code, &base, nullptr);
    for (int i = 0; rf && i < 32; ++i) {
        if (rf->UnwindData & 1) {  // an indirect entry
            rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + (rf->UnwindData & ~1u));
            continue;
        }
        const auto* ui = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) return uintptr_t(base + rf->BeginAddress);
        rf = reinterpret_cast<const RUNTIME_FUNCTION*>(ui + 4 + ((ui[2] + 1) & ~1) * 2);
    }
    return 0;
}

void unhook_update() {
    if (!g_hooked) return;
    const MH_STATUS status = MH_RemoveHook(g_hooked);
    log("halo: player-control update hook removed: %s", MH_StatusToString(status));
    g_hooked = nullptr;
}

// The game's DLL unloads (leaving the game in MCC's menu): unhook while its code is still mapped.
struct DllNotificationData {
    ULONG flags;
    const void* full_name;  // UNICODE_STRING
    const void* base_name;
    void* base;
    ULONG size;
};
VOID CALLBACK on_dll_notification(ULONG reason, const DllNotificationData* data, void*) {
    constexpr ULONG kUnloaded = 2;
    if (reason != kUnloaded || !data || !g_hooked) return;
    if (reinterpret_cast<uintptr_t>(g_hooked) - reinterpret_cast<uintptr_t>(data->base) < data->size) {
        g_write_at = 0;
        unhook_update();
    }
}

bool hook_update(uintptr_t target) {
    static const bool watching = [] {
        using Register = LONG(NTAPI*)(ULONG, decltype(&on_dll_notification), void*, void**);
        auto reg = reinterpret_cast<Register>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        void* cookie = nullptr;
        return reg && reg(0, &on_dll_notification, nullptr, &cookie) == 0;
    }();
    if (!watching) return false;  // it couldn't be unhooked before its DLL unloads
    if (!g_thunk && !(g_thunk = make_thunk())) return false;
    unhook_update();
    void* const at = reinterpret_cast<void*>(target);
    void** const original = reinterpret_cast<void**>(g_thunk + 0x58);
    bool ok = hooks::hook_function(at, g_thunk, original);
    if (!ok) {  // left over from a DLL that unloaded unnoticed at the same address: its bytes are back, start again
        MH_DisableHook(at);
        MH_RemoveHook(at);
        ok = hooks::hook_function(at, g_thunk, original);
    }
    if (ok) g_hooked = at;
    return ok;
}

class HaloEngine final : public Engine {
public:
    const char* name() const override { return "Halo"; }

    // MCC's content root is three folders above the exe's (mcc\binaries\win64): any of the games' DLL folders there.
    bool detect(const std::wstring& exe_path) const override {
        std::wstring root = exe_path;
        for (int i = 0; i < 4; ++i) {
            const size_t cut = root.find_last_of(L"\\/");
            if (cut == std::wstring::npos) return false;
            root.resize(cut);
        }
        for (const wchar_t* game : {L"halo1", L"halo2", L"groundhog", L"halo3", L"halo3odst", L"haloreach", L"halo4"})
            if (exists(root + L"\\" + game + L"\\" + game + L".dll")) return true;
        return false;
    }

    void configure() override { load_zoom_reference(); }
    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        const bool was = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was) log("halo: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        f.paused = shell_paused();
        find_game();
        if (!control_) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        const uintptr_t had = player_;
        player_ = find_player();
        if (player_ != had) log("halo: %s player control %s", game_->name, player_ ? "readable (in game)" : "gone (menu or loading)");
        if (hooked_) {
            g_write_at = player_ ? player_ + game_->yaw : 0;
            const DWORD thread = g_update_thread.load(std::memory_order_relaxed);
            if (thread && thread != update_thread_)
                log("halo: %s's player-control update runs on thread %lu: the gyro writes there", game_->name, thread);
            update_thread_ = thread;
        }
        if (!player_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = std::format("{} player control", game_->name);
        if (auto z = zoom_fov()) f.zoom_fov = *z;
        return f;
    }

    // Gyro senses (pitch up, yaw right, degrees). Halo's yaw grows to the left.
    // With the hook (Game::write_in_update): queued for the game's thread, which clamps and wraps it.
    bool apply(double pitch, double yaw) override {
        float a[2];
        if (!angles(a)) return false;
        if (hooked_) {
            std::lock_guard lock(g_pending_mutex);
            g_pending_yaw -= yaw * kRad;
            g_pending_pitch += pitch * kRad;
            return true;
        }
        const double p = std::clamp(a[1] + pitch * kRad, -kMaxPitch, kMaxPitch);
        return store(std::remainder(a[0] - yaw * kRad, 2 * std::numbers::pi), p);
    }

    Recenter recenter_support(std::string& detail) override {
        float a[2];
        if (!angles(a)) {
            detail = "Halo player control not readable";
            return Recenter::NotYet;
        }
        detail = std::format("pitch {:.1f}", a[1] / kRad);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float a[2];
        if (!angles(a)) return false;
        if (hooked_) {
            std::lock_guard lock(g_pending_mutex);
            g_pending_pitch -= (a[1] + g_pending_pitch) * fraction;
            return true;
        }
        return store(a[0], a[1] * (1.0 - fraction));
    }

    // Not with the hook: its writes land on the game's next tick, which the stick fallback would take for undone writes.
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float a[2];
        if (hooked_ || !angles(a)) return false;
        pitch = a[1] / kRad;
        yaw = -a[0] / kRad;
        camera = player_;
        return true;
    }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        float a[2];
        if (player_ && angles(a))
            return std::format("halo: {} player yaw {:.1f} pitch {:.1f} (deg), cursor={}", game_->name, a[0] / kRad, a[1] / kRad,
                               int(cursor_));
        if (control_) return std::format("halo: {}, no player (menu or loading), cursor={}", game_->name, int(cursor_));
        return std::format("halo: no camera hook (gyro as right stick), cursor={}", int(cursor_));
    }

private:
    // The game DLLs come and go with the game picked in MCC's menu: find the loaded one's player control when it loads.
    void find_game() {
        const Game* game = nullptr;
        HMODULE module = nullptr;
        for (const Game& g : kGames)
            if ((module = GetModuleHandleW(g.dll))) {
                game = &g;
                break;
            }
        if (module == module_) return;
        if (game_) log("halo: %s unloaded", game_->name);
        module_ = module;
        game_ = game;
        control_ = fov_ = first_ = zoom_scale_ = 0;
        third_person_ = hooked_ = false;
        g_write_at = 0;
        unhook_update();  // normally gone already, with its DLL
        update_thread_ = 0;
        tls_index_ = -1;
        globals_ = player_ = 0;
        last_scan_ = 0;
        if (!game) return;
        log("halo: %s loaded", game->name);
        const uintptr_t code = find_code(module, game->control, std::format("{}'s player-control update", game->name).c_str());
        if (code && find_code(module, game->turn, std::format("{}'s player turn", game->name).c_str())) {
            if (game->global) {
                int32_t disp = 0;
                if (mem::rd(code + 3, disp)) control_ = code + 7 + disp;
                log("halo: %s's player-control globals pointer at %s", game->name, mem::module_offset(control_).c_str());
            } else if ((tls_index_ = tls_index(module)) >= 0) {
                control_ = code;
                log("halo: %s's player-control globals in its thread-local block (index %lld, offset 0x%x)", game->name, tls_index_,
                    game->tls_slot);
            }
        }
        if (control_ && game->write_in_update) {
            // Its game state is hashed on checkpoints: write on the game's thread, at the start of the update.
            const uintptr_t start = function_start(code);
            uint8_t head[8] = {};
            mem::guarded_copy(head, start, sizeof head);
            hooked_ = start && hook_update(start);
            log("halo: %s's player-control update starts at %s (%02x %02x %02x %02x %02x %02x %02x %02x): %s", game->name,
                start ? mem::module_offset(start).c_str() : "not found", head[0], head[1], head[2], head[3], head[4], head[5],
                head[6], head[7], hooked_ ? "hooked, the gyro writes on the game's thread" : "not hooked: the gyro goes to it as right stick");
            if (!hooked_) control_ = 0;
        } else if (!control_)
            log("halo: %s's player control not found: the gyro goes to it as right stick", game->name);
        if (game->fov) {
            if (const uintptr_t lea = find_code(module, game->fov, std::format("{}'s camera FOV", game->name).c_str())) {
                int32_t disp = 0;
                if (mem::rd(lea + game->fov_lea + 3, disp)) {
                    const uintptr_t target = lea + game->fov_lea + 7 + disp;
                    fov_ = target + game->fov_at;
                    if (game->zoom_scale) zoom_scale_ = target + game->zoom_scale;
                }
                log("halo: %s's FOV at %s%s%s", game->name, mem::module_offset(fov_).c_str(),
                    zoom_scale_ ? ", zoom scale at " : "", zoom_scale_ ? mem::module_offset(zoom_scale_).c_str() : "");
            }
            if (!fov_) log("halo: %s's FOV not found: no zoom compensation", game->name);
        }
        if (fov_ && game->fov_settings) {
            if (const uintptr_t init = find_code(module, game->fov_settings, std::format("{}'s FOV settings", game->name).c_str())) {
                int32_t first = 0, third = 0;
                if (!game->fov_third) {
                    if (mem::rd(init + game->fov_first + 3, first))
                        first_ = init + game->fov_first + 7 + first + game->fov_settings_at;
                } else if (mem::rd(init + game->fov_first, first) && mem::rd(init + game->fov_third, third) &&
                           init + game->fov_third + 4 + third == init + game->fov_first + 4 + first + 4)
                    first_ = init + game->fov_first + 4 + first;
                log("halo: %s's FOV settings (first, third person) at %s", game->name,
                    first_ ? mem::module_offset(first_).c_str() : "an unexpected layout: not used");
            }
            if (!first_) log("halo: %s's FOV settings not found: zoom compensation needs ZoomReferenceFOV=", game->name);
        }
    }

    // The unzoomed FOV: [Gyro4All] ZoomReferenceFOV= (horizontal degrees, profile or Gyro4All.ini); 0 or implausible
    // uses the game's own (MCC's FOV setting, or the zoom scale). At start and on save.
    void load_zoom_reference() {
        double fov = std::atof(settings::get_string(L"ZoomReferenceFOV", L"0").c_str());
        if (fov > 0 && !fov::is_valid(fov)) {
            log("halo: ZoomReferenceFOV=%.1f implausible: ignored", fov);
            fov = 0;
        }
        if (fov != zoom_reference_fov_)
            log("halo: zoom reference FOV %.1f degrees ([Gyro4All] ZoomReferenceFOV%s)", fov,
                fov > 0 ? "" : ": 0, the game's own");
        zoom_reference_fov_ = fov;
    }

    // The current and unzoomed FOV, both vertical degrees (converted with the window's aspect), for GyroLib's zoom
    // compensation. The unzoomed one is MCC's setting for the camera in use: the third-person one while the camera is at
    // it (vehicles), else the first-person one, kept through a zoom, which narrows either. Halo 2: the current one divided
    // by the zoom scale.
    std::optional<ZoomFov> zoom_fov() {
        float radians = 0;
        if (!fov_ || !mem::rd(fov_, radians)) return std::nullopt;
        const bool fit = game_->fov_fit_a > 0;
        if (fit) radians = float(std::atan(std::tan(radians / 2.0) / game_->fov_fit_a) / game_->fov_fit_b);
        const double current = radians / kRad;
        if (!fov::is_valid(current)) return std::nullopt;
        double reference = zoom_reference_fov_;
        const bool vertical = game_->fov_vertical;
        if (reference > 0 && vertical)  // ZoomReferenceFOV= is horizontal: that of the camera's own unit
            reference = reference * game_->fov_settings_scale / kRad;
        float set[2];
        if (first_ && mem::guarded_copy(set, first_, sizeof set)) {
            const float scale = game_->fov_settings_scale;
            set[0] *= scale;
            set[1] *= scale;
            const float tolerance = fit ? 2e-3f : 1e-4f;  // the fit is within 0.1%
            auto at = [&](float v) { return std::abs(radians - v) <= tolerance * std::abs(v); };
            const bool third = at(set[1]) ? true : at(set[0]) ? false : third_person_;
            if (third != third_person_)
                log("halo: %s's camera at the %s-person FOV setting", game_->name, third ? "third" : "first");
            third_person_ = third;
            if (reference <= 0) reference = set[third ? 1 : 0] / kRad;
        }
        float scale = 0;
        if (reference <= 0 && zoom_scale_ && mem::rd(zoom_scale_, scale) && scale > 0.01f && scale <= 1.0f)
            reference = current / scale;
        if (!fov::is_valid(reference)) return std::nullopt;
        const double aspect = fov::game_window_aspect();
        const ZoomFov z = vertical ? ZoomFov{current, reference}
                                   : ZoomFov{fov::vertical_from_horizontal(current, aspect),
                                             fov::vertical_from_horizontal(reference, aspect)};
        if (!fov::is_valid(z.current_vertical) || !fov::is_valid(z.reference_vertical)) return std::nullopt;
        return z;
    }

    // Entry 0 of the player-control globals while it has a unit and plausible angles, else 0.
    uintptr_t find_player() {
        if (game_->global) return entry_player(mem::rdp(control_, 4));  // Blam allocations are only 4-byte aligned
        if (const uintptr_t p = entry_player(globals_)) return p;
        // Halo 3 and later: look through the threads' blocks again while there is no player, at most once a second.
        const ULONGLONG now = GetTickCount64();
        if (now - last_scan_ < 1000) return 0;
        last_scan_ = now;
        const std::vector<uintptr_t> values = tls_values(uint32_t(tls_index_), game_->tls_slot);
        uintptr_t found = 0;
        for (uintptr_t v : values)
            if (entry_player(v)) {
                found = v;
                break;
            }
        if (found != globals_ || values.size() != scanned_) {
            std::string list;
            for (uintptr_t v : values) list += std::format(" {:#x}{}", v, v == found ? " (in game)" : "");
            log("halo: %s's player-control globals in %zu thread(s):%s", game_->name, values.size(), list.c_str());
        }
        scanned_ = values.size();
        globals_ = found;
        return entry_player(found);
    }

    uintptr_t entry_player(uintptr_t entry) const {
        uint32_t unit = 0;
        float a[2];
        if (!entry || !mem::rd(entry + game_->unit, unit) || unit == 0xFFFFFFFF || !mem::guarded_copy(a, entry + game_->yaw, sizeof a))
            return 0;
        return valid(a) ? entry : 0;
    }

    static bool valid(const float* a) { return valid_angles(a); }

    // Desired yaw and pitch (radians) of the current player, if readable.
    bool angles(float* a) const {
        return player_ && mem::guarded_copy(a, player_ + game_->yaw, 2 * sizeof(float)) && valid(a);
    }

    bool store(double yaw, double pitch) {
        const float a[2] = {float(yaw), float(pitch)};
        return mem::guarded_store(player_ + game_->yaw, a, sizeof a);
    }

    // MCC's pause menu (or another of its overlays that holds the game) is open. Found in the exe on the first frame.
    bool shell_paused() {
        if (!pause_searched_) {
            pause_searched_ = true;
            const HMODULE exe = GetModuleHandleW(nullptr);
            if (const uintptr_t code = find_code(exe, kShellPaused, "MCC's pause flag update")) {
                int32_t disp = 0;
                if (mem::rd(code + kShellPausedAt + 2, disp)) pause_ = code + kShellPausedAt + 7 + disp;
            }
            log("halo: MCC's pause flag %s", pause_ ? ("at " + mem::module_offset(pause_)).c_str()
                                                    : "not found: only the mouse cursor stops the gyro in menus");
        }
        uint8_t v = 0;
        const bool now = pause_ && mem::rd(pause_, v) && v != 0;
        if (now != paused_) log("halo: MCC's pause menu %s", now ? "open" : "closed");
        paused_ = now;
        if (now) {  // nothing queued for the game's thread carries over the pause
            std::lock_guard lock(g_pending_mutex);
            g_pending_yaw = g_pending_pitch = 0;
        }
        return now;
    }

    bool cursor_ = false;
    bool pause_searched_ = false;
    uintptr_t pause_ = 0;       // MCC's pause flag (a byte in the exe), 0 = not found
    bool paused_ = false;
    bool hooked_ = false;       // Game::write_in_update: the update is hooked, turns are queued for it
    DWORD update_thread_ = 0;   // the update's thread, as last logged
    HMODULE module_ = nullptr;
    const Game* game_ = nullptr;
    uintptr_t control_ = 0;     // the global holding the globals' pointer (Halo CE, Halo 2), or the update's code (later games)
    int64_t tls_index_ = -1;    // later games: the DLL's thread-local storage index
    uintptr_t globals_ = 0;     // later games: the player-control globals found in a thread's block
    size_t scanned_ = 0;        // threads with a block set at the last look
    ULONGLONG last_scan_ = 0;
    uintptr_t player_ = 0;      // entry 0 while in game
    uintptr_t fov_ = 0;         // the rendered FOV (horizontal radians), Halo CE and Halo 2
    uintptr_t zoom_scale_ = 0;  // the camera's zoom scale (1 / magnification), Halo 2
    uintptr_t first_ = 0;       // MCC's first-person FOV setting, the third-person one after it (Halo CE, Halo 3)
    bool third_person_ = false; // the camera was last at the third-person setting
    double zoom_reference_fov_ = 0;  // [Gyro4All] ZoomReferenceFOV=, horizontal degrees; 0 = MCC's setting
};

}  // namespace

Engine& engine() {
    static HaloEngine instance;
    return instance;
}

}  // namespace gyro4all::halo
