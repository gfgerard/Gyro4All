// Games that make XInput devices only of the DirectInput controllers WMI calls XInput (Microsoft's IsXInputDevice
// sample: Battlefield 4, 007 First Light, MODLOG 2026-10-29) never poll a controller Gyro4All shows as XInput. The
// sample reads the DeviceID of every Win32_PNPEntity, takes those with "IG_" and compares their VID/PID with the
// DirectInput product GUID's Data1. Here, once the game creates a WbemLocator, the property reader of WMI's in-process
// objects (fastprox.dll's CWbemObject::Get, slot 4 of IWbemClassObject, the same for classes and instances) is hooked
// inline, found from a class object of Gyro4All's own. Not the vtables of the locator, the services or the
// enumerators: the Steam overlay patches those itself, and two patchers of one slot ended up calling each other
// (007 First Light crashed at start, stack overflow). The DeviceID of each HID game controller (usage page 1,
// joystick, gamepad or multi-axis) without "IG_" reads as an XInput one: "HID\VID_xxxx&PID_xxxx&IG_00\<instance>"
// (also for Bluetooth ones, whose "_VID&0002xxxx" the sample can't parse). The game's own check then makes it an
// XInput device, whatever the engine. PlayStation controllers are left alone when the game reads them itself (Sony's
// pad library, auto mode).
#include "xinput_wmi.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "xinput.hpp"

#include <windows.h>
#include <hidsdi.h>
#include <intrin.h>
#include <oleauto.h>
#include <setupapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace gyro4all::xinput {

namespace {

using CoCreate_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using Get_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, long, VARIANT*, long*, long*);

constexpr int kGet = 4;  // IWbemClassObject::Get's vtable slot
constexpr CLSID kWbemLocator = {0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
constexpr CLSID kWbemClassObject = {0x9A653086, 0x174F, 0x11D2, {0xB5, 0xF9, 0x00, 0x10, 0x4B, 0x70, 0x3E, 0xFD}};
constexpr IID kIWbemClassObject = {0xDC12A681, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
constexpr ULONGLONG kPadsMs = 2000;  // the controller list is read again after this long
constexpr GUID kHidInterface = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
constexpr uint16_t kSony = 0x054C;

CoCreate_t g_co_create = nullptr;
Get_t g_get = nullptr;  // the trampoline to CWbemObject::Get
std::atomic<bool> g_get_tried{false};
bool g_sony_too = false;  // XInput=2: the game's own access to PlayStation controllers is refused

std::mutex g_mutex;
std::set<uint32_t> g_pads;     // vendor << 16 | product of the HID game controllers without "IG_"
ULONGLONG g_pads_at = 0;       // when g_pads was read (0 never)
std::set<uint32_t> g_logged;   // controllers whose DeviceID was shown with "IG_" (logged once each)

// The module is Gyro4All, GyroLib or GyroLib's SDL3.dll.
bool ours(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ours), &self);
    return m && (m == self || m == GetModuleHandleW(L"gyrolib.dll") || m == GetModuleHandleW(L"SDL3.dll"));
}

// The HID game controllers present, from SetupAPI and hid.dll (their exports, not the game's hooked imports); those
// whose path has "IG_" are XInput ones already.
std::set<uint32_t> list_pads() {
    using GetClassDevs_t = HDEVINFO(WINAPI*)(const GUID*, PCWSTR, HWND, DWORD);
    using EnumInterfaces_t = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
    using InterfaceDetail_t = BOOL(WINAPI*)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W,
                                            DWORD, PDWORD, PSP_DEVINFO_DATA);
    using DestroyList_t = BOOL(WINAPI*)(HDEVINFO);
    using GetAttributes_t = BOOLEAN(__stdcall*)(HANDLE, PHIDD_ATTRIBUTES);
    using GetPreparsedData_t = BOOLEAN(__stdcall*)(HANDLE, PHIDP_PREPARSED_DATA*);
    using FreePreparsedData_t = BOOLEAN(__stdcall*)(PHIDP_PREPARSED_DATA);
    using GetCaps_t = NTSTATUS(__stdcall*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
    std::set<uint32_t> out;
    HMODULE setup = LoadLibraryW(L"setupapi.dll"), hid = LoadLibraryW(L"hid.dll");
    if (!setup || !hid) return out;
    auto get_devs = reinterpret_cast<GetClassDevs_t>(GetProcAddress(setup, "SetupDiGetClassDevsW"));
    auto enum_ifs = reinterpret_cast<EnumInterfaces_t>(GetProcAddress(setup, "SetupDiEnumDeviceInterfaces"));
    auto detail = reinterpret_cast<InterfaceDetail_t>(GetProcAddress(setup, "SetupDiGetDeviceInterfaceDetailW"));
    auto destroy = reinterpret_cast<DestroyList_t>(GetProcAddress(setup, "SetupDiDestroyDeviceInfoList"));
    auto get_attributes = reinterpret_cast<GetAttributes_t>(GetProcAddress(hid, "HidD_GetAttributes"));
    auto get_data = reinterpret_cast<GetPreparsedData_t>(GetProcAddress(hid, "HidD_GetPreparsedData"));
    auto free_data = reinterpret_cast<FreePreparsedData_t>(GetProcAddress(hid, "HidD_FreePreparsedData"));
    auto get_caps = reinterpret_cast<GetCaps_t>(GetProcAddress(hid, "HidP_GetCaps"));
    if (!get_devs || !enum_ifs || !detail || !destroy || !get_attributes || !get_data || !free_data || !get_caps) return out;
    HDEVINFO set = get_devs(&kHidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{sizeof iface};
    for (DWORD i = 0; enum_ifs(set, nullptr, &kHidInterface, i, &iface); ++i) {
        alignas(8) uint8_t buffer[1024];
        auto* d = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!detail(set, &iface, d, sizeof buffer, nullptr, nullptr)) continue;
        std::wstring path = d->DevicePath;
        for (wchar_t& c : path) c = wchar_t(std::towupper(c));
        if (path.find(L"IG_") != std::wstring::npos) continue;
        HANDLE h = CreateFileW(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{sizeof attributes};
        PHIDP_PREPARSED_DATA data = nullptr;
        HIDP_CAPS caps{};
        if (get_attributes(h, &attributes) && get_data(h, &data)) {
            if (get_caps(data, &caps) != HIDP_STATUS_SUCCESS) caps = {};
            free_data(data);
        }
        CloseHandle(h);
        if (caps.UsagePage != 0x01 || (caps.Usage != 0x04 && caps.Usage != 0x05 && caps.Usage != 0x08)) continue;
        if (attributes.VendorID == kSony && !g_sony_too && sony_library()) continue;
        out.insert(uint32_t(attributes.VendorID) << 16 | attributes.ProductID);
    }
    destroy(set);
    return out;
}

// The last four hex digits after "VID" / "PID" and one separator ('_' or '&'): "VID_057E" (USB), "VID&0002057E"
// (Bluetooth). -1 if absent.
int hex_after(const std::wstring& id, const wchar_t* key) {
    const size_t at = id.find(key);
    if (at == std::wstring::npos || at + 4 >= id.size() || (id[at + 3] != L'_' && id[at + 3] != L'&')) return -1;
    size_t end = at + 4;
    while (end < id.size() && std::iswxdigit(id[end])) ++end;
    if (end - (at + 4) < 4) return -1;
    return int(std::wcstoul(id.substr(end - 4, 4).c_str(), nullptr, 16));
}

// oleaut32's BSTR functions, from the module WMI already loaded (no import of Gyro4All's own).
struct Bstr {
    using Len_t = UINT(WINAPI*)(BSTR);
    using Alloc_t = BSTR(WINAPI*)(const OLECHAR*, UINT);
    using Free_t = void(WINAPI*)(BSTR);
    Len_t len = nullptr;
    Alloc_t alloc = nullptr;
    Free_t free = nullptr;
    Bstr() {
        if (HMODULE m = GetModuleHandleW(L"oleaut32.dll")) {
            len = reinterpret_cast<Len_t>(GetProcAddress(m, "SysStringLen"));
            alloc = reinterpret_cast<Alloc_t>(GetProcAddress(m, "SysAllocStringLen"));
            free = reinterpret_cast<Free_t>(GetProcAddress(m, "SysFreeString"));
        }
    }
    bool ok() const { return len && alloc && free; }
};

HRESULT STDMETHODCALLTYPE hk_get(void* object, LPCWSTR name, long flags, VARIANT* value, long* type, long* flavor) {
    const HRESULT hr = g_get(object, name, flags, value, type, flavor);
    if (FAILED(hr) || !value || value->vt != VT_BSTR || !value->bstrVal || !name || _wcsicmp(name, L"DeviceID") != 0 ||
        ours(_ReturnAddress()) || !translates())
        return hr;
    static const Bstr bstr;
    if (!bstr.ok()) return hr;
    std::wstring id(value->bstrVal, bstr.len(value->bstrVal));
    for (wchar_t& c : id) c = wchar_t(std::towupper(c));
    if (id.rfind(L"HID\\", 0) != 0 || id.find(L"IG_") != std::wstring::npos) return hr;
    const int vendor = hex_after(id, L"VID"), product = hex_after(id, L"PID");
    if (vendor < 0 || product < 0) return hr;
    const uint32_t key = uint32_t(vendor) << 16 | uint32_t(product);
    bool first = false;
    {
        std::lock_guard lock(g_mutex);
        const ULONGLONG now = GetTickCount64();
        if (!g_pads_at || now - g_pads_at > kPadsMs) {
            g_pads = list_pads();
            g_pads_at = now;
        }
        if (!g_pads.count(key)) return hr;
        first = g_logged.insert(key).second;
    }
    // HID\<hardware id>\<instance>: the hardware id rebuilt in the USB form, with the XInput interface's "IG_00".
    const size_t instance = id.find(L'\\', 4);
    wchar_t head[64];
    std::swprintf(head, std::size(head), L"HID\\VID_%04X&PID_%04X&IG_00", vendor, product);
    const std::wstring shown = head + (instance == std::wstring::npos ? std::wstring() : std::wstring(value->bstrVal + instance));
    BSTR text = bstr.alloc(shown.data(), UINT(shown.size()));
    if (!text) return hr;
    bstr.free(value->bstrVal);
    value->bstrVal = text;
    if (first)
        log("xinput: WMI: the game reads controller %04X:%04X's DeviceID: shown as an XInput one (\"IG_\")", vendor, product);
    listed_as_xinput(uint16_t(vendor), uint16_t(product), false);
    return hr;
}

// CWbemObject::Get, from a class object of our own (in process, no WMI connection), hooked inline once.
void hook_get(CoCreate_t real) {
    if (g_get_tried.exchange(true)) return;
    void* object = nullptr;
    const HRESULT hr = real(kWbemClassObject, nullptr, CLSCTX_INPROC_SERVER, kIWbemClassObject, &object);
    if (FAILED(hr) || !object) {
        log("xinput: WMI: no class object of our own (0x%08lX): DeviceID not hooked", static_cast<unsigned long>(hr));
        return;
    }
    void* get = (*static_cast<void***>(object))[kGet];
    static_cast<IUnknown*>(object)->Release();
    const bool ok = hooks::hook_function(get, reinterpret_cast<void*>(&hk_get), reinterpret_cast<void**>(&g_get));
    log("xinput: WMI: IWbemClassObject::Get %s", ok ? "hooked" : "could NOT be hooked");
}

HRESULT WINAPI hk_co_create(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out) {
    // Import slots: the original as patched, else (a delay-load not resolved yet) from combase.dll itself.
    CoCreate_t real =
        reinterpret_cast<CoCreate_t>(InterlockedCompareExchangePointer(reinterpret_cast<void**>(&g_co_create), nullptr, nullptr));
    if (!real)
        if (HMODULE m = LoadLibraryW(L"combase.dll")) real = reinterpret_cast<CoCreate_t>(GetProcAddress(m, "CoCreateInstance"));
    const HRESULT hr = real ? real(clsid, outer, context, iid, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && IsEqualCLSID(clsid, kWbemLocator) && !ours(_ReturnAddress())) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log("xinput: WMI: the game creates a WbemLocator (an XInput device check, likely)");
        hook_get(real);
    }
    return hr;
}

}  // namespace

void install_wmi(bool sony_too) {
    g_sony_too = sony_too;
    // Protected exes: inline on combase.dll's export (ole32's forwards there), else the game modules' imports.
    if (hooks::inline_mode()) LoadLibraryW(L"combase.dll");
    const bool ok = hooks::add(hooks::inline_mode() ? "combase.dll" : nullptr, "CoCreateInstance",
                               reinterpret_cast<void*>(&hk_co_create), reinterpret_cast<void**>(&g_co_create));
    log("xinput: WMI DeviceID hook %s", ok ? "installed" : "registered (no game module imports CoCreateInstance yet)");
}

}  // namespace gyro4all::xinput
