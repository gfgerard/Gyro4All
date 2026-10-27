#include "nativepad.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "settings.hpp"

#include <windows.h>
#include <hidsdi.h>
#include <intrin.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

namespace gyro4all::nativepad {
namespace {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultEx_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
using WriteFile_t = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetAttributes_t = BOOLEAN(__stdcall*)(HANDLE, PHIDD_ATTRIBUTES);
using GetString_t = BOOLEAN(__stdcall*)(HANDLE, PVOID, ULONG);
using Feature_t = BOOLEAN(__stdcall*)(HANDLE, PVOID, ULONG);
using GetPreparsedData_t = BOOLEAN(__stdcall*)(HANDLE, PHIDP_PREPARSED_DATA*);
using FreePreparsedData_t = BOOLEAN(__stdcall*)(PHIDP_PREPARSED_DATA);
using GetCaps_t = NTSTATUS(__stdcall*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
using GetValueCaps_t = NTSTATUS(__stdcall*)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);

CreateFileW_t g_create_file = nullptr;
ReadFile_t g_read_file = nullptr;
GetOverlappedResult_t g_get_overlapped = nullptr;
GetOverlappedResultEx_t g_get_overlapped_ex = nullptr;
CloseHandle_t g_close_handle = nullptr;
CreateFileA_t g_create_file_a = nullptr;
WriteFile_t g_write_file = nullptr;
GetAttributes_t g_get_attributes = nullptr;
GetString_t g_get_product = nullptr;
Feature_t g_get_feature = nullptr;
Feature_t g_set_feature = nullptr;
GetPreparsedData_t g_get_preparsed = nullptr;
FreePreparsedData_t g_free_preparsed = nullptr;
GetCaps_t g_get_caps = nullptr;
GetValueCaps_t g_get_value_caps = nullptr;

// The original of an import: from the hook, else from the DLL the game imports it from (never called from DllMain).
template <typename F>
F original(F& slot, const char* function, const char* fallback_dll) {
    if (F f = reinterpret_cast<F>(InterlockedCompareExchangePointer(reinterpret_cast<void**>(&slot), nullptr, nullptr)))
        return f;
    std::string dll = hooks::source_dll(function);
    if (dll.empty()) dll = fallback_dll;
    HMODULE m = LoadLibraryA(dll.c_str());
    F f = m ? reinterpret_cast<F>(GetProcAddress(m, function)) : nullptr;
    if (f) InterlockedExchangePointer(reinterpret_cast<void**>(&slot), reinterpret_cast<void*>(f));
    return f;
}

constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;

bool g_suppression = false;            // NativeFlickStick: the read hooks are installed
bool g_as_ds4 = false;                 // DualSenseAsDualShock4: a DualSense is shown to the game as a USB DualShock 4
bool g_as_ds4_learned = false;         // ...and Gyro4All wrote it to the profile itself (DualSenseAsDualShock4Learned)
// Does the game's own PlayStation support take a DualSense? Its Sony library opens every controller it finds; one it
// supports stays open and is read within milliseconds, one it doesn't is closed again unread (Mortal Shell: open,
// HidD_GetAttributes, close). The library only looks at controllers when the game starts (it doesn't look again on
// WM_DEVICECHANGE), so the DualShock 4 identity can only take effect from the next start (the host writes it to the
// game's profile).
std::atomic<ULONGLONG> g_dualsense_opened{0};  // the game first opened a DualSense
std::atomic<bool> g_dualsense_read{false};     // the game read a DualSense
bool g_follow = false;                 // either of them: the game's controller handles are followed
std::atomic<bool> g_refuse{false};     // XInput=2
std::atomic<uint32_t> g_pad_ids{0};    // selected controller: vendor << 16 | product
std::atomic<int> g_refused{0};

// Vendor IDs of HID devices the game opened itself (auto mode's check).
constexpr int kMaxVendors = 8;
std::atomic<uint16_t> g_vendors[kMaxVendors];

// ------------------------------------------------------------------ HID paths

bool is_hid_path(const wchar_t* path) {
    return path && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\' &&
           std::towlower(path[4]) == L'h' && std::towlower(path[5]) == L'i' && std::towlower(path[6]) == L'd';
}

// Vendor or product ID in a HID device path: USB "\\?\hid#vid_054c&pid_0ce6&...", Bluetooth
// "\\?\hid#{00001124-...}_vid&0002054c_pid&0ce6#..." (there "0002" says where the ID comes from). 0 if absent.
uint16_t path_id(const wchar_t* path, const wchar_t* key) {
    for (const wchar_t* p = path; p[0] && p[1] && p[2] && p[3]; ++p) {
        if (std::towlower(p[0]) != key[0] || std::towlower(p[1]) != key[1] || std::towlower(p[2]) != key[2] ||
            (p[3] != L'_' && p[3] != L'&'))
            continue;
        uint32_t value = 0;
        int digits = 0;
        for (const wchar_t* d = p + 4; digits < 8 && std::iswxdigit(*d); ++d, ++digits)
            value = value << 4 | uint32_t(std::iswdigit(*d) ? *d - L'0' : std::towlower(*d) - L'a' + 10);
        if (digits == 4 || digits == 8) return uint16_t(value);
    }
    return 0;
}

// Diagnostics: each HID device the game opens, once per vendor/product and kind of access.
void log_open(uint16_t vendor, uint16_t product, bool reads, bool ok) {
    static std::mutex mutex;
    static uint32_t seen[32];
    static int count = 0;
    const uint32_t key = uint32_t(vendor) << 16 | product;
    const uint32_t entry = key ^ (reads ? 0x80000000u : 0) ^ (ok ? 0 : 0x40000000u);
    std::lock_guard lock(mutex);
    for (int i = 0; i < count; ++i)
        if (seen[i] == entry) return;
    if (count < 32) seen[count++] = entry;
    log("input: the game opens HID device %04X:%04X %s%s", vendor, product,
        reads ? "for reading" : "without read access (looking at it)", ok ? "" : " (failed)");
}

// XInput=2: PlayStation and Nintendo pads, plus the selected controller.
bool refused_device(uint16_t vendor, uint16_t product) {
    if (vendor == kSony || vendor == kNintendo) return true;
    const uint32_t ids = g_pad_ids;
    return ids && vendor == ids >> 16 && product == (ids & 0xFFFF);
}

// ------------------------------------------------------------------ controllers the game reads

// A Sony controller handle the game opened for reading. The handle is published last (release) and cleared first,
// so the read hooks only need an atomic compare to skip every other file.
struct Device {
    std::atomic<HANDLE> handle{nullptr};
    uint16_t vendor = 0, product = 0;
    DWORD report_length = 0;                // the device's input report length (HidP_GetCaps), 0 = unknown
    std::atomic<ULONGLONG> last_report{0};  // tick of the last report with a known layout
    std::atomic<int32_t> stick{-1};         // right stick in that report before any change: x | y << 8
    std::atomic<int32_t> buttons{-1};       // its buttons before any change (SDL button bits), -1 = unknown
    std::atomic<bool> logged{false};        // first report logged
    std::atomic<bool> crc_warned{false};
    bool ds4 = false;                       // a DualSense shown to the game as a (USB) DualShock 4
    bool bluetooth = false;                 // connected over Bluetooth
    DWORD output_length = 0;                // the real device's output and feature report lengths
    DWORD feature_length = 0;
    uint8_t counter = 0;                    // DualShock 4 report counter (6 bits)
    uint8_t out[640] = {};                  // converted output report (stays valid during an overlapped write)
};
constexpr int kMaxDevices = 16;
Device g_devices[kMaxDevices];
std::atomic<int> g_device_count{0};

// Overlapped reads of those handles still in flight: the data arrives later, when GetOverlappedResult(Ex) reports
// the read done, and is changed then (before the game copies it out of its buffer).
struct Pending {
    OVERLAPPED* ol = nullptr;
    HANDLE handle = nullptr;
    uint8_t* buffer = nullptr;
    DWORD size = 0;
};
constexpr int kMaxPending = 16;
Pending g_pending[kMaxPending];
std::atomic<int> g_pending_count{0};
std::mutex g_mutex;  // guards g_devices slot changes and g_pending

std::atomic<bool> g_suppress{false};
std::atomic<float> g_stick_x{0}, g_stick_y{0};  // stick fallback (set per frame)  // centre the selected controller's right stick in the reports (set per frame)
std::atomic<bool> g_neutral{false};   // GyroLib's panel has the controller: every Sony report reads as idle (set per frame)
std::atomic<uint32_t> g_changed{0};   // reports changed so far (diagnostics)
std::atomic<uint32_t> g_neutralised{0};  // reports made idle so far (diagnostics)
// Long-press blocking (longpress.cpp): the selected controller's buttons held back or added (SDL button bits), once its
// button layout was checked against SDL (g_buttons_ok).
std::atomic<uint32_t> g_hide{0}, g_tap{0};
std::atomic<bool> g_buttons_ok{false};
std::atomic<uint32_t> g_buttons_changed{0};  // reports whose buttons changed so far (diagnostics)

int find_device(HANDLE h) {
    // Free slots hold nullptr: a CloseHandle(nullptr) from the game (Pragmata) must not match one.
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    for (int i = 0; i < kMaxDevices; ++i)
        if (g_devices[i].handle.load(std::memory_order_acquire) == h) return i;
    return -1;
}

// The device's real report lengths (the hid.dll exports themselves, not the game's hooked imports): one ReadFile can
// return several input reports back to back (Pacific Drive's libScePad reads 640 bytes, ten 64-byte USB reports, at
// once). hid.dll is resolved here, never in DllMain.
HIDP_CAPS device_caps(HANDLE h) {
    HIDP_CAPS caps{};
    HMODULE hid = LoadLibraryW(L"hid.dll");
    if (!hid) return caps;
    auto get_data = reinterpret_cast<GetPreparsedData_t>(GetProcAddress(hid, "HidD_GetPreparsedData"));
    auto free_data = reinterpret_cast<FreePreparsedData_t>(GetProcAddress(hid, "HidD_FreePreparsedData"));
    auto get_caps = reinterpret_cast<GetCaps_t>(GetProcAddress(hid, "HidP_GetCaps"));
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!get_data || !free_data || !get_caps || !get_data(h, &data)) return caps;
    if (get_caps(data, &caps) != HIDP_STATUS_SUCCESS) caps = {};
    free_data(data);
    return caps;
}

bool is_dualsense(uint16_t vendor, uint16_t product) {
    return vendor == kSony && (product == 0x0CE6 || product == 0x0DF2);  // DualSense, DualSense Edge
}

void track(HANDLE h, uint16_t vendor, uint16_t product, bool ds4, bool bluetooth) {
    const HIDP_CAPS caps = device_caps(h);
    const DWORD length = caps.InputReportByteLength;
    std::lock_guard lock(g_mutex);
    if (find_device(h) >= 0) return;
    for (Device& d : g_devices) {
        if (d.handle.load()) continue;
        d.vendor = vendor;
        d.product = product;
        d.report_length = length;
        d.last_report = 0;
        d.stick = -1;
        d.buttons = -1;
        d.logged = false;
        d.crc_warned = false;
        d.ds4 = ds4;
        d.output_length = caps.OutputReportByteLength;
        d.feature_length = caps.FeatureReportByteLength;
        d.bluetooth = bluetooth;
        d.counter = 0;
        d.handle.store(h, std::memory_order_release);
        ++g_device_count;
        log("nativepad: the game opened controller %04X:%04X (handle %p, %lu-byte input reports)%s", vendor, product, h,
            length, ds4 ? (bluetooth ? ", Bluetooth, shown to it as a USB DualShock 4" : ", shown to it as a DualShock 4") : "");
        return;
    }
    log("nativepad: more than %d controller handles open; %04X:%04X not followed", kMaxDevices, vendor, product);
}

void untrack(HANDLE h) {
    std::lock_guard lock(g_mutex);
    const int i = find_device(h);
    if (i < 0) return;
    g_devices[i].handle.store(nullptr, std::memory_order_release);
    --g_device_count;
    for (Pending& p : g_pending)
        if (p.ol && p.handle == h) {
            p = {};
            --g_pending_count;
        }
    log("nativepad: the game closed controller %04X:%04X (handle %p)", g_devices[i].vendor, g_devices[i].product, h);
}

void remember(OVERLAPPED* ol, HANDLE h, void* buffer, DWORD size) {
    std::lock_guard lock(g_mutex);
    Pending* free = nullptr;
    for (Pending& p : g_pending) {
        if (p.ol == ol) {
            p = {ol, h, static_cast<uint8_t*>(buffer), size};
            return;
        }
        if (!p.ol && !free) free = &p;
    }
    if (!free) {
        static bool warned = false;
        if (!warned) log("nativepad: more than %d controller reads in flight; some reports left alone", kMaxPending);
        warned = true;
        return;
    }
    *free = {ol, h, static_cast<uint8_t*>(buffer), size};
    ++g_pending_count;
}

// ------------------------------------------------------------------ input reports

// Where the right stick is in an input report as the game reads it from Windows (byte 0 = report ID), and where its
// CRC32 is (Bluetooth reports: CRC of 0xA1 + the bytes before it; 0 = none).
//   0x01  DualSense / DualShock 4 over USB, or their basic Bluetooth report: LX LY RX RY at bytes 1-4
//   0x31  DualSense over Bluetooth: sequence byte, then LX LY RX RY at bytes 2-5, CRC at 74
//   0x11  DualShock 4 over Bluetooth: two flag bytes, then LX LY RX RY at bytes 3-6, CRC at 74
// Axes are bytes 0..255 (128 = centre, Y down = 255).
struct Layout {
    int stick = 0;  // right stick X offset (Y follows); 0 = unknown report
    int crc = 0;
    const char* name = "unknown layout, left alone";
};

Layout layout_of(const uint8_t* r, DWORD n) {
    if (n >= 10 && r[0] == 0x01) return {3, 0, "report 0x01 (USB or basic Bluetooth)"};
    if (n >= 78 && r[0] == 0x31) return {4, 74, "report 0x31 (DualSense, Bluetooth)"};
    if (n >= 78 && r[0] == 0x11) return {5, 74, "report 0x11 (DualShock 4, Bluetooth)"};
    return {};
}

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// Bluetooth reports carry a CRC32 of the HID header byte (0xA1 input, 0xA2 output) followed by the report.
uint32_t report_crc(const uint8_t* r, int length, uint8_t seed = 0xA1) {
    return ~crc32(r, size_t(length), crc32(&seed, 1, 0xFFFFFFFFu));
}

uint32_t stored_crc(const uint8_t* r, int at) {
    return uint32_t(r[at]) | uint32_t(r[at + 1]) << 8 | uint32_t(r[at + 2]) << 16 | uint32_t(r[at + 3]) << 24;
}

// The report as an idle controller sends it: sticks centred, no buttons (d-pad released), triggers up, no finger on
// the touchpad; motion, sequence/counter and battery bytes are kept. Offsets from b = the left stick's X byte:
//   DualSense (USB 0x01, Bluetooth 0x31): triggers b+4..5, buttons b+7..10 (d-pad in the low nibble of b+7, 8 =
//     released), touch points at b+32 and b+36 (bit 7 set = no finger)
//   DualShock 4 (USB 0x01, Bluetooth 0x11, a DualSense's basic Bluetooth 0x01, a DualSense shown as a DualShock 4):
//     buttons b+4..5 and the low two bits of b+6 (the counter above them), triggers b+7..8, three touch packets from
//     b+33 (9 bytes each: timestamp, then two 4-byte points)
// The report uses the DualSense's own layout (else the DualShock 4's, see idle_report).
bool dualsense_layout(const Device& d, const uint8_t* r) {
    return is_dualsense(d.vendor, d.product) && !d.ds4 && (r[0] == 0x31 || !d.bluetooth);
}

void idle_report(const Device& d, uint8_t* r, DWORD length, const Layout& layout) {
    const int b = layout.stick - 2;
    const auto set = [&](int i, uint8_t v) { if (DWORD(i) < length) r[i] = v; };
    const auto no_finger = [&](int i) { if (DWORD(i) < length) r[i] |= 0x80; };
    for (int k = 0; k < 4; ++k) set(b + k, 0x80);
    if (dualsense_layout(d, r)) {
        set(b + 4, 0);
        set(b + 5, 0);
        set(b + 7, 0x08);
        for (int k = 8; k <= 10; ++k) set(b + k, 0);
        no_finger(b + 32);
        no_finger(b + 36);
    } else {
        set(b + 4, 0x08);
        set(b + 5, 0);
        if (DWORD(b + 6) < length) r[b + 6] &= 0xFC;
        set(b + 7, 0);
        set(b + 8, 0);
        for (int k = 0; k < 3; ++k) {
            no_finger(b + 34 + 9 * k);
            no_finger(b + 38 + 9 * k);
        }
    }
}

// Buttons as SDL_GamepadButton bits. Both layouts share the bits of their three button bytes (DualSense from b+7,
// DualShock 4 from b+4): d-pad hat in the low nibble of the first (0 up, clockwise, 8 released) and the face buttons
// above it; then L1 R1 L2 R2 Create/Share Options L3 R3; then PS, touchpad click and (DualSense only: on a DualShock 4
// the rest of that byte is its report counter) Mute.
struct ButtonBit {
    SDL_GamepadButton button;
    uint8_t byte, mask;
};
constexpr ButtonBit kButtonBits[] = {
    {SDL_GAMEPAD_BUTTON_WEST, 0, 0x10},           {SDL_GAMEPAD_BUTTON_SOUTH, 0, 0x20},
    {SDL_GAMEPAD_BUTTON_EAST, 0, 0x40},           {SDL_GAMEPAD_BUTTON_NORTH, 0, 0x80},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 1, 0x01},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 1, 0x02},
    {SDL_GAMEPAD_BUTTON_BACK, 1, 0x10},           {SDL_GAMEPAD_BUTTON_START, 1, 0x20},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 1, 0x40},     {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 1, 0x80},
    {SDL_GAMEPAD_BUTTON_GUIDE, 2, 0x01},          {SDL_GAMEPAD_BUTTON_TOUCHPAD, 2, 0x02},
    {SDL_GAMEPAD_BUTTON_MISC1, 2, 0x04}};
constexpr uint32_t bit(SDL_GamepadButton b) { return 1u << b; }
constexpr uint32_t kUp = bit(SDL_GAMEPAD_BUTTON_DPAD_UP), kDown = bit(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                   kLeft = bit(SDL_GAMEPAD_BUTTON_DPAD_LEFT), kRight = bit(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
constexpr uint32_t kHat[9] = {kUp, kUp | kRight, kRight, kDown | kRight, kDown, kDown | kLeft, kLeft, kUp | kLeft, 0};
// Compared with SDL's view to trust the layout (not PS, touchpad or Mute: Windows or Steam may take those).
constexpr uint32_t kCheckedButtons =
    bit(SDL_GAMEPAD_BUTTON_SOUTH) | bit(SDL_GAMEPAD_BUTTON_EAST) | bit(SDL_GAMEPAD_BUTTON_WEST) |
    bit(SDL_GAMEPAD_BUTTON_NORTH) | bit(SDL_GAMEPAD_BUTTON_BACK) | bit(SDL_GAMEPAD_BUTTON_START) |
    bit(SDL_GAMEPAD_BUTTON_LEFT_STICK) | bit(SDL_GAMEPAD_BUTTON_RIGHT_STICK) | bit(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
    bit(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) | kUp | kDown | kLeft | kRight;

// Where the three button bytes start, or -1 if the report is too short.
int button_bytes(const Device& d, const uint8_t* r, DWORD length, const Layout& layout) {
    const int at = layout.stick - 2 + (dualsense_layout(d, r) ? 7 : 4);
    return DWORD(at + 2) < length ? at : -1;
}

uint32_t report_buttons(const Device& d, const uint8_t* r, int at) {
    const bool dualsense = dualsense_layout(d, r);
    uint32_t bits = kHat[std::min(r[at] & 0x0F, 8)];
    for (const ButtonBit& b : kButtonBits)
        if ((dualsense || b.button != SDL_GAMEPAD_BUTTON_MISC1) && (r[at + b.byte] & b.mask)) bits |= bit(b.button);
    return bits;
}

// Writes buttons (SDL bits) into the report; a d-pad pair that can't be one hat value (up and down) keeps up/right.
void write_buttons(const Device& d, uint8_t* r, int at, uint32_t bits) {
    const bool dualsense = dualsense_layout(d, r);
    for (const ButtonBit& b : kButtonBits) {
        if (!dualsense && b.button == SDL_GAMEPAD_BUTTON_MISC1) continue;
        if (bits & bit(b.button)) r[at + b.byte] |= b.mask;
        else r[at + b.byte] &= uint8_t(~b.mask);
    }
    uint32_t pad = bits & (kUp | kDown | kLeft | kRight);
    if ((pad & kUp) && (pad & kDown)) pad &= ~kDown;
    if ((pad & kLeft) && (pad & kRight)) pad &= ~kLeft;
    uint8_t hat = 8;
    for (uint8_t k = 0; k < 9; ++k)
        if (kHat[k] == pad) hat = k;
    r[at] = uint8_t((r[at] & 0xF0) | hat);
}

// What to change in the reports of one read.
struct Edit {
    bool centre_stick = false;  // flick stick: the game doesn't see the right stick
    float add_x = 0, add_y = 0; // stick fallback: deflection added to the right stick (x right, y up)
    bool idle = false;          // GyroLib's panel has the controller: the whole report reads idle
    uint32_t hide = 0, tap = 0;  // long-press blocking (SDL button bits)
};

// One input report (length bytes at r) of controller d: returns the right stick before any change (x | y << 8), or
// -1 if the layout is unknown or the CRC doesn't verify (left alone); buttons gets its buttons before any change (SDL
// bits, -1 if unknown). Applies edit.
int32_t handle_report(Device& d, uint8_t* r, DWORD length, const Edit& edit, int32_t& buttons) {
    buttons = -1;
    const Layout layout = layout_of(r, length);
    if (!layout.stick) return -1;
    if (layout.crc && stored_crc(r, layout.crc) != report_crc(r, layout.crc)) {
        if (!d.crc_warned.exchange(true))
            log("nativepad: %04X:%04X: the report's CRC isn't the expected one; its reports are left alone", d.vendor, d.product);
        return -1;
    }
    const int32_t stick = int32_t(r[layout.stick]) | int32_t(r[layout.stick + 1]) << 8;
    const int at = button_bytes(d, r, length, layout);
    const uint32_t before = at >= 0 ? report_buttons(d, r, at) : 0;
    if (at >= 0) buttons = int32_t(before);
    bool changed = false;
    if (edit.idle) {
        idle_report(d, r, length, layout);
        ++g_neutralised;
        changed = true;
    } else {
        if (edit.centre_stick) {
            r[layout.stick] = r[layout.stick + 1] = 0x80;
            ++g_changed;
            changed = true;
        }
        if (edit.add_x != 0 || edit.add_y != 0) {  // bytes 0..255, centre 0x80, Y down
            const auto add = [](uint8_t& v, double d) { v = uint8_t(std::clamp(v + d * 127, 0.0, 255.0)); };
            add(r[layout.stick], edit.add_x);
            add(r[layout.stick + 1], -edit.add_y);
            changed = true;
        }
        if (at >= 0 && (edit.hide | edit.tap)) {
            const uint32_t after = (before & ~edit.hide) | edit.tap;
            if (after != before) {
                write_buttons(d, r, at, after);
                ++g_buttons_changed;
                changed = true;
            }
        }
    }
    if (changed && layout.crc) {
        const uint32_t crc = report_crc(r, layout.crc);
        for (int k = 0; k < 4; ++k) r[layout.crc + k] = uint8_t(crc >> (8 * k));
    }
    return stick;
}

// A USB DualSense input report (0x01, 64 bytes) rewritten in place as a USB DualShock 4 report (0x01, 64 bytes):
//   DualSense: LX LY RX RY at 1-4, L2 R2 at 5-6, sequence 7, buttons 8-10 (same bits as the DualShock 4's 5-7),
//              gyro 16-21, accelerometer 22-27, timestamp 28-31 (0.33 us), temperature 32, touch 33-40
//   DualShock 4: LX LY RX RY at 1-4, buttons 5-6, PS/touchpad + 6-bit counter 7, L2 R2 8-9, timestamp 10-11
//              (5.33 us), temperature 12, gyro 13-18, accelerometer 19-24, battery/cable 30, touch 33-42
void dualsense_to_ds4(Device& d, uint8_t* r) {
    uint8_t s[64];
    std::memcpy(s, r, 64);
    std::memset(r + 5, 0, 59);
    r[5] = s[8];
    r[6] = s[9];
    r[7] = uint8_t((s[10] & 0x03) | (d.counter << 2));
    d.counter = uint8_t((d.counter + 1) & 0x3F);
    r[8] = s[5];
    r[9] = s[6];
    const uint32_t stamp = uint32_t(s[28]) | uint32_t(s[29]) << 8 | uint32_t(s[30]) << 16 | uint32_t(s[31]) << 24;
    const uint16_t ds4_stamp = uint16_t(stamp / 16);
    r[10] = uint8_t(ds4_stamp);
    r[11] = uint8_t(ds4_stamp >> 8);
    r[12] = s[32];
    std::memcpy(r + 13, s + 16, 6);
    std::memcpy(r + 19, s + 22, 6);
    r[30] = 0x1B;  // cable connected, battery full
    r[33] = 1;     // one touch packet
    r[34] = s[7];
    std::memcpy(r + 35, s + 33, 8);
}

// One report of a DualSense over Bluetooth (length bytes at raw) as a USB DualShock 4 report (64 bytes at out):
// 0x31 (the full report, after GyroLib's SDL asked for it) is the USB report shifted by one byte; 0x01 (the basic
// report a DualSense sends before that) already has the DualShock 4's first ten bytes. False for anything else.
bool bluetooth_to_ds4(Device& d, const uint8_t* raw, DWORD length, uint8_t* out) {
    if (length >= 78 && raw[0] == 0x31) {
        out[0] = 0x01;
        std::memcpy(out + 1, raw + 2, 63);
        dualsense_to_ds4(d, out);
        return true;
    }
    if (length >= 10 && raw[0] == 0x01) {
        std::memset(out, 0, 64);
        std::memcpy(out, raw, 10);
        out[7] = uint8_t((raw[7] & 0x03) | (d.counter << 2));
        d.counter = uint8_t((d.counter + 1) & 0x3F);
        out[30] = 0x1B;
        return true;
    }
    return false;
}

// What the game just received from controller i (n bytes at r, one or more reports of the device's report length):
// note the newest right stick, and centre it in every report while GyroLib's flick stick asks for that.
void on_report(int i, uint8_t* r, DWORD n, const char* how, DWORD chunk = 0) {
    Device& d = g_devices[i];
    if (!r || !n) return;
    DWORD length = chunk ? chunk : d.report_length;
    if (!length) length = r[0] == 0x01 && n % 64 == 0 ? 64 : r[0] == 0x31 && n % 78 == 0 ? 78 : n;  // usual sizes
    if (length > n || n % length) length = n;  // not whole reports: treat it as one
    if (!d.logged.exchange(true)) {
        char head[3 * 8 + 1] = {};
        for (DWORD k = 0; k < n && k < 8; ++k) std::snprintf(head + 3 * k, 4, "%02X ", r[k]);
        log("nativepad: the game reads %04X:%04X (%s): %lu bytes (%lu report(s) of %lu), starts %s- %s", d.vendor, d.product,
            how, n, n / length, length, head, d.vendor == kSony ? layout_of(r, length).name : "not a Sony controller, left alone");
    }
    if (d.vendor != kSony) return;
    if (is_dualsense(d.vendor, d.product)) g_dualsense_read = true;
    if (d.ds4 && !d.bluetooth)
        for (DWORD at = 0; at + 64 <= n && length == 64; at += 64)
            if (r[at] == 0x01) dualsense_to_ds4(d, r + at);
    const bool selected = (uint32_t(d.vendor) << 16 | d.product) == g_pad_ids.load();
    Edit edit;
    edit.centre_stick = selected && g_suppress.load(std::memory_order_relaxed);
    if (selected) {
        edit.add_x = g_stick_x.load(std::memory_order_relaxed);
        edit.add_y = g_stick_y.load(std::memory_order_relaxed);
    }
    edit.idle = g_neutral.load(std::memory_order_relaxed);
    if (selected && g_buttons_ok.load(std::memory_order_relaxed)) {
        edit.hide = g_hide.load(std::memory_order_relaxed);
        edit.tap = g_tap.load(std::memory_order_relaxed);
    }
    int32_t newest = -1, newest_buttons = -1;
    for (DWORD at = 0; at + length <= n; at += length) {
        int32_t buttons = -1;
        if (const int32_t stick = handle_report(d, r + at, length, edit, buttons); stick >= 0) {
            newest = stick;
            newest_buttons = buttons;
        }
    }
    if (newest < 0) return;
    d.stick = newest;
    d.buttons = newest_buttons;
    d.last_report = GetTickCount64();
}

// GetOverlappedResult(Ex) said how an overlapped read ended.
void finish(HANDLE h, OVERLAPPED* ol, BOOL ok, DWORD n, DWORD error) {
    Pending p;
    {
        std::lock_guard lock(g_mutex);
        Pending* found = nullptr;
        for (Pending& q : g_pending)
            if (q.ol == ol && q.handle == h) found = &q;
        if (!found || (!ok && error == ERROR_IO_INCOMPLETE)) return;  // not ours, or still in flight
        p = *found;
        *found = {};
        --g_pending_count;
    }
    if (!ok) return;
    if (const int i = find_device(h); i >= 0) on_report(i, p.buffer, std::min(n, p.size), "overlapped reads");
}

// ------------------------------------------------------------------ hooks (any game thread)

// The module is Gyro4All, GyroLib or GyroLib's SDL3.dll (or another DLL in Gyro4All's folder). Each other module that opens a controller is
// logged once.
bool opened_by_us(HMODULE module) {
    static std::mutex mutex;
    static std::vector<std::pair<HMODULE, bool>> known;
    std::lock_guard lock(mutex);
    for (const auto& [m, ours] : known)
        if (m == module) return ours;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&opened_by_us), &self);
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring folder = settings::install_dir() + L"\\";
    std::wstring file = path;
    for (wchar_t& c : folder) c = wchar_t(std::towlower(c));
    for (wchar_t& c : file) c = wchar_t(std::towlower(c));
    // GyroLib unpacks SDL3.dll to its own runtime folder; within a process SDL3.dll is one module, GyroLib's.
    const bool ours = module == self || module == GetModuleHandleW(L"gyrolib.dll") ||
                      module == GetModuleHandleW(L"SDL3.dll") || file.rfind(folder, 0) == 0;
    known.emplace_back(module, ours);
    if (!ours) log("nativepad: %ls opens HID devices; its controllers are followed", path);
    return ours;
}

// Cheap prefix test first: this runs for every file the game opens.
HANDLE WINAPI hk_create_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE templ) {
    // Inline hooks (protected exe) see every caller in the process. Opens by Gyro4All, gyrolib.dll and its SDL3.dll
    // (GyroLib's controller reader) pass untouched; the game's, and those of the system
    // DLLs it reads controllers through, are followed (RE2 reads the DualSense through DirectInput: DINPUT8.dll opens
    // and reads it).
    if (hooks::inline_mode() && is_hid_path(path)) {
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(_ReturnAddress()), &caller);
        if (caller && opened_by_us(caller)) {
            CreateFileW_t real = original(g_create_file, "CreateFileW", "kernel32.dll");
            return real ? real(path, access, share, sa, disposition, flags, templ) : INVALID_HANDLE_VALUE;
        }
    }
    const bool hid = is_hid_path(path);
    uint16_t vendor = 0, product = 0;
    if (hid) {
        vendor = path_id(path, L"vid");
        product = path_id(path, L"pid");
        if (g_refuse && refused_device(vendor, product)) {
            const int n = ++g_refused;
            if (n <= 3)
                log("xinput: XInput=2: the game's access to HID device %04X:%04X refused (it gets the Xbox controller)%s",
                    vendor, product, n == 3 ? "; further refusals not logged" : "");
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (vendor)
            for (auto& slot : g_vendors) {
                uint16_t expected = 0;
                if (slot.load() == vendor || slot.compare_exchange_strong(expected, vendor)) break;
            }
    }
    CreateFileW_t real = original(g_create_file, "CreateFileW", "kernel32.dll");
    HANDLE h = real ? real(path, access, share, sa, disposition, flags, templ) : INVALID_HANDLE_VALUE;
    // An enumeration opens each device without access rights; only handles that can read are followed.
    const bool reads = (access & (GENERIC_READ | GENERIC_ALL | MAXIMUM_ALLOWED | FILE_READ_DATA)) != 0;
    if (hid && vendor) {
        const DWORD error = GetLastError();
        log_open(vendor, product, reads, h != INVALID_HANDLE_VALUE);
        SetLastError(error);
    }
    if (h == INVALID_HANDLE_VALUE || !g_follow) return h;
    // A DualSense shown as a DualShock 4 is followed even when only looked at: the game identifies it then.
    const bool bluetooth = hid && std::wcsstr(path, L"{00001124") != nullptr;
    const bool dualsense = hid && is_dualsense(vendor, product);
    const bool ds4 = g_as_ds4 && dualsense;
    if (dualsense) {
        ULONGLONG expected = 0;
        g_dualsense_opened.compare_exchange_strong(expected, GetTickCount64());
    }
    // Every HID device read is logged with NativeFlickStick; Sony controllers read by the game are always followed (the
    // panel's idle controller and long-press blocking need them); only Sony reports change.
    if (hid && vendor && (dualsense || (reads && (g_suppression || vendor == kSony)))) {
        const DWORD error = GetLastError();
        track(h, vendor, product, ds4, bluetooth);
        SetLastError(error);
    } else if (g_device_count.load(std::memory_order_relaxed) && find_device(h) >= 0) {
        const DWORD error = GetLastError();
        untrack(h);  // the handle was closed without CloseHandle being seen, and its value is reused
        SetLastError(error);
    }
    return h;
}

// ANSI opens (Space Marine 2 imports both): HID device paths go through hk_create_file as Unicode (the same open: the
// ANSI export converts and calls the Unicode one), so those handles are followed too; any other path is left alone.
HANDLE WINAPI hk_create_file_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE templ) {
    if (path && path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\' && (path[4] | 0x20) == 'h' &&
        (path[5] | 0x20) == 'i' && (path[6] | 0x20) == 'd') {
        wchar_t wide[1024];
        if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, 1024)) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) log("nativepad: the game opens HID devices through CreateFileA too; followed");
            return hk_create_file(wide, access, share, sa, disposition, flags, templ);
        }
    }
    CreateFileA_t real = original(g_create_file_a, "CreateFileA", "kernel32.dll");
    return real ? real(path, access, share, sa, disposition, flags, templ) : INVALID_HANDLE_VALUE;
}

// A read of a Bluetooth DualSense shown as a USB DualShock 4: the game's buffer holds 64-byte reports, the device's
// are longer (78 bytes and up), so Gyro4All reads them into its own buffer (same number of reports) and gives the game
// converted ones. Overlapped reads are waited for here (reports come every few milliseconds) and completed at once.
BOOL bluetooth_read(int i, ReadFile_t real, HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED ol) {
    Device& d = g_devices[i];
    const DWORD report = d.report_length ? d.report_length : 78;
    const DWORD count = std::max<DWORD>(1, size / 64);
    thread_local std::vector<uint8_t> raw;
    raw.assign(size_t(count) * report, 0);
    DWORD got = 0;
    BOOL ok = FALSE;
    if (ol) {
        thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED mine{};
        mine.hEvent = event;
        ResetEvent(event);
        ok = real(h, raw.data(), DWORD(raw.size()), nullptr, &mine);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) CancelIoEx(h, &mine);
            GetOverlappedResult_t get = original(g_get_overlapped, "GetOverlappedResult", "kernel32.dll");
            ok = get && get(h, &mine, &got, TRUE);
            if (!ok && GetLastError() == ERROR_OPERATION_ABORTED) ok = TRUE, got = 0;  // nothing within a second
        } else if (ok) {
            got = DWORD(mine.InternalHigh);
        }
    } else {
        ok = real(h, raw.data(), DWORD(raw.size()), &got, nullptr);
    }
    if (!ok) return FALSE;
    auto* out = static_cast<uint8_t*>(buffer);
    DWORD n = 0;
    for (DWORD at = 0; at + report <= got && n + 64 <= size; at += report)
        if (bluetooth_to_ds4(d, raw.data() + at, report, out + n)) n += 64;
    on_report(i, out, n, ol ? "Bluetooth, overlapped reads, converted" : "Bluetooth, synchronous reads, converted", 64);
    if (ol) {
        ol->Internal = 0;
        ol->InternalHigh = n;
        if (ol->hEvent) SetEvent(ol->hEvent);
    }
    if (read) *read = n;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI hk_read_file(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED ol) {
    ReadFile_t real = original(g_read_file, "ReadFile", "kernel32.dll");
    if (g_as_ds4 && real && buffer && g_device_count.load(std::memory_order_relaxed))
        if (const int i = find_device(h); i >= 0 && g_devices[i].ds4 && g_devices[i].bluetooth)
            return bluetooth_read(i, real, h, buffer, size, read, ol);
    const BOOL ok = real ? real(h, buffer, size, read, ol) : FALSE;
    if (!g_device_count.load(std::memory_order_relaxed)) return ok;
    const int i = find_device(h);
    if (i < 0) return ok;
    const DWORD error = GetLastError();
    // Diagnostics (protected exes): the first reads of followed handles, with their caller and how they ended.
    static std::atomic<int> read_logged{0};
    if (hooks::inline_mode() && read_logged.fetch_add(1) < 6) {
        HMODULE caller = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(_ReturnAddress()), &caller))
            GetModuleFileNameW(caller, name, MAX_PATH);
        const wchar_t* base = std::wcsrchr(name, L'\\');
        log("nativepad: ReadFile of controller handle %p from %ls, %lu bytes, %s", h, base ? base + 1 : name, size,
            ok ? "done at once" : error == ERROR_IO_PENDING ? "pending" : "failed");
    }
    if (ok) {
        const DWORD n = read ? *read : ol ? DWORD(ol->InternalHigh) : 0;
        on_report(i, static_cast<uint8_t*>(buffer), n, ol ? "overlapped reads, done at once" : "synchronous reads");
    } else if (ol && error == ERROR_IO_PENDING) {
        remember(ol, h, buffer, size);
    }
    SetLastError(error);
    return ok;
}

BOOL WINAPI hk_get_overlapped(HANDLE h, LPOVERLAPPED ol, LPDWORD n, BOOL wait) {
    GetOverlappedResult_t real = original(g_get_overlapped, "GetOverlappedResult", "kernel32.dll");
    const BOOL ok = real ? real(h, ol, n, wait) : FALSE;
    if (!g_pending_count.load(std::memory_order_relaxed)) return ok;
    const DWORD error = GetLastError();
    finish(h, ol, ok, ok && n ? *n : 0, error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI hk_get_overlapped_ex(HANDLE h, LPOVERLAPPED ol, LPDWORD n, DWORD timeout, BOOL alertable) {
    GetOverlappedResultEx_t real = original(g_get_overlapped_ex, "GetOverlappedResultEx", "kernel32.dll");
    const BOOL ok = real ? real(h, ol, n, timeout, alertable) : FALSE;
    if (!g_pending_count.load(std::memory_order_relaxed)) return ok;
    const DWORD error = GetLastError();
    finish(h, ol, ok, ok && n ? *n : 0, error);
    SetLastError(error);
    return ok;
}

// The followed DualSense shown as a DualShock 4 (index into g_devices), or -1.
int ds4_device(HANDLE h) {
    if (!g_as_ds4 || !g_device_count.load(std::memory_order_relaxed)) return -1;
    const int i = find_device(h);
    return i >= 0 && g_devices[i].ds4 ? i : -1;
}

constexpr uint16_t kDs4Product = 0x09CC;  // DualShock 4 (second version)

BOOLEAN __stdcall hk_get_attributes(HANDLE h, PHIDD_ATTRIBUTES attributes) {
    GetAttributes_t real = original(g_get_attributes, "HidD_GetAttributes", "hid.dll");
    const BOOLEAN ok = real ? real(h, attributes) : FALSE;
    if (ok && attributes && ds4_device(h) >= 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            log("nativepad: DualSense %04X:%04X (version %04X) shown to the game as a DualShock 4 (%04X:%04X, version 0100)",
                attributes->VendorID, attributes->ProductID, attributes->VersionNumber, kSony, kDs4Product);
        attributes->ProductID = kDs4Product;
        // Sony's library reads the version as decimal digits and wants at least 23: a USB DualSense passes, over
        // Bluetooth Windows reports another value and the pad is skipped. A USB DualShock 4 reports 0x0100.
        attributes->VersionNumber = 0x0100;
    }
    return ok;
}

BOOLEAN __stdcall hk_get_product(HANDLE h, PVOID buffer, ULONG length) {
    if (ds4_device(h) >= 0 && buffer && length >= 2) {
        const wchar_t name[] = L"Wireless Controller";
        const size_t chars = std::min<size_t>(length / sizeof(wchar_t) - 1, std::size(name) - 1);
        std::memcpy(buffer, name, chars * sizeof(wchar_t));
        static_cast<wchar_t*>(buffer)[chars] = 0;
        return TRUE;
    }
    GetString_t real = original(g_get_product, "HidD_GetProductString", "hid.dll");
    return real ? real(h, buffer, length) : FALSE;
}

// Feature reports a DualShock 4 driver asks for, answered from the DualSense: 0x02 calibration (the DualSense's 0x05
// has the same layout), 0x12 / 0x81 the controller's address (DualSense 0x09), 0xA3 (firmware) zeros; any other ID
// fails, as on a real DualShock 4. Only the report's own size is written: like the device, never the whole buffer the
// game says it has (Mortal Shell passes a larger length than the buffer it allocated for 0x12).
DWORD ds4_feature_size(uint8_t id) {
    switch (id) {
    case 0x02: return 37;
    case 0x12: return 16;
    case 0x81: return 7;
    case 0xA3: return 49;
    default: return 0;
    }
}

BOOLEAN __stdcall hk_get_feature(HANDLE h, PVOID buffer, ULONG length) {
    Feature_t real = original(g_get_feature, "HidD_GetFeature", "hid.dll");
    if (ds4_device(h) < 0 || !buffer || !length) return real ? real(h, buffer, length) : FALSE;
    auto* b = static_cast<uint8_t*>(buffer);
    const uint8_t id = b[0];
    const DWORD size = ds4_feature_size(id);
    const int device = ds4_device(h);
    uint8_t source[640] = {};
    // The DualSense's own feature reports: their exact size over USB, the device's feature length over Bluetooth.
    const auto read_feature = [&](uint8_t source_id, ULONG usb_size) {
        source[0] = source_id;
        const Device& d = g_devices[device];
        const ULONG n = d.bluetooth && d.feature_length ? std::min<ULONG>(d.feature_length, sizeof source) : usb_size;
        return real && real(h, source, n);
    };
    const char* how = "zeros";
    BOOLEAN ok = TRUE;
    if (!size || length < size) {
        ok = FALSE;
        how = size ? "nothing (buffer too small)" : "nothing (not a DualShock 4 report this emulation knows)";
    } else {
        std::memset(b + 1, 0, size - 1);
    }
    if (!ok) {
    } else if (id == 0x02) {
        ok = read_feature(0x05, 41);
        if (ok) std::memcpy(b + 1, source + 1, 34);
        how = ok ? "the DualSense's calibration (0x05)" : "failed to read the DualSense's calibration";
    } else if (id == 0xA3) {
        // Build date and time strings, then little-endian versions at 0x21, 0x23 (hardware: Sony's library needs at
        // least 0x3100), 0x25 (32 bits), 0x29, 0x2B, 0x2D (32 bits).
        std::memcpy(b + 1, "Sep 21 2018", 11);
        std::memcpy(b + 17, "04:50:51", 8);
        const uint16_t versions[] = {0x0100, 0xB400, 0, 0, 0xA007, 0x0001};
        const int at[] = {0x21, 0x23, 0x25, 0x27, 0x29, 0x2B};
        for (int k = 0; k < 6; ++k) {
            b[at[k]] = uint8_t(versions[k]);
            b[at[k] + 1] = uint8_t(versions[k] >> 8);
        }
        how = "a DualShock 4 firmware description";
    } else if (id == 0x12 || id == 0x81) {
        if (read_feature(0x09, 20)) {
            std::memcpy(b + 1, source + 1, 6);
            how = "the DualSense's address (0x09)";
        }
    }
    static std::atomic<uint32_t> seen[8];
    bool fresh = true;
    for (auto& v : seen) {
        uint32_t expected = 0;
        if (v.load() == id + 1u) { fresh = false; break; }
        if (v.compare_exchange_strong(expected, id + 1u)) break;
    }
    if (fresh)
        log("nativepad: DualShock 4 feature report 0x%02X (buffer %lu bytes) asked by the game: answered with %s", id, length, how);
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    return ok;
}

BOOLEAN __stdcall hk_set_feature(HANDLE h, PVOID buffer, ULONG length) {
    if (ds4_device(h) >= 0) {
        static std::atomic<int> count{0};
        if (++count <= 5)
            log("nativepad: DualShock 4 feature report 0x%02X sent by the game: not passed to the DualSense",
                buffer && length ? static_cast<uint8_t*>(buffer)[0] : 0);
        return TRUE;
    }
    Feature_t real = original(g_set_feature, "HidD_SetFeature", "hid.dll");
    return real ? real(h, buffer, length) : FALSE;
}

// Preparsed data of a DualSense shown as a DualShock 4: HidP_GetCaps then reports the DualShock 4's USB lengths.
std::mutex g_preparsed_mutex;
PHIDP_PREPARSED_DATA g_preparsed[8];

BOOLEAN __stdcall hk_get_preparsed(HANDLE h, PHIDP_PREPARSED_DATA* data) {
    GetPreparsedData_t real = original(g_get_preparsed, "HidD_GetPreparsedData", "hid.dll");
    const BOOLEAN ok = real ? real(h, data) : FALSE;
    if (ok && data && *data && ds4_device(h) >= 0) {
        std::lock_guard lock(g_preparsed_mutex);
        for (auto& p : g_preparsed)
            if (!p) { p = *data; break; }
    }
    return ok;
}

BOOLEAN __stdcall hk_free_preparsed(PHIDP_PREPARSED_DATA data) {
    {
        std::lock_guard lock(g_preparsed_mutex);
        for (auto& p : g_preparsed)
            if (p == data) p = nullptr;
    }
    FreePreparsedData_t real = original(g_free_preparsed, "HidD_FreePreparsedData", "hid.dll");
    return real ? real(data) : FALSE;
}

bool is_ds4_preparsed(PHIDP_PREPARSED_DATA data) {
    std::lock_guard lock(g_preparsed_mutex);
    for (auto& p : g_preparsed)
        if (p && p == data) return true;
    return false;
}

// The DualShock 4's feature reports as Sony's PC library checks them in the device's value caps before accepting it:
// (usage page, usage, report ID, report count in bytes). The library also wants many of them (at least 19), so the
// list carries the DualShock 4's other vendor feature reports too.
struct FeatureCap {
    USHORT page, usage;
    UCHAR id;
    USHORT count;
};
constexpr FeatureCap kDs4Features[] = {
    {0xFF00, 0x24, 0x02, 0x24}, {0xFF80, 0x43, 0xA3, 0x30}, {0xFF02, 0x21, 0x12, 0x0F}, {0xFF02, 0x22, 0x13, 0x16},
    {0xFF05, 0x20, 0x14, 0x10}, {0xFF05, 0x21, 0x15, 0x2C}, {0xFF80, 0x20, 0x80, 0x06}, {0xFF80, 0x21, 0x81, 0x06},
    {0xFF80, 0x22, 0x82, 0x05}, {0xFF80, 0x23, 0x83, 0x01}, {0xFF80, 0x24, 0x84, 0x04}, {0xFF80, 0x25, 0x85, 0x06},
    {0xFF80, 0x26, 0x86, 0x06}, {0xFF80, 0x27, 0x87, 0x23}, {0xFF80, 0x28, 0x88, 0x22}, {0xFF80, 0x29, 0x89, 0x02},
    {0xFF80, 0x30, 0x90, 0x05}, {0xFF80, 0x31, 0x91, 0x03}, {0xFF80, 0x32, 0x92, 0x03}, {0xFF80, 0x33, 0x93, 0x0C},
    {0xFF80, 0x40, 0xA0, 0x06}, {0xFF80, 0x41, 0xA1, 0x01}, {0xFF80, 0x42, 0xA2, 0x01}, {0xFF80, 0x44, 0xA4, 0x0D},
    {0xFF80, 0x57, 0xE1, 0x30}, {0xFF80, 0x57, 0xE3, 0x04}, {0xFF80, 0x57, 0xF0, 0x10}};
constexpr USHORT kDs4FeatureCount = USHORT(std::size(kDs4Features));

NTSTATUS __stdcall hk_get_caps(PHIDP_PREPARSED_DATA data, PHIDP_CAPS caps) {
    GetCaps_t real = original(g_get_caps, "HidP_GetCaps", "hid.dll");
    const NTSTATUS status = real ? real(data, caps) : HIDP_STATUS_INVALID_PREPARSED_DATA;
    if (status == HIDP_STATUS_SUCCESS && caps && data && is_ds4_preparsed(data)) {
        caps->InputReportByteLength = 64;
        caps->OutputReportByteLength = 32;
        caps->FeatureReportByteLength = 64;
        caps->NumberFeatureButtonCaps = 0;
        caps->NumberFeatureValueCaps = kDs4FeatureCount;
        caps->NumberFeatureDataIndices = kDs4FeatureCount;
    }
    return status;
}

NTSTATUS __stdcall hk_get_value_caps(HIDP_REPORT_TYPE type, PHIDP_VALUE_CAPS value_caps, PUSHORT length,
                                     PHIDP_PREPARSED_DATA data) {
    GetValueCaps_t real = original(g_get_value_caps, "HidP_GetValueCaps", "hid.dll");
    if (type != HidP_Feature || !length || !data || !is_ds4_preparsed(data))
        return real ? real(type, value_caps, length, data) : HIDP_STATUS_INVALID_PREPARSED_DATA;
    const USHORT room = value_caps ? *length : 0;
    const USHORT n = std::min(room, kDs4FeatureCount);
    for (USHORT k = 0; k < n; ++k) {
        HIDP_VALUE_CAPS& c = value_caps[k];
        std::memset(&c, 0, sizeof c);
        c.UsagePage = kDs4Features[k].page;
        c.ReportID = kDs4Features[k].id;
        c.BitSize = 8;
        c.ReportCount = kDs4Features[k].count;
        c.LogicalMax = 255;
        c.PhysicalMax = 255;
        c.IsAbsolute = TRUE;
        c.LinkUsage = 5;
        c.LinkUsagePage = 1;
        c.NotRange.Usage = kDs4Features[k].usage;
        c.NotRange.DataIndex = k;
    }
    *length = kDs4FeatureCount;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        log("nativepad: DualShock 4 feature value caps (%u) given to the game (room for %u)", unsigned(kDs4FeatureCount), unsigned(room));
    return room >= kDs4FeatureCount ? HIDP_STATUS_SUCCESS : HIDP_STATUS_BUFFER_TOO_SMALL;
}

// A DualShock 4 USB output report (0x05: flags, right/weak and left/strong motor at 4-5, light bar RGB at 6-8) sent to
// a DualSense as its output report (USB 0x02: enable bits 1-2, motors 3-4, light bar RGB 45-47; Bluetooth 0x31, see
// below). Written with our own buffer; the game's request is completed as written in full. Other reports are dropped.
BOOL WINAPI hk_write_file(HANDLE h, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED ol) {
    WriteFile_t real = original(g_write_file, "WriteFile", "kernel32.dll");
    const int i = g_as_ds4 ? ds4_device(h) : -1;
    if (i < 0 || !buffer || !size) return real ? real(h, buffer, size, written, ol) : FALSE;
    Device& d = g_devices[i];
    const auto* in = static_cast<const uint8_t*>(buffer);
    const DWORD length = std::min<DWORD>(d.output_length ? d.output_length : (d.bluetooth ? 78 : 48), sizeof d.out);
    const uint8_t id = d.bluetooth ? 0x31 : 0x02;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 3)
        log("nativepad: DualShock 4 output report 0x%02X (%lu bytes) from the game -> DualSense report 0x%02X (%lu bytes)%s",
            in[0], size, id, length, in[0] == 0x05 ? "" : ": not a USB output report, dropped");
    BOOL ok = TRUE;
    if (in[0] == 0x05 && size >= 11 && length >= (d.bluetooth ? 78u : 48u) && real) {
        // Over Bluetooth: 0x31, a 0x02 tag, the same effects block one byte later, CRC32 (seed 0xA2) at 74.
        uint8_t* out = d.out;
        std::memset(out, 0, length);
        out[0] = id;
        const int base = d.bluetooth ? 2 : 1;
        if (d.bluetooth) out[1] = 0x02;
        if (in[1] & 0x01) {
            out[base] = 0x03;  // rumble (compatible vibration)
            out[base + 2] = in[4];
            out[base + 3] = in[5];
        }
        if (in[1] & 0x02) {
            out[base + 1] = 0x04;  // light bar colour
            out[base + 44] = in[6];
            out[base + 45] = in[7];
            out[base + 46] = in[8];
        }
        if (d.bluetooth) {
            const uint32_t crc = report_crc(out, 74, 0xA2);
            for (int k = 0; k < 4; ++k) out[74 + k] = uint8_t(crc >> (8 * k));
        }
        DWORD done = 0;
        if (ol) {
            OVERLAPPED mine{};
            mine.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ok = real(h, out, length, nullptr, &mine);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(mine.hEvent, 200) != WAIT_OBJECT_0) CancelIoEx(h, &mine);
                GetOverlappedResult_t get = original(g_get_overlapped, "GetOverlappedResult", "kernel32.dll");
                ok = get && get(h, &mine, &done, TRUE);
            }
            if (mine.hEvent) CloseHandle(mine.hEvent);
        } else {
            ok = real(h, out, length, &done, nullptr);
        }
    }
    // Completed as far as the game knows (a failed rumble write isn't worth an error in its controller code).
    if (ol) {
        ol->Internal = 0;
        ol->InternalHigh = size;
        if (ol->hEvent) SetEvent(ol->hEvent);
    }
    if (written) *written = size;
    SetLastError(ERROR_SUCCESS);
    (void)ok;
    return TRUE;
}

// Protected exes, diagnostics only: other ways the game could read a followed controller, logged once each.
using ReadFileEx_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
using DeviceIoControl_t = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
ReadFileEx_t g_read_file_ex = nullptr;
DeviceIoControl_t g_device_io = nullptr;
std::atomic<bool> g_read_ex_logged{false};
std::atomic<int> g_device_io_logged{0};
DWORD g_device_io_codes[8] = {};

BOOL WINAPI hk_read_file_ex(HANDLE h, LPVOID buffer, DWORD size, LPOVERLAPPED ol, LPOVERLAPPED_COMPLETION_ROUTINE done) {
    if (g_device_count.load(std::memory_order_relaxed) && find_device(h) >= 0 && !g_read_ex_logged.exchange(true))
        log("nativepad: the game reads its controller with ReadFileEx (completion routines; its reports aren't followed)");
    return g_read_file_ex(h, buffer, size, ol, done);
}

BOOL WINAPI hk_device_io(HANDLE h, DWORD code, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD n,
                         LPOVERLAPPED ol) {
    // Each distinct IOCTL code once (8 at most): HidD_GetInputReport polling would show up as 0xB01A2.
    if (g_device_count.load(std::memory_order_relaxed) && g_device_io_logged.load() < 8 && find_device(h) >= 0) {
        bool seen = false;
        for (DWORD c : g_device_io_codes) seen |= c == code;
        if (!seen) {
            g_device_io_codes[std::min(g_device_io_logged.fetch_add(1), 7)] = code;
            log("nativepad: the game sends its controller IOCTL 0x%lX (DeviceIoControl, handle %p, %lu bytes out)", code,
                h, out_size);
        }
    }
    return g_device_io(h, code, in, in_size, out, out_size, n, ol);
}

// Protected exes: the game may get its controller's reports through Raw Input (WM_INPUT) instead of reading the
// handles it opens (RE2: the exe only identifies the DualSense with HID IOCTLs, nothing reads it with ReadFile). While
// GyroLib's panel has the controller, Sony HID reports handed out by GetRawInputData/GetRawInputBuffer are made idle.
// GyroLib reads the controller over HID, not Raw Input, so the panel still gets it.
using RegisterRawInputDevices_t = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
RegisterRawInputDevices_t g_register_raw = nullptr;
GetRawInputData_t g_get_raw_data = nullptr;
GetRawInputBuffer_t g_get_raw_buffer = nullptr;

// Raw Input devices seen so far: whether each is a Sony controller, as a Device for the report helpers.
struct RawPad {
    HANDLE device = nullptr;
    bool sony = false;
    Device d;
};
constexpr int kMaxRawPads = 16;
RawPad g_raw_pads[kMaxRawPads];
std::atomic<int> g_raw_pad_count{0};
std::mutex g_raw_mutex;

RawPad* raw_pad(HANDLE device) {
    const int n = g_raw_pad_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_raw_pads[i].device == device) return &g_raw_pads[i];
    std::lock_guard lock(g_raw_mutex);
    const int count = g_raw_pad_count.load();
    for (int i = n; i < count; ++i)
        if (g_raw_pads[i].device == device) return &g_raw_pads[i];
    if (count >= kMaxRawPads) return nullptr;
    RawPad& p = g_raw_pads[count];
    wchar_t name[512] = L"";
    UINT length = 512;
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &length);
    p.device = device;
    p.d.vendor = path_id(name, L"vid");
    p.d.product = path_id(name, L"pid");
    p.d.bluetooth = std::wcsstr(name, L"{00001124") != nullptr;
    p.sony = p.d.vendor == kSony;
    g_raw_pad_count.store(count + 1, std::memory_order_release);
    log("nativepad: the game receives Raw Input from HID device %04X:%04X%s", p.d.vendor, p.d.product,
        p.sony ? " (Sony controller: made idle while GyroLib's panel is open)" : "");
    return &p;
}

// One RAWINPUT block the game received.
void on_raw_input(RAWINPUT* input) {
    if (!input || input->header.dwType != RIM_TYPEHID) return;
    RawPad* pad = raw_pad(input->header.hDevice);
    if (!pad || !pad->sony) return;
    const RAWHID& hid = input->data.hid;
    if (!pad->d.logged.exchange(true))
        log("nativepad: Raw Input reports of %04X:%04X: %lu x %lu bytes, %s", pad->d.vendor, pad->d.product, hid.dwCount,
            hid.dwSizeHid, hid.dwSizeHid ? layout_of(hid.bRawData, hid.dwSizeHid).name : "empty");
    if (!g_neutral.load(std::memory_order_relaxed) || !hid.dwSizeHid) return;
    Edit edit;
    edit.idle = true;
    auto* r = const_cast<BYTE*>(hid.bRawData);
    for (DWORD k = 0; k < hid.dwCount; ++k) {
        int32_t buttons = -1;
        handle_report(pad->d, r + size_t(k) * hid.dwSizeHid, hid.dwSizeHid, edit, buttons);
    }
}

BOOL WINAPI hk_register_raw(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    for (UINT i = 0; devices && i < count && i < 16; ++i)
        if (devices[i].usUsagePage == 1 && devices[i].usUsage != 2 && devices[i].usUsage != 6)
            log("nativepad: Raw Input registered for usage page 1 usage 0x%X (flags 0x%lX)", unsigned(devices[i].usUsage),
                devices[i].dwFlags);
    return g_register_raw(devices, count, size);
}

UINT WINAPI hk_get_raw_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const UINT n = g_get_raw_data(input, command, data, size, header_size);
    if (command == RID_INPUT && data && n != UINT(-1) && n >= sizeof(RAWINPUTHEADER))
        on_raw_input(static_cast<RAWINPUT*>(data));
    return n;
}

UINT WINAPI hk_get_raw_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    const UINT n = g_get_raw_buffer(data, size, header_size);
    if (data && n != UINT(-1)) {
        PRAWINPUT block = data;
        // NEXTRAWINPUTBLOCK: the next block starts 8-byte aligned after this one.
        for (UINT i = 0; i < n; ++i) {
            on_raw_input(block);
            block = reinterpret_cast<PRAWINPUT>((reinterpret_cast<uintptr_t>(block) + block->header.dwSize + 7) & ~uintptr_t(7));
        }
    }
    return n;
}

BOOL WINAPI hk_close_handle(HANDLE h) {
    if (g_device_count.load(std::memory_order_relaxed) && find_device(h) >= 0) untrack(h);
    CloseHandle_t real = original(g_close_handle, "CloseHandle", "kernel32.dll");
    return real ? real(h) : FALSE;
}

}  // namespace

void install(bool suppression, bool dualsense_as_ds4, bool learned) {
    if (hooks::inline_mode()) {
        // Protected exe: no import slots, so inline hooks on the system exports, acting only on controllers the game
        // opens, itself or through a system DLL (hk_create_file skips GyroLib's opens; the other hooks only touch
        // handles it followed). Enough to follow the
        // game's Sony controller reads, so GyroLib's panel can idle them and NativeFlickStick can edit them. The
        // DualShock 4 identity (HID metadata and output reports) is left out here.
        g_suppression = suppression;
        g_follow = true;
        // Reads on KernelBase's exports: RE2's exe doesn't read through kernel32's (kernel32's stubs jump to these, so
        // both kinds of callers pass once). Opens on kernel32's, whose hook sees the caller's return address.
        auto add = [](const char* dll, const char* function, void* hook, void** slot) { hooks::add(dll, function, hook, slot); };
        add("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(&hk_create_file), reinterpret_cast<void**>(&g_create_file));
        add("kernel32.dll", "CloseHandle", reinterpret_cast<void*>(&hk_close_handle), reinterpret_cast<void**>(&g_close_handle));
        add("kernelbase.dll", "ReadFile", reinterpret_cast<void*>(&hk_read_file), reinterpret_cast<void**>(&g_read_file));
        add("kernelbase.dll", "GetOverlappedResult", reinterpret_cast<void*>(&hk_get_overlapped), reinterpret_cast<void**>(&g_get_overlapped));
        add("kernelbase.dll", "GetOverlappedResultEx", reinterpret_cast<void*>(&hk_get_overlapped_ex), reinterpret_cast<void**>(&g_get_overlapped_ex));
        add("kernelbase.dll", "ReadFileEx", reinterpret_cast<void*>(&hk_read_file_ex), reinterpret_cast<void**>(&g_read_file_ex));
        add("kernelbase.dll", "DeviceIoControl", reinterpret_cast<void*>(&hk_device_io), reinterpret_cast<void**>(&g_device_io));
        add("user32.dll", "RegisterRawInputDevices", reinterpret_cast<void*>(&hk_register_raw), reinterpret_cast<void**>(&g_register_raw));
        add("user32.dll", "GetRawInputData", reinterpret_cast<void*>(&hk_get_raw_data), reinterpret_cast<void**>(&g_get_raw_data));
        add("user32.dll", "GetRawInputBuffer", reinterpret_cast<void*>(&hk_get_raw_buffer), reinterpret_cast<void**>(&g_get_raw_buffer));
        log("nativepad: protected exe: the game's own controller reads followed through inline hooks (no DualShock 4 "
            "identity)%s", dualsense_as_ds4 ? "; DualSenseAsDualShock4=1 ignored" : "");
        return;
    }
    g_suppression = suppression;
    g_as_ds4 = dualsense_as_ds4;
    g_as_ds4_learned = dualsense_as_ds4 && learned;
    g_follow = true;  // DualSense handles are always followed: does the game read the one it opens?
    hooks::add_with_pad_library(nullptr, "CreateFileW", reinterpret_cast<void*>(&hk_create_file), reinterpret_cast<void**>(&g_create_file));
    hooks::add_with_pad_library(nullptr, "CreateFileA", reinterpret_cast<void*>(&hk_create_file_a), reinterpret_cast<void**>(&g_create_file_a));
    if (dualsense_as_ds4) {
        auto add = [](const char* function, void* hook, void** slot) { hooks::add_with_pad_library(nullptr, function, hook, slot); };
        add("HidD_GetAttributes", reinterpret_cast<void*>(&hk_get_attributes), reinterpret_cast<void**>(&g_get_attributes));
        add("HidD_GetProductString", reinterpret_cast<void*>(&hk_get_product), reinterpret_cast<void**>(&g_get_product));
        add("HidD_GetFeature", reinterpret_cast<void*>(&hk_get_feature), reinterpret_cast<void**>(&g_get_feature));
        add("HidD_SetFeature", reinterpret_cast<void*>(&hk_set_feature), reinterpret_cast<void**>(&g_set_feature));
        add("HidD_GetPreparsedData", reinterpret_cast<void*>(&hk_get_preparsed), reinterpret_cast<void**>(&g_get_preparsed));
        add("HidD_FreePreparsedData", reinterpret_cast<void*>(&hk_free_preparsed), reinterpret_cast<void**>(&g_free_preparsed));
        add("HidP_GetCaps", reinterpret_cast<void*>(&hk_get_caps), reinterpret_cast<void**>(&g_get_caps));
        add("HidP_GetValueCaps", reinterpret_cast<void*>(&hk_get_value_caps), reinterpret_cast<void**>(&g_get_value_caps));
        add("WriteFile", reinterpret_cast<void*>(&hk_write_file), reinterpret_cast<void**>(&g_write_file));
    }
    if (g_follow) {
        hooks::add_with_pad_library(nullptr, "ReadFile", reinterpret_cast<void*>(&hk_read_file), reinterpret_cast<void**>(&g_read_file));
        hooks::add_with_pad_library(nullptr, "GetOverlappedResult", reinterpret_cast<void*>(&hk_get_overlapped),
                                    reinterpret_cast<void**>(&g_get_overlapped));
        hooks::add_with_pad_library(nullptr, "GetOverlappedResultEx", reinterpret_cast<void*>(&hk_get_overlapped_ex),
                                    reinterpret_cast<void**>(&g_get_overlapped_ex));
        hooks::add_with_pad_library(nullptr, "CloseHandle", reinterpret_cast<void*>(&hk_close_handle), reinterpret_cast<void**>(&g_close_handle));
    }
    log("settings: NativeFlickStick=%d (%s)", suppression ? 1 : 0,
        suppression ? "flick stick also with the game's own PlayStation controller support, once its reports are checked"
                    : "flick stick only while the controller is shown as an Xbox one");
    log("settings: DualSenseAsDualShock4=%d%s%s", dualsense_as_ds4 ? 1 : 0,
        dualsense_as_ds4 ? " (a DualSense, USB or Bluetooth, is shown to the game's own PlayStation support as a USB DualShock 4)" : "",
        g_as_ds4_learned ? ", learned by Gyro4All in an earlier session" : "");
}

void refuse_pads(bool refuse) { g_refuse = refuse; }

void set_neutral(bool neutral) {
    if (g_neutral.exchange(neutral) == neutral || !g_device_count.load()) return;
    log("nativepad: the game's controller reports %s (%u made idle so far)",
        neutral ? "made idle (GyroLib's panel has the controller)" : "passed through again", g_neutralised.load());
}

uint16_t hid_path_id(const wchar_t* path, const wchar_t* key) { return path ? path_id(path, key) : 0; }

namespace {

// The game opened a DualSense (as itself or as a DualShock 4), closed it again unread and has none open 5 s later.
bool dualsense_closed_unread() {
    if (g_dualsense_read) return false;
    const ULONGLONG opened = g_dualsense_opened;
    if (!opened || GetTickCount64() - opened < 5000) return false;
    for (Device& d : g_devices)  // a DualSense handle still open: the game took it (it may read it later)
        if (d.handle.load(std::memory_order_acquire) && is_dualsense(d.vendor, d.product)) return false;
    return true;
}

}  // namespace

bool dualsense_ignored() { return !g_as_ds4 && dualsense_closed_unread(); }

bool learned_ds4_rejected() { return g_as_ds4_learned && dualsense_closed_unread(); }

bool opened_vendor(uint16_t vendor) {
    if (!vendor) return false;
    for (auto& v : g_vendors)
        if (v.load() == vendor) return true;
    return false;
}

namespace {

// Long-press blocking changes the selected controller's buttons in the game's reports once its button bytes agree
// with SDL's view of the buttons: >= 10 compared reads with a button down (and >= 4x the misses); 30+ misses
// dominating -> never for that controller. buttons = the newest report's buttons (-1 = none fresh).
void check_buttons(SDL_Gamepad* pad, uint32_t ids, int32_t buttons, bool reading) {
    static uint32_t checked = 0;
    static int matches = 0, misses = 0;
    static bool verified = false, mismatch = false;
    if (ids != checked) {
        checked = ids;
        matches = misses = 0;
        verified = mismatch = false;
    }
    if (pad && buttons >= 0 && !verified && !mismatch) {
        uint32_t sdl = 0;
        for (int n = 0; n < 32 && n < int(SDL_GAMEPAD_BUTTON_COUNT); ++n)
            if (SDL_GetGamepadButton(pad, SDL_GamepadButton(n))) sdl |= 1u << n;
        sdl &= kCheckedButtons;
        const uint32_t report = uint32_t(buttons) & kCheckedButtons;
        if (sdl | report) {
            if (sdl == report) ++matches;
            else ++misses;
            if (matches >= 10 && matches >= 4 * misses) {
                verified = true;
                log("nativepad: %04X:%04X: the buttons in the game's reports match SDL (%d of %d samples)", ids >> 16,
                    ids & 0xFFFF, matches, matches + misses);
            } else if (misses >= 30 && misses > matches) {
                mismatch = true;
                log("nativepad: %04X:%04X: the buttons in the game's reports don't match SDL (report %08X, SDL %08X; "
                    "%d of %d samples off): no long-press blocking for it",
                    ids >> 16, ids & 0xFFFF, report, sdl, misses, matches + misses);
            }
        }
    }
    const bool ok = reading && verified;
    if (g_buttons_ok.exchange(ok) != ok)
        log(ok ? "nativepad: long-press blocking available (buttons in the game's own controller reports)"
               : "nativepad: long-press blocking unavailable: the game stopped reading the controller");
}

}  // namespace

bool set_buttons(uint32_t hide, uint32_t tap) {
    g_hide = hide;
    g_tap = tap;
    return g_buttons_ok;
}

void set_stick(float x, float y) {
    g_stick_x.store(x, std::memory_order_relaxed);
    g_stick_y.store(y, std::memory_order_relaxed);
}

bool frame(SDL_Gamepad* pad, bool suppress) {
    const uint32_t ids = pad ? uint32_t(SDL_GetGamepadVendor(pad)) << 16 | SDL_GetGamepadProduct(pad) : 0;
    g_pad_ids = ids;


    // The layout is trusted once the right stick in the game's reports agrees with SDL's while the stick is held
    // away from the centre; a clear disagreement turns suppression off for this controller.
    static uint32_t checked = 0;
    static int matches = 0, misses = 0;
    static bool verified = false, mismatch = false, available = false;
    static int transitions = 0;
    if (ids != checked) {
        checked = ids;
        matches = misses = 0;
        verified = mismatch = false;
    }
    // The newest report of the selected controller the game read.
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = 0;
    int32_t stick = -1, buttons = -1;
    if (ids)
        for (Device& d : g_devices) {
            if (!d.handle.load(std::memory_order_acquire) || (uint32_t(d.vendor) << 16 | d.product) != ids) continue;
            const ULONGLONG t = d.last_report;
            if (t > last) {
                last = t;
                stick = d.stick;
                buttons = d.buttons;
            }
        }
    const bool reading = last && now - last < 2000;
    check_buttons(pad, ids, reading && now - last < 100 ? buttons : -1, reading);
    if (g_suppression && ids) {
        if (reading && !verified && !mismatch && stick >= 0 && now - last < 100) {
            const int sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            if (std::abs(sx) > 12000 || std::abs(sy) > 12000) {
                const int rx = stick & 0xFF, ry = stick >> 8 & 0xFF;
                const int ex = (sx + 32768) / 257, ey = (sy + 32768) / 257;  // SDL: byte * 257 - 32768
                if (std::abs(rx - ex) < 48 && std::abs(ry - ey) < 48) ++matches;
                else ++misses;
                if (matches >= 15 && matches >= 4 * misses) {
                    verified = true;
                    log("nativepad: %04X:%04X: the right stick in the game's reports matches SDL (%d of %d samples)",
                        ids >> 16, ids & 0xFFFF, matches, matches + misses);
                } else if (misses >= 30 && misses > matches) {
                    mismatch = true;
                    log("nativepad: %04X:%04X: the right stick in the game's reports doesn't match SDL (report %d,%d, "
                        "SDL %d,%d; %d of %d samples off): no native suppression for it",
                        ids >> 16, ids & 0xFFFF, rx, ry, ex, ey, misses, matches + misses);
                }
            }
        }
    }
    const bool now_available = reading && verified;
    if (now_available != available) {
        available = now_available;
        log(available ? "nativepad: native right-stick suppression available (flick stick with the game's own support)"
                      : "nativepad: native right-stick suppression unavailable: the game stopped reading the controller");
    }
    const bool want = suppress && available;
    if (want != g_suppress.load()) {
        g_suppress = want;
        if (++transitions <= 10)
            log("nativepad: right stick %s in the game's reports (%u reports changed so far)%s",
                want ? "centred" : "passed through", g_changed.load(), transitions == 10 ? "; further changes not logged" : "");
    }
    return available;
}

}  // namespace gyro4all::nativepad
