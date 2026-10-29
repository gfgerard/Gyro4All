// Glacier 2 (IO Interactive): HITMAN 3 (Epic, HITMAN3.exe 3.280.0.0, MODLOG 2026-10-28). The game is detected by Glacier's
// thumbs.dat next to the exe (Retail\) and its package list in ..\Runtime\packagedefinition.txt. The exe pumps messages
// with PeekMessageA on the main thread; Gyro4All's frame runs there, from its hook (a Present-driven frame thread made no
// difference once the look-at blend was handled, and moved the F10 panel to a startup window). The game reads the
// DualSense itself over HID (Sony's libScePad.dll). 007 First Light never calls PeekMessage: its frame comes from the
// game loop's frame wrapper (loop_thread), and its camera turns through the active orbit camera's angles (glacier_look.cpp).
//
// The scene (layouts from ZHMModSDK's reconstructed headers, https://github.com/OrfeasZ/ZHMModSDK; the globals found by
// the code that refers to them, as the SDK does): the player registry (a static object) holds SNetPlayerData[4]
// (0xD0 bytes each) inline at +0x50 (its TArray at +0x390 has no end pointer in 3.280.0.0); the local player is the
// first with a Hitman entity whose controller has no net player (+0x50), its Hitman
// entity a ZEntityRef at +0xA8. A ZEntityRef points to the entity's ZEntityType* (the entity's +8); the type lists the
// entity's interfaces (+0x20: TArray of {STypeID*, offset from the ref}), each named by its IType (STypeID +0x08, or
// its source's when flags == 1; IType +0x10: the name). ZHitman5's m_rMainCamera (a TEntityRef<ZHM5MainCamera>) is at
// +0x1348. The game clock: ZGameTimeManager (a static object) m_bPaused at +0x70.
//
// The view (MODLOG 2026-10-28, found with camtrace): the third-person camera controller (vtable exe+0x1E1BA10, stored by
// its constructor at exe+0x722945, kVtableCode) holds the view's angles as floats in radians at +0x2C and +0x30, kept
// when written (the update at exe+0x724240 copies them to +0x3E4/+0x3E0, builds the camera's local quaternion from them
// and writes it to the main camera, ZHM5MainCamera +0x50; its world matrix at +0x20 follows). +0x10 points to the main
// camera's ZHM5MainCamera interface, +0x20 to the local ZHitman5. +0x2C is the yaw, +0x30 the pitch (senses:
// kYawSense/kPitchSense, checked in game). The controller is found by its vtable with
// a heap scan on a thread of its own, again whenever it goes away. Without the vtable, or while no controller matches
// the local Hitman, the gyro goes to the game as right-stick input (the core's blind stick fallback).
#include "glacier.hpp"
#include "glacier_look.hpp"
#include "glacier_xinput.hpp"

#include "dinputpad.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <vector>

namespace gyro4all::glacier {

namespace {

// Code that refers to each global (0xFF in the mask = byte must match); the disp32 at byte 3 is relative to the end of
// the instruction, 7 bytes from the pattern's start (data_from_code's defaults). Each unique in .text of HITMAN3.exe
// 3.280.0.0 (2026-10-28).
// lea rdx,[rip+registry]; mov [rsp+50h],r15 (exe+0xBD121 -> exe+0x3130720).
constexpr uint8_t kRegistryCode[] = {0x48, 0x8D, 0x15, 0, 0, 0, 0, 0x4C, 0x89, 0x7C, 0x24, 0x50};
constexpr uint8_t kRegistryMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// mov [rip+manager],rax (its vtable); mov rax,[rip+..]; xorps xmm1,xmm1 (exe+0xF0EC89 -> exe+0x315D5C0).
constexpr uint8_t kTimeCode[] = {0x48, 0x89, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x0F, 0x57, 0xC9};
constexpr uint8_t kTimeMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};

// The camera controller's constructor: xor ebp,ebp; lea rax,[rip+vtable]; mov [rsi],rax; lea rcx,[rsi+2F0h]
// (exe+0x722945 -> exe+0x1E1BA10, unique).
constexpr uint8_t kVtableCode[] = {0x33, 0xED, 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x89, 0x06, 0x48, 0x8D, 0x8E, 0xF0, 0x02, 0x00, 0x00};
constexpr uint8_t kVtableMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr size_t kVtableDisp = 5, kVtableInsnEnd = 9;

constexpr uintptr_t kPlayers = 0x50, kPlayerSize = 0xD0, kNetPlayer = 0x50, kHitmanEntity = 0xA8;
constexpr size_t kMaxPlayers = 4;
constexpr uintptr_t kTypeInterfaces = 0x20, kMainCamera = 0x1348, kPaused = 0x70;
// The controller: the main camera's interface, the local ZHitman5, the angles (radians).
constexpr uintptr_t kCtlCamera = 0x10, kCtlHitman = 0x20, kYaw = 0x2C, kPitch = 0x30;
constexpr uintptr_t kPendingYaw = 0x3E4, kPendingPitch = 0x3E0;  // the update's working copies
// The camera's look-at blend (the update's +0x2F0 struct +0x6C4, exe+0x727E05): while this byte is set, every update
// pulls the pending angles toward a fixed world point (+0x990) over +0x860 seconds (0.54) and clears it at the end. It
// starts at aim-in when the gyro turns (never seen with the stick) and holds the view still: a gyro freeze.
constexpr uintptr_t kLookAtBlend = 0x9B4;
// The update's input: [ZHitman5+0xBD0] +0x50 holds the stick's turn since the last update, float yaw, pitch in degrees
// in the angles' senses; the update (exe+0x724776) scales it by pi/180, adds it to the pending angles and clears it.
constexpr uintptr_t kHitmanInput = 0xBD0, kInputDelta = 0x50;
// While aiming, the game clears the delta without applying it (or leaves it): how a delta write is judged. A write
// still untouched after kUnconsumedFrames of the game's frames wasn't consumed; a consumed one was applied if the angles
// moved by at least half of it; turns under kJudgeMin degrees tell nothing. kMarker (degrees of yaw): the delta left in
// the angles mode, to see when the game applies it again.
constexpr uint32_t kUnconsumedFrames = 3;
constexpr float kJudgeMin = 0.005f, kMarker = 0.01f;
// The angles' senses against the gyro's (yaw right, pitch up): the yaw grows turning right, the pitch looking down
// (checked in game, DualSense USB, Steam Input off).
constexpr double kYawSense = 1, kPitchSense = -1;
// The pitch limits (radians, the game's sense: down positive): the game's live clamp at +0x8B4 (down) and +0x8B8 (up)
// (the update's +0x5C4/+0x5C8 from +0x2F0): 70 degrees down and 45 up on foot, 80 both ways aimed, moving between
// them during the raise and lower. Without plausible values there, the on-foot ones (kMaxUp, kMaxDown, just past
// -0.78540 and 1.22173 for float rounding); never past kMaxPitch.
constexpr uintptr_t kLimitDown = 0x8B4, kLimitUp = 0x8B8;
constexpr float kMaxUp = 0.7855f, kMaxDown = 1.2218f, kMaxPitch = 1.5f;
constexpr ULONGLONG kRescanMs = 5000;
constexpr double kRad = 3.14159265358979323846 / 180.0, kTurn = 2 * 3.14159265358979323846;

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

}  // namespace

// The address in the exe's section `in` a RIP-relative instruction refers to, from the code around it. 0 unless the
// pattern is found and all its matches agree; code_at: where it matched (the last match).
uintptr_t data_from_code(const uint8_t* code, const uint8_t* mask, size_t n, const char* what, size_t disp_at,
                         size_t insn_end, const char* in, uintptr_t* code_at) {
    const mem::Range text = exe_section(".text");
    const mem::Range data = exe_section(in);
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
            if (target == found && code_at) *code_at = at + i;
        }
        if (len < chunk.size()) break;
    }
    if (hits != 1) {
        log("glacier: the code that refers to %s matched %d different addresses; not used", what, hits);
        return 0;
    }
    log("glacier: %s at %s", what, mem::module_offset(found).c_str());
    return found;
}

namespace {

// A type's name from its STypeID (STypeID::GetTypeInfo, then IType's name), "" if unreadable.
std::string type_name(uintptr_t type_id) {
    uint16_t flags = 0;
    uintptr_t type = mem::rdp(type_id + 0x08);
    if (!mem::rd(type_id, flags)) return {};
    if (flags == 1 || !type) {
        const uintptr_t source = mem::rdp(type_id + 0x10);
        if (source) type = mem::rdp(source + 0x08);
    }
    const uintptr_t name = type ? mem::rdp(type + 0x10, 1) : 0;
    char text[64] = {};
    if (!name || !mem::guarded_copy(text, name, sizeof text - 1)) return {};
    text[sizeof text - 1] = 0;
    return text;
}

// The interface of this name of the entity a ZEntityRef points to (ref = its m_pObj), 0 if it has none.
uintptr_t query_interface(uintptr_t ref, const char* want) {
    const uintptr_t type = ref ? mem::rdp(ref) : 0;
    const uintptr_t list = type ? mem::rdp(type + kTypeInterfaces) : 0;
    const uintptr_t begin = list ? mem::rdp(list) : 0, end = list ? mem::rdp(list + 8) : 0;
    if (!begin || end < begin || end - begin > 0x100 * 16) return 0;
    for (uintptr_t at = begin; at < end; at += 16) {
        int64_t offset = 0;
        const uintptr_t id = mem::rdp(at);
        if (id && mem::rd(at + 8, offset) && type_name(id) == want) return ref + offset;
    }
    return 0;
}

// Every interface name of an entity, for the log.
std::string interface_names(uintptr_t ref) {
    const uintptr_t type = ref ? mem::rdp(ref) : 0;
    const uintptr_t list = type ? mem::rdp(type + kTypeInterfaces) : 0;
    const uintptr_t begin = list ? mem::rdp(list) : 0, end = list ? mem::rdp(list + 8) : 0;
    if (!begin || end < begin || end - begin > 0x100 * 16) return "?";
    std::string out;
    for (uintptr_t at = begin; at < end; at += 16) {
        int64_t offset = 0;
        const uintptr_t id = mem::rdp(at);
        if (!id || !mem::rd(at + 8, offset)) continue;
        out += std::format("{}{}@{:+#x}", out.empty() ? "" : " ", type_name(id), offset);
    }
    return out;
}

// A camera controller of this Hitman: its vtable, the Hitman at +0x20, a camera at +0x10 and finite angles.
bool valid_controller(uintptr_t c, uintptr_t vtable, uintptr_t hitman) {
    uintptr_t vt = 0, h = 0;
    float angles[2] = {};
    if (!mem::rd(c, vt) || vt != vtable || !mem::rd(c + kCtlHitman, h) || h != hitman || !mem::rdp(c + kCtlCamera) ||
        !mem::guarded_copy(angles, c + kYaw, sizeof angles))
        return false;
    return std::isfinite(angles[0]) && std::isfinite(angles[1]) && std::fabs(angles[0]) < 100 && std::fabs(angles[1]) < 100;
}

// Background heap scan for the controller: every committed private read-write region, for a qword equal to the vtable.
std::atomic<uintptr_t> g_vtable{0}, g_hitman{0}, g_found{0};
std::atomic<bool> g_scanning{false};

uintptr_t scan_region(uintptr_t begin, uintptr_t end, uintptr_t vtable, uintptr_t hitman, int& count) {
    uintptr_t first = 0;
    __try {
        for (uintptr_t a = begin; a + 8 <= end; a += 8)
            if (*reinterpret_cast<const uintptr_t*>(a) == vtable && valid_controller(a, vtable, hitman)) {
                if (!first) first = a;
                ++count;
            }
    } __except (mem::guard_filter(GetExceptionInformation())) {
    }
    return first;
}

DWORD WINAPI scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    const uintptr_t vtable = g_vtable, hitman = g_hitman;
    uint64_t bytes = 0;
    uintptr_t found = 0;
    int count = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            if (const uintptr_t c = scan_region(base, end, vtable, hitman, count); c && !found) found = c;
            bytes += mbi.RegionSize;
        }
        a = end;
    }
    log("glacier: camera controller %s (%d found; %.2f GB scanned in %llu ms)",
        found ? std::format("at {}", mem::module_offset(found)).c_str() : "not found", count,
        double(bytes) / (1024.0 * 1024.0 * 1024.0), static_cast<unsigned long long>(GetTickCount64() - start));
    g_found = found;
    g_scanning = false;
    return 0;
}

// Replaces the input delta (two floats) only if it still holds `expected`, in one locked 8-byte exchange: the game's
// update, consuming and clearing it, can't land between the read and the write. Unaligned: a
// plain store. False if the delta changed meanwhile (or isn't writable).
bool exchange_delta(uintptr_t at, const float (&expected)[2], const float (&value)[2]) {
    if (at % 8) return mem::guarded_store(at, value, sizeof value);
    LONG64 e = 0, v = 0;
    std::memcpy(&e, expected, sizeof e);
    std::memcpy(&v, value, sizeof v);
    __try {
        return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(at), v, e) == e;
    } __except (mem::guard_filter(GetExceptionInformation())) {
        return false;
    }
}

// The game loop's frame (MODLOG 2026-10-29, 007 First Light): builds whose exe never calls PeekMessage pump the window on
// a thread of their own (GetMessageW) and run the game loop on the main thread. Each frame of that loop (exe+0x49C10)
// calls the frame's work (exe+0x4A300) through an optional wrapper, a delegate in ZGameLoopManager (+0xB0), empty in
// the game: {member fn, this adjustment (low dword), static fn, object}. A static fn alone is called with the frame's
// own delegate ({0x4A300, 0, 0, the app}); the gyro frame runs there, just before the frame's work, on the main thread.
struct Delegate {
    void* member;
    int64_t adjust;
    void* fn;
    void* object;
};
// vmovups ymm2,[rip+wrapper]; mov rsi,[rsp+..]; vmovq r9,xmm2; vextractf128 xmm0,ymm2,1; vextractf128 xmm1,ymm2,1;
// vmovq r8,xmm0; vpextrq rdx,xmm1,1; test r9,r9 (exe+0x4A218 -> exe+0x5CF8330, unique).
constexpr uint8_t kWrapperCode[] = {0xC5, 0xFC, 0x10, 0x15, 0, 0, 0, 0, 0x48, 0x8B, 0xB4, 0x24, 0, 0, 0, 0,
                                    0xC4, 0xC1, 0xF9, 0x7E, 0xD1, 0xC4, 0xE3, 0x7D, 0x19, 0xD0, 0x01, 0xC4, 0xE3, 0x7D,
                                    0x19, 0xD1, 0x01, 0xC4, 0xC1, 0xF9, 0x7E, 0xC0, 0xC4, 0xE3, 0xF9, 0x16, 0xCA, 0x01,
                                    0x4D, 0x85, 0xC9};
constexpr uint8_t kWrapperMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF};
constexpr size_t kWrapperDisp = 4, kWrapperInsnEnd = 8;
constexpr uintptr_t kManagerWrapper = 0xB0;  // the wrapper's offset in ZGameLoopManager, whose vtable is at +0
// ZGameLoopManager's loop state (007 First Light, MODLOG 2026-10-29): 3 while the game runs, 2 in the pause menu (the
// orbit camera keeps updating behind it).
constexpr uintptr_t kLoopState = 0x98;
constexpr uint32_t kLoopRunning = 3;
constexpr ULONGLONG kManagerWaitMs = 300000;

Engine::FrameFn g_frame = nullptr;
std::atomic<uintptr_t> g_loop_manager{0};  // ZGameLoopManager, once built

// The game loop runs (not paused); true while ZGameLoopManager isn't known yet.
bool loop_running() {
    const uintptr_t manager = g_loop_manager.load();
    uint32_t state = 0;
    return !manager || (mem::rd(manager + kLoopState, state) && state == kLoopRunning);
}

void loop_frame(const Delegate* frame) {
    g_frame();
    using Work = void (*)(void*);
    reinterpret_cast<Work>(frame->member)(static_cast<char*>(frame->object) + static_cast<int32_t>(frame->adjust));
}

// True if the exe imports PeekMessageA or PeekMessageW (HITMAN 3): the core's pump hook gives the frame then.
bool exe_imports_peek() {
    const uintptr_t base = exe_base();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0 || !imp->OriginalFirstThunk) continue;
        for (auto* t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk); t->u1.AddressOfData; ++t) {
            if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
            const char* fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + t->u1.AddressOfData)->Name;
            if (!std::strcmp(fn, "PeekMessageW") || !std::strcmp(fn, "PeekMessageA")) return true;
        }
    }
    return false;
}

// Installs the wrapper once ZGameLoopManager is built (its vtable, in .rdata, at +0; the constructor clears the wrapper
// after it), then keeps it: put back if the game clears it, left alone if something else installs one.
DWORD WINAPI loop_thread(void*) {
    const uintptr_t wrapper = data_from_code(kWrapperCode, kWrapperMask, sizeof kWrapperCode, "the game loop's frame wrapper",
                                             kWrapperDisp, kWrapperInsnEnd);
    if (!wrapper) {
        log("glacier: no game loop frame: no gyro (this build's frame loop isn't known)");
        return 0;
    }
    look::install();
    const mem::Range rdata = exe_section(".rdata");
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const uintptr_t vtable = mem::rdp(wrapper - kManagerWrapper);
        if (vtable >= rdata.begin && vtable < rdata.end) {
            g_loop_manager = wrapper - kManagerWrapper;
            break;
        }
        if (GetTickCount64() - start > kManagerWaitMs) {
            log("glacier: ZGameLoopManager not built after %llu s: no gyro", kManagerWaitMs / 1000);
            return 0;
        }
        Sleep(10);
    }
    Sleep(100);
    auto* slot = reinterpret_cast<volatile LONG64*>(wrapper + offsetof(Delegate, fn));
    const LONG64 ours = reinterpret_cast<LONG64>(&loop_frame);
    bool installed = false, foreign = false;
    for (;;) {
        Delegate d{};
        if (!mem::guarded_copy(&d, wrapper, sizeof d)) {
            log("glacier: the game loop's frame wrapper is unreadable: no gyro");
            return 0;
        }
        if (d.member || d.object || (d.fn && d.fn != &loop_frame)) {
            if (!foreign) log("glacier: the game loop's frame wrapper is in use (%p %p): left alone, no gyro", d.member, d.fn);
            foreign = true;
            installed = false;
        } else if (!d.fn && InterlockedCompareExchange64(slot, ours, 0) == 0) {
            if (installed) log("glacier: the game loop's frame wrapper was cleared: put back");
            else log("glacier: gyro frame in the game loop's frame wrapper (%s), %llu ms after start",
                     mem::module_offset(wrapper).c_str(), static_cast<unsigned long long>(GetTickCount64() - start));
            installed = true;
            foreign = false;
        }
        Sleep(1000);
    }
}

class GlacierEngine final : public Engine {
public:
    const char* name() const override { return "Glacier"; }

    // Builds that pump with PeekMessage (HITMAN 3) get the core's frame; the others the game loop's (loop_thread).
    bool frame_source(FrameFn frame) override {
        if (!loop_mode_) return false;
        g_frame = frame;
        if (HANDLE thread = CreateThread(nullptr, 0, &loop_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            log("glacier: the exe never calls PeekMessage: the gyro frame runs in the game loop");
            return true;
        }
        log("glacier: can't start the game loop thread (error %lu)", GetLastError());
        return false;
    }

    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\thumbs.dat") && exists(dir + L"\\..\\Runtime\\packagedefinition.txt");
    }


    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // Test switches (MODLOG 2026-10-28): GlacierInputDelta=1 turns through the input delta (flaps while aimed), and
    // GlacierPendingWrites=1 writes the angles' pending copies too (crashed the game twice, GPU hang, with the Present frame
    // since removed).
    // Builds without PeekMessage (007 First Light) run the gyro frame in the game loop and turn the active orbit camera's
    // angles (glacier_look.cpp).
    void configure() override {
        loop_mode_ = !exe_imports_peek();
        input_delta_ = settings::get(L"GlacierInputDelta", 0) != 0;
        pending_writes_ = settings::get(L"GlacierPendingWrites", 0) != 0;
        log("settings: GlacierInputDelta=%d GlacierPendingWrites=%d", int(input_delta_), int(pending_writes_));
        dinputpad::on_create(&hook_xinput_device_check);
    }

    // The default view while a local Hitman exists (the game's pause and the mouse cursor gate it); nothing otherwise
    // (front end, loading). Its camera controller turned directly, the gyro as right stick until it is found.
    EngineFrame read_frame() override {
        EngineFrame f;
        const bool was_cursor = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was_cursor) log("glacier: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        if (loop_mode_) return read_look_frame(f);
        if (!searched_) find_globals();
        f.paused = paused();
        const uintptr_t hitman = local_hitman();
        if (hitman != hitman_) {
            hitman_ = hitman;
            camera_ = hitman ? mem::rdp(hitman + kMainCamera) : 0;
            if (hitman) {
                log("glacier: local Hitman (ZHitman5) at %s, main camera ref %s", mem::module_offset(hitman).c_str(),
                    camera_ ? mem::module_offset(camera_).c_str() : "none");
                if (camera_) log("glacier: main camera interfaces: %s", interface_names(camera_).c_str());
            } else {
                log("glacier: no local Hitman (front end or loading)");
            }
        }
        if (!hitman_) {
            controller_ = 0;
            return f;
        }
        find_controller();
        if (controller_ != turned_) {
            turned_ = controller_;
            write_ = {};
            angles_mode_ = false;
        }
        f.in_game = true;
        f.view_id = 1;
        f.no_camera = !controller_;
        f.target = controller_ ? "camera controller" : "ZHitman5 (gyro as right stick)";
        return f;
    }

    // Trims the pitch to the game's limits, so the gyro stops there instead of pushing writes the game clamps back.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        float p = 0, up = 0, down = 0;
        if (!controller_ || !mem::rd(controller_ + kPitch, p)) return;
        pitch_limits(up, down);
        const double target = std::clamp(p + kPitchSense * pitch * kRad, double(std::min(up, p)), double(std::max(down, p)));
        pitch = (target - p) / (kPitchSense * kRad);
    }

    // Degrees, the gyro's senses (pitch up, yaw right).
    // GlacierInputDelta=1 (off by default: the game's own aimed turning makes the judging flap): the turn goes into the input delta the update consumes, as the game's stick does; the
    // game applies it in its own update, in step with its frames, and clamps the pitch itself. While aiming (full zoom)
    // the update clears the delta without applying it, or leaves it (MODLOG 2026-10-28): each write is judged on the next
    // frame by how far the angles moved, and while the game drops it the turn goes to the angles directly, with a marker
    // in the delta that shows when the game applies it again. A dropped or unconsumed turn is turned again at the switch.
    // GlacierInputDelta=0 (default): the angles only. Angle writes can land in the middle of the game's update and be lost to its
    // angle = pending; GlacierPendingWrites=1 writes the pending copies too.
    bool apply(double pitch, double yaw) override {
        if (loop_mode_) {
            return look::live() && loop_running() && look::turn(pitch, yaw);
        }
        if (!controller_) return false;
        ++frames_;
        double ty = kYawSense * yaw, tp = kPitchSense * pitch;  // degrees, the game's senses
        // The gyro turning ends the game's look-at blend, as the game does when it runs out.
        if (uint8_t blend = 0; (ty != 0 || tp != 0) && mem::rd(controller_ + kLookAtBlend, blend) && blend) {
            constexpr uint8_t kOff = 0;
            if (mem::guarded_store(controller_ + kLookAtBlend, &kOff, sizeof kOff) && blends_stopped_++ < 20)
                log("glacier: the game's look-at blend stopped for the gyro (%u so far)", blends_stopped_);
        }
        float d[2] = {}, a[2] = {};
        const uintptr_t input = input_delta_ && hitman_ ? mem::rdp(hitman_ + kHitmanInput) : 0;
        const uintptr_t at = input + kInputDelta;
        // The delta first, then the angles: the update clears the delta before it stores the angles.
        if ((input && !read_floats(at, d)) || !read_floats(controller_ + kYaw, a)) return false;
        if (!input) return write_angles(a, ty, tp, write_.angles);
        if (write_.on) judge(at, d, a, ty, tp);
        if (angles_mode_) {
            if (!write_angles(a, ty, tp, write_.angles)) return false;
            // The marker only into an empty delta (never over the game's own input); its sign alternates, so markers the game
            // applies before a check don't add up.
            if (!write_.on && d[0] == 0 && d[1] == 0) {
                const float marker[2] = {marker_sign_ * kMarker, 0};
                if (exchange_delta(at, d, marker)) {
                    write_.on = true;
                    std::memcpy(write_.delta, marker, sizeof marker);
                    write_.frames = game_frames();
                    marker_sign_ = -marker_sign_;
                }
            }
            return true;
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            const float value[2] = {float(d[0] + ty), float(d[1] + tp)};
            if (exchange_delta(at, d, value)) {
                write_.on = value[0] != 0 || value[1] != 0;
                std::memcpy(write_.delta, value, sizeof value);
                std::memcpy(write_.angles, a, sizeof a);
                write_.frames = game_frames();
                return true;
            }
            // The game consumed the delta meanwhile: again from what it left.
            if (!read_floats(at, d) || !read_floats(controller_ + kYaw, a)) return false;
        }
        return false;
    }

    Recenter recenter_support(std::string& detail) override {
        if (loop_mode_) {
            detail = "the orbit camera (no leveling)";
            return Recenter::No;
        }
        float p = 0;
        if (!controller_ || !mem::rd(controller_ + kPitch, p)) {
            detail = "camera controller not found";
            return Recenter::NotYet;
        }
        detail = std::format("pitch {:.1f}", kPitchSense * p / kRad);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float p = 0;
        if (!controller_ || !mem::rd(controller_ + kPitch, p)) return false;
        p = float(p * (1.0 - fraction));
        return mem::guarded_store(controller_ + kPitch, &p, sizeof p);
    }

    // No view_angles: no stick fallback while the controller is found. While aiming (L2, aim assist off), the game takes
    // part of the gyro's writes back, and the fallback took that for undone writes and gave the gyro to the right stick
    // (MODLOG 2026-10-28); without it, the gyro keeps writing the angles directly.

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (loop_mode_) return std::format("{} cursor={}", look::status(), int(cursor_));
        float a[2] = {}, up = 0, down = 0;
        if (controller_) mem::guarded_copy(a, controller_ + kYaw, sizeof a);
        pitch_limits(up, down);
        return std::format("glacier: hitman={} camera={} controller={} yaw={:.3f} pitch={:.3f} (limits {:.3f}..{:.3f}, rad) "
                           "turning {} ({} switches) paused={} cursor={}",
                           hitman_ ? mem::module_offset(hitman_) : "-", camera_ ? mem::module_offset(camera_) : "-",
                           controller_ ? mem::module_offset(controller_) : "-", a[0], a[1], up, down,
                           !input_delta_ || angles_mode_ ? "angles" : "input delta", switches_, int(paused_), int(cursor_));
    }

private:
    // 007 First Light: the default view while the orbit camera's update runs (not in the front end or a load) and the
    // game loop isn't paused, turned through the active orbit camera's angles; the mouse cursor gates it too.
    EngineFrame read_look_frame(EngineFrame f) {
        const bool live = look::live() && loop_running();
        if (live != look_live_) {
            look_live_ = live;
            log("glacier: orbit camera %s", live ? "running" : "stopped (front end, load or pause)");
        }
        f.in_game = live;
        f.view_id = live ? 1 : 0;
        f.target = "orbit camera";
        return f;
    }

    static bool read_floats(uintptr_t at, float (&v)[2]) {
        return mem::guarded_copy(v, at, sizeof v) && std::isfinite(v[0]) && std::isfinite(v[1]);
    }

    // The game's live pitch clamp (radians, up negative), else the on-foot one.
    void pitch_limits(float& up, float& down) const {
        float l[2] = {};  // down, up
        if (controller_ && read_floats(controller_ + kLimitDown, l) && l[0] >= 0 && l[0] <= kMaxPitch && l[1] <= 0 &&
            l[1] >= -kMaxPitch) {
            down = l[0];
            up = l[1];
            return;
        }
        up = -kMaxUp;
        down = kMaxDown;
    }

    // The game's frames: Gyro4All's own, one per pump of the game thread.
    uint32_t game_frames() const { return frames_; }

    // Turns the angles from a (and, with GlacierPendingWrites=1, their pending copies) by ty, tp (degrees, the game's
    // senses); out: the angles as written. The yaw kept in [0, 2*pi) as the game does; the pitch within the game's
    // limits, or no further out where it is already past them (the limits move during the raise and lower).
    bool write_angles(const float (&a)[2], double ty, double tp, float (&out)[2]) {
        float up = 0, down = 0;
        pitch_limits(up, down);
        const auto turn_yaw = [&](float v) {
            const double y = std::fmod(v + ty * kRad, kTurn);
            return float(y < 0 ? y + kTurn : y);
        };
        const auto turn_pitch = [&](float v) { return std::clamp(float(v + tp * kRad), std::min(up, v), std::max(down, v)); };
        out[0] = turn_yaw(a[0]);
        out[1] = turn_pitch(a[1]);
        bool ok = mem::guarded_store(controller_ + kYaw, out, sizeof out);
        if (float v = 0; pending_writes_) {
            if (mem::rd(controller_ + kPendingYaw, v)) v = turn_yaw(v), ok &= mem::guarded_store(controller_ + kPendingYaw, &v, sizeof v);
            if (mem::rd(controller_ + kPendingPitch, v))
                v = turn_pitch(v), ok &= mem::guarded_store(controller_ + kPendingPitch, &v, sizeof v);
        }
        return ok;
    }

    // What the game made of the last write into the delta (write_), from the delta d and the angles a now: switches the
    // mode and adds what it dropped, or left unconsumed, to this frame's turn (ty, tp).
    void judge(uintptr_t at, float (&d)[2], float (&a)[2], double& ty, double& tp) {
        if (std::memcmp(d, write_.delta, sizeof d) == 0) {
            // Not consumed yet: no update since (a frame of the game's own, or the raise/lower block). After
            // kUnconsumedFrames of its frames, the turn comes back out of the delta (the marker stays where it is).
            if (angles_mode_ || game_frames() - write_.frames < kUnconsumedFrames) return;
            const float zero[2] = {};
            if (!exchange_delta(at, d, zero)) return;  // consumed just now: judged next frame
            ty += d[0];
            tp += d[1];
            d[0] = d[1] = 0;
            write_.on = false;
            set_angles_mode(true, "doesn't consume");
            return;
        }
        write_.on = false;
        int moved = moved_by(a);
        if (moved == 0 && !angles_mode_) {
            // The update may be between its clear and its store: once more, a millisecond later.
            Sleep(1);
            if (!read_floats(controller_ + kYaw, a)) return;
            moved = moved_by(a);
        }
        if (moved == 0 && !angles_mode_) {
            ty += write_.delta[0];
            tp += write_.delta[1];
            set_angles_mode(true, "drops");
        } else if (moved == 1 && angles_mode_) {
            set_angles_mode(false, "applies");
        }
    }

    // Did the angles a move from write_.angles by the delta written (write_.delta)? 1 by at least half of it, 0 not, -1
    // can't tell (a turn too small, or a pitch the game clamps at its limit).
    int moved_by(const float (&a)[2]) const {
        double expected = 0, moved = 0;
        if (std::fabs(write_.delta[0]) >= kJudgeMin) {
            expected = write_.delta[0];
            moved = std::remainder(double(a[0]) - write_.angles[0], kTurn) / kRad;
        } else if (std::fabs(write_.delta[1]) >= kJudgeMin) {
            float up = 0, down = 0;
            pitch_limits(up, down);
            const double target = write_.angles[1] + write_.delta[1] * kRad;
            if (target <= up || target >= down) return -1;
            expected = write_.delta[1];
            moved = (double(a[1]) - write_.angles[1]) / kRad;
        } else {
            return -1;
        }
        return moved / expected >= 0.5 ? 1 : 0;
    }

    void set_angles_mode(bool on, const char* why) {
        if (on == angles_mode_) return;
        angles_mode_ = on;
        ++switches_;
        log("glacier: the game %s the input delta: the gyro turns %s", why, on ? "the angles (aiming)" : "through the input delta");
    }

    // Once, on the first frame.
    void find_globals() {
        searched_ = true;
        registry_ = data_from_code(kRegistryCode, kRegistryMask, sizeof kRegistryCode, "the player registry");
        time_ = data_from_code(kTimeCode, kTimeMask, sizeof kTimeCode, "the game time manager");
        if (!time_) log("glacier: no pause detection");
        g_vtable = data_from_code(kVtableCode, kVtableMask, sizeof kVtableCode, "the camera controller's vtable", kVtableDisp,
                                  kVtableInsnEnd, ".rdata");
        if (!g_vtable) log("glacier: no camera controller: gyro as right stick");
    }

    // Keeps the controller while it stays valid for this Hitman; else picks up a finished scan or starts one (every
    // kRescanMs at most).
    void find_controller() {
        if (!g_vtable) return;
        if (controller_ && valid_controller(controller_, g_vtable, hitman_)) return;
        if (controller_) {
            log("glacier: camera controller %s gone", mem::module_offset(controller_).c_str());
            controller_ = 0;
        }
        if (g_scanning) return;
        if (const uintptr_t c = g_found.exchange(0); c && valid_controller(c, g_vtable, hitman_)) {
            controller_ = c;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < next_scan_) return;
        next_scan_ = now + kRescanMs;
        g_hitman = hitman_;
        g_scanning = true;
        if (HANDLE thread = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr)) CloseHandle(thread);
        else g_scanning = false;
    }

    // The local player's ZHitman5 interface, 0 in the front end and while loading.
    uintptr_t local_hitman() const {
        if (!registry_) return 0;
        for (size_t i = 0; i < kMaxPlayers; ++i) {
            const uintptr_t player = registry_ + kPlayers + i * kPlayerSize;
            uintptr_t net = 0;
            const uintptr_t ref = mem::rdp(player + kHitmanEntity);
            if (ref && mem::rd(player + kNetPlayer, net) && !net) return query_interface(ref, "ZHitman5");
        }
        return 0;
    }

    // ZGameTimeManager::m_bPaused (not readable: not paused).
    bool paused() {
        uint8_t flag = 0;
        const bool was = paused_;
        paused_ = time_ && mem::rd(time_ + kPaused, flag) && flag == 1;
        if (paused_ != was) log("glacier: %s", paused_ ? "paused" : "unpaused");
        return paused_;
    }

    bool loop_mode_ = false;  // no PeekMessage: the game loop's frame and the orbit camera (007 First Light)
    bool look_live_ = false;
    bool searched_ = false;
    uintptr_t registry_ = 0;
    uintptr_t time_ = 0;
    uintptr_t hitman_ = 0;
    uintptr_t camera_ = 0;
    uintptr_t controller_ = 0;
    ULONGLONG next_scan_ = 0;
    uintptr_t turned_ = 0;  // the controller write_ and angles_mode_ are about
    bool paused_ = false;
    bool pending_writes_ = false;
    bool input_delta_ = true;
    // The last write into the delta, judged on the next frame: what it left there, the angles then, the game's frame.
    struct Write {
        bool on = false;
        float delta[2] = {};
        float angles[2] = {};
        uint32_t frames = 0;
    } write_;
    bool angles_mode_ = false;  // the game drops the delta (aiming): the turn goes to the angles
    float marker_sign_ = 1;
    uint32_t frames_ = 0;
    uint32_t switches_ = 0;
    uint32_t blends_stopped_ = 0;
    bool cursor_ = false;
};

}  // namespace

Engine& engine() {
    static GlacierEngine instance;
    return instance;
}

}  // namespace gyro4all::glacier
