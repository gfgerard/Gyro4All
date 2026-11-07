#pragma once
// ProbeKey=: the key that starts and cancels an engine's camera probe during play.
#include "log.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace gyro4all {

struct ProbeKey {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
    std::string name;
};

inline bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// F1..F24, optionally after Ctrl+, Shift+ and/or Alt+ (case-insensitive). Empty or unknown = no key.
inline ProbeKey parse_probe_key(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    ProbeKey k;
    for (const std::string& part : split(text, '+')) {
        if (part == "CTRL" || part == "CONTROL") k.ctrl = true;
        else if (part == "SHIFT") k.shift = true;
        else if (part == "ALT") k.alt = true;
        else if (part.size() >= 2 && part[0] == 'F' && std::atoi(part.c_str() + 1) >= 1 && std::atoi(part.c_str() + 1) <= 24)
            k.vk = VK_F1 + std::atoi(part.c_str() + 1) - 1;
        else return {};
    }
    if (!k.vk) return {};
    k.name = std::string(k.ctrl ? "Ctrl+" : "") + (k.shift ? "Shift+" : "") + (k.alt ? "Alt+" : "") + "F" +
             std::to_string(k.vk - VK_F1 + 1);
    return k;
}

// ProbeKey= as written: the game's profile wins over Gyro4All.ini, [Gyro4All] over [GyroUE]. Present but empty = no
// key (settings::get_string can't tell that from missing); missing everywhere = Ctrl+F7.
inline std::string probe_key_setting() {
    for (const std::wstring* path : {&settings::profile_path(), &settings::settings_path()})
        for (const wchar_t* section : {settings::kSection, settings::kLegacySection}) {
            wchar_t value[64];
            GetPrivateProfileStringW(section, L"ProbeKey", L"\x01", value, 64, path->c_str());
            if (value[0] != L'\x01') return narrow(value);
        }
    return "Ctrl+F7";
}

// The key went down now (with exactly its modifiers, the game focused). down: its state last frame, updated.
inline bool probe_key_pressed(const ProbeKey& k, bool focused, bool& down) {
    const bool now = k.vk && focused && key_held(k.vk) && k.ctrl == key_held(VK_CONTROL) &&
                     k.shift == key_held(VK_SHIFT) && k.alt == key_held(VK_MENU);
    const bool pressed = now && !down;
    down = now;
    return pressed;
}

}  // namespace gyro4all
