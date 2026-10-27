#include "deps.hpp"

#include "log.hpp"
#include "notify.hpp"

#include <windows.h>

#include <SDL3/SDL_version.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace gyro4all::deps {
namespace {

struct Version {
    uint16_t major = 0, minor = 0, build = 0, revision = 0;
    uint32_t major_minor() const { return uint32_t(major) << 16 | minor; }
    std::string text() const { return std::format("{}.{}.{}.{}", major, minor, build, revision); }
};

constexpr uint32_t version_key(uint16_t major, uint16_t minor) { return uint32_t(major) << 16 | minor; }

// The VC++ runtime that binaries built with MSVC 14.40+ need at least: older MSVCP140 can't lock their std::mutex
// (constexpr constructor change, VS 2022 17.10) and crashes on the first lock.
constexpr uint32_t kMutexSafeRuntime = version_key(14, 40);

// VS_FIXEDFILEINFO from the module's version resource. Not GetFileVersionInfo: that lives in version.dll, which
// Gyro4All itself may be installed as.
bool module_version(HMODULE m, Version& v) {
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));  // VS_VERSION_INFO, RT_VERSION
    if (!r) return false;
    const auto* p = static_cast<const uint8_t*>(LockResource(LoadResource(m, r)));
    const DWORD n = SizeofResource(m, r);
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {
        VS_FIXEDFILEINFO f;
        std::memcpy(&f, p + i, sizeof(f));
        if (f.dwSignature != 0xFEEF04BD) continue;
        v = {HIWORD(f.dwFileVersionMS), LOWORD(f.dwFileVersionMS), HIWORD(f.dwFileVersionLS), LOWORD(f.dwFileVersionLS)};
        return true;
    }
    return false;
}

bool file_version(const std::wstring& path, Version& v) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!m) return false;
    const bool ok = module_version(m, v);
    FreeLibrary(m);
    return ok;
}

// gyrolib.dll's linker version and DLL imports (regular and delay-load), read from the file without running it.
bool read_image(const std::wstring& path, uint32_t& linker, std::vector<std::string>& imports) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(m) & ~uintptr_t(3));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    bool ok = dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
              nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    if (ok) {
        linker = version_key(nt->OptionalHeader.MajorLinkerVersion, nt->OptionalHeader.MinorLinkerVersion);
        const IMAGE_DATA_DIRECTORY* dir = nt->OptionalHeader.DataDirectory;
        if (const DWORD rva = dir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
            for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + rva); d->Name; ++d)
                imports.emplace_back(reinterpret_cast<const char*>(base + d->Name));
        if (const DWORD rva = dir[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
            for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + rva); d->DllNameRVA; ++d)
                imports.emplace_back(reinterpret_cast<const char*>(base + d->DllNameRVA));
    }
    FreeLibrary(m);
    return ok;
}

std::string lower(std::string s) {  // DLL names are ASCII
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // DLL names are ASCII

std::wstring folder_of(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The copy of `name` that GyroLib will get: one already loaded in the process wins (DLLs are shared by name),
// else the game exe's folder (searched before System32), else System32.
struct Source {
    std::wstring path;
    bool loaded = false, system = false;
};

Source resolve(const std::string& name) {
    wchar_t buffer[MAX_PATH];
    const UINT n = GetSystemDirectoryW(buffer, MAX_PATH);
    const std::wstring system_dir(buffer, n);
    Source s;
    if (HMODULE m = GetModuleHandleW(widen(name).c_str())) {
        const DWORD len = GetModuleFileNameW(m, buffer, MAX_PATH);
        s.path.assign(buffer, len);
        s.loaded = true;
    } else {
        const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        const std::wstring local = folder_of(std::wstring(buffer, len)) + L"\\" + widen(name);
        if (exists(local)) s.path = local;
        else if (exists(system_dir + L"\\" + widen(name))) s.path = system_dir + L"\\" + widen(name);
    }
    s.system = !s.path.empty() && lstrcmpiW(folder_of(s.path).c_str(), system_dir.c_str()) == 0;
    return s;
}

std::string where(const Source& s) {
    return std::format("{}{}", s.system ? "System32" : narrow(s.path), s.loaded ? ", already loaded" : "");
}

}  // namespace

bool check_gyrolib(const std::wstring& gyrolib_path) {
    uint32_t linker = 0;
    std::vector<std::string> imports;
    if (!read_image(gyrolib_path, linker, imports)) {
        log("deps: can't read %s's imports, not checked", narrow(gyrolib_path).c_str());
        return true;  // the load itself will tell
    }
    const std::string built = std::format("{}.{}", linker >> 16, linker & 0xFFFF);
    std::vector<std::string> blocking;  // runtime DLLs that would crash GyroLib
    std::wstring blocking_folder;
    bool blocking_system = false;
    for (const std::string& name : imports) {
        const std::string l = lower(name);
        const bool vc = l.starts_with("msvcp") || l.starts_with("vcruntime") || l.starts_with("concrt");
        if (!vc && l != "sdl3.dll") continue;
        const Source s = resolve(name);
        if (l == "sdl3.dll") {
            // GyroLib loads its own SDL3 from its cache, unless the game already has one: then it gets the game's.
            if (!s.loaded) continue;
            Version v;
            const bool known = file_version(s.path, v);
            const bool older = known && version_key(v.major, v.minor) * 1000ull + v.build <
                                            version_key(SDL_MAJOR_VERSION, SDL_MINOR_VERSION) * 1000ull + SDL_MICRO_VERSION;
            log("deps: %s %s (%s): the game's own SDL3; GyroLib will use it instead of its own (built against %d.%d.%d)%s",
                name.c_str(), known ? v.text().c_str() : "(version unknown)", where(s).c_str(), SDL_MAJOR_VERSION,
                SDL_MINOR_VERSION, SDL_MICRO_VERSION, older ? ": OLDER, controller input may misbehave" : "");
            continue;
        }
        if (s.path.empty()) {
            log("deps: %s not found (GyroLib will fail to load: install the Microsoft Visual C++ Redistributable x64)",
                name.c_str());
            continue;
        }
        Version v;
        if (!file_version(s.path, v)) {
            log("deps: %s version unknown (%s)", name.c_str(), where(s).c_str());
            continue;
        }
        if (l.starts_with("msvcp") && linker >= kMutexSafeRuntime && v.major_minor() < kMutexSafeRuntime) {
            log("deps: %s %s (%s): older than 14.40, GyroLib (built with %s) would crash the game", name.c_str(),
                v.text().c_str(), where(s).c_str(), built.c_str());
            blocking.push_back(name);
            if (blocking_folder.empty()) blocking_folder = folder_of(s.path);
            blocking_system |= s.system;
        } else if (v.major_minor() < linker) {
            log("deps: %s %s (%s): older than GyroLib's build tools (%s), not known to break it", name.c_str(),
                v.text().c_str(), where(s).c_str(), built.c_str());
        } else {
            log("deps: %s %s (%s)", name.c_str(), v.text().c_str(), where(s).c_str());
        }
    }
    if (blocking.empty()) return true;

    std::string files;
    for (const std::string& name : blocking) files += (files.empty() ? "" : " and ") + name;
    if (blocking_system) {
        log("deps: gyro disabled for this session. Fix: install the latest Microsoft Visual C++ Redistributable (x64)");
        // One line per idea: notify wraps long lines to the game window's width.
        notify::show(L"Gyro4All: gyro disabled for this session.\n"
                     L"Windows' Visual C++ runtime is too old for GyroLib (it would crash the game).\n"
                     L"Fix: install the latest Microsoft Visual C++ Redistributable (x64), then restart the game.",
                     30);
    } else {
        log("deps: gyro disabled for this session. Fix: rename %s in %s (e.g. add .bak) so the game uses the system's "
            "newer copies, then restart",
            files.c_str(), narrow(blocking_folder).c_str());
        notify::show(L"Gyro4All: gyro disabled for this session.\n"
                     L"This game ships an old Visual C++ runtime that would crash GyroLib.\n"
                     L"Fix: in the game's Binaries\\Win64 folder, rename " +
                         widen(files) + L" (e.g. add .bak), then restart the game. Details in Gyro4All.log.",
                     30);
    }
    return false;
}

}  // namespace gyro4all::deps
