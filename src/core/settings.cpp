#include "settings.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

namespace gyro4all::settings {
namespace {

std::wstring g_mod_dir, g_install_dir, g_exe_stem, g_settings_path, g_profile_path;

// Gyro4All\Gyro4All.ini: settings for every game in this folder, created with the defaults on the first run and never
// overwritten (releases don't ship it, so updates keep the user's values). The engine's block goes after [Gyro4All].
const char kHeader[] =
    "; Gyro4All settings for every game in this folder. Created on the first run; edit freely, it is never overwritten.\r\n"
    "; A game's profile (Gyro4All\\profiles\\<game exe>.ini) can override any of these in its own [Gyro4All] section.\r\n"
    "\r\n"
    "[Gyro4All]\r\n";
const char kCoreSettings[] =
    "; 0 = no on-screen notices (they still go to Gyro4All.log).\r\n"
    "Notices=1\r\n"
    "\r\n"
    "\r\n"
    "; Sensor-time pacing: each frame turns by the controller's motion over that frame's own length, instead of by the\r\n"
    ";     reports that happened to arrive in it (an uneven turn at a steady hand). 0 = off. Applies on save.\r\n"
    ";     SensorPacingDelayUs: how far behind the frame the motion is played (microseconds); -1 = automatic (the usual\r\n"
    ";     lag of the last report, so the average latency stays as without pacing), 0 = none (the last rate carried\r\n"
    ";     forward to the frame time). The values below are the defaults: remove the ; to change.\r\n"
    ";SensorPacing=1\r\n"
    ";SensorPacingDelayUs=-1\r\n"
    "\r\n"
    "\r\n"
    "; Show a non-Xbox controller (DualSense, Switch Pro, DirectInput pads) to the game as an Xbox (XInput) controller, for\r\n"
    "; games that only support Xbox controllers without Steam Input. 0 = off, 1 = auto (only when the game doesn't read\r\n"
    "; the controller itself and no Xbox/Steam Input/DS4Windows controller is present), 2 = always (unless the\r\n"
    "; controller already is an Xbox one). Restart the game after changing it.\r\n"
    "XInput=1\r\n"
    "\r\n"
    "\r\n"
    "; Face buttons: 0 = by position (bottom = Xbox A), 1 = swap A<->B and X<->Y (Nintendo A = Xbox A; any controller).\r\n"
    "XInputNintendoLabels=0\r\n"
    "\r\n"
    "\r\n"
    "; 1 = the DualSense touchpad click also presses View (Back), like the Create button. 0 = the game ignores it.\r\n"
    "XInputTouchpadBack=1\r\n"
    "\r\n"
    "\r\n"
    "; 1 = GyroLib's flick stick (F10 panel) also in games with their own PlayStation controller support: Gyro4All centres\r\n"
    ";     the right stick in what the game reads while flick stick is on, keeping PlayStation icons, haptics and\r\n"
    ";     adaptive triggers. Offered after you move the right stick once in game. Needs XInput=1 (with XInput=2 the\r\n"
    ";     game doesn't read the controller itself; flick stick then works through the Xbox controller). 0 = off.\r\n"
    "NativeFlickStick=0\r\n"
    "\r\n"
    "\r\n"
    "; Stick camera, for games whose right-stick turn is too slow even at full deflection: 1 = Gyro4All turns the camera\r\n"
    ";     with the right stick itself (no mouse emulation, so the game keeps its controller prompts)  \r\n"
    ";     Speeds in degrees/s at full deflection; deadzone and edge in %; the speed ramps up to StickEdgeSpeed while\r\n"
    ";     the stick is held past StickEdge for StickEdgeRamp ms. The other Stick* values apply on save; StickCamera itself\r\n"
    ";     needs a restart. 0 = off. The values below are the defaults: remove the ; to change (here or in a game's profile)\r\n"
    ";StickCamera=0\r\n"
    ";StickSpeed=360\r\n"
    ";StickSpeedPitch=200\r\n"
    ";StickDeadzone=10\r\n"
    "; Response exponent: 1 = linear, higher = finer near the centre.\r\n"
    ";StickCurve=2.0\r\n"
    ";StickEdge=90\r\n"
    ";StickEdgeSpeed=720\r\n"
    ";StickEdgeRamp=400\r\n"
    ";StickInvertY=0\r\n"
    "; Buttons that, held +200 ms, disable StickCamera until released (e.g. weapon wheel) comma-separated.\r\n"
    ";     (Xbox or PlayStation: RB = R1) Example: RB,LB\r\n"
    ";StickCameraPassButtons=\r\n"
    "\r\n"
    "\r\n"
    "; Zoom compensation: switch \"Zoom compensation\" on in a tab of the F10 panel (off by default; offered where Gyro4All\r\n"
    ";     reads the game's field of view, Unreal Engine 3 games so far) and the gyro slows down as the game zooms in, so a\r\n"
    ";     scope moves the view on screen about as fast as normal play. It works with the two values below (they apply on\r\n"
    ";     save; the tabs' descriptions in the panel show the ones in use):\r\n"
    "; ZoomReference: your normal field of view in VERTICAL degrees. That tab's sensitivity is your gyro speed there; every\r\n"
    ";     other field of view (scopes, aiming, sprint, other cameras) scales from it, the same whatever you played before.\r\n"
    ";     The log's status line shows the current one (\"field of view 100.0 (vertical 67.7)\": put the vertical number of\r\n"
    ";     your normal view here). Vertical, so it survives a change of resolution or an ultrawide screen. The default 50.5\r\n"
    ";     is 80 degrees horizontal at 16:9.\r\n"
    "; ZoomStrength: how much the field of view changes the speed, in %. 100 = every field of view moves the view on\r\n"
    ";     screen as fast as the normal one; 0 = no change; between = zoomed views feel somewhat faster (75: a 13.6x scope\r\n"
    ";     1.9x, a 3.3x aim 1.35x) and wider ones somewhat slower.\r\n"
    "; The values below are the defaults: remove the ; to change them (here or in a game's profile).\r\n"
    ";ZoomReference=50.5\r\n"
    ";ZoomStrength=100\r\n"
    "\r\n"
    "\r\n"
    "; Sensitivity profiles, all off by default: extra tabs in the F10 panel (ADS, ALT_ADS for games with a second aim, OTHER) \r\n"
    ";     that start with the Exploration settings (ALT_ADS: with the ADS settings); change them there. \r\n"
    ";     A profile needs both its button and its mode (until then its tab says \"- disabled\"). \r\n"
    ";     Mode: Hold = while the button is held, Toggle = from one press to the next.\r\n"
    ";     Buttons: LT RT LB RB LS RS A B X Y (or L2 R2 L1 R1 L3 R3 Cross Circle Square Triangle),\r\n"
    ";     Back Start Up Down Left Right Touchpad Misc LP1 LP2 RP1 RP2 (paddles). Restart the game after a change.\r\n"
    ";     Example: AdsProfileButton=LT and AdsProfileMode=Hold.\r\n"
    "AdsProfileButton=\r\n"
    "AdsProfileMode=\r\n"
    "AltAdsProfileButton=\r\n"
    "AltAdsProfileMode=\r\n"
    "OtherProfileButton=\r\n"
    "OtherProfileMode=\r\n";

}  // namespace

void configure(const std::wstring& mod_dir, const std::wstring& exe_stem, const std::wstring& install_dir) {
    g_mod_dir = mod_dir;
    g_install_dir = install_dir.empty() ? mod_dir : install_dir;
    g_exe_stem = exe_stem;
    g_settings_path = mod_dir + L"\\Gyro4All.ini";
    g_profile_path = mod_dir + L"\\profiles\\" + exe_stem + L".ini";
}

void use_profile_alias(const std::vector<std::wstring>& aliases) {
    if (GetFileAttributesW(g_profile_path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    for (const std::wstring& alias : aliases) {
        const std::wstring path = g_mod_dir + L"\\profiles\\" + alias + L".ini";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        log("settings: no profiles\\%s.ini; using %s.ini (the same game's exe in another store)",
            narrow(g_exe_stem).c_str(), narrow(alias).c_str());
        g_profile_path = path;
        return;
    }
}

void migrate_from_gyroue(const std::wstring& win64_dir) {
    const std::wstring old_dir = win64_dir + L"\\GyroUE";
    const DWORD attributes = GetFileAttributesW(old_dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    CreateDirectoryW((g_mod_dir + L"\\profiles").c_str(), nullptr);
    int copied = 0, kept = 0;
    auto copy = [&](const std::wstring& from, const std::wstring& to) {
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (CopyFileW(from.c_str(), to.c_str(), TRUE)) ++copied;
        else ++kept;  // Gyro4All already has it (e.g. a profile shipped in the release): the newer one stays
    };
    copy(old_dir + L"\\GyroUE.ini", g_settings_path);
    copy(old_dir + L"\\gyrolib.ini", g_mod_dir + L"\\gyrolib.ini");
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((old_dir + L"\\profiles\\*.ini").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                copy(old_dir + L"\\profiles\\" + found.cFileName, g_mod_dir + L"\\profiles\\" + found.cFileName);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    const std::wstring done = old_dir + L".migrated";
    const bool renamed = MoveFileExW(old_dir.c_str(), done.c_str(), 0) != 0;
    log("settings: GyroUE folder found (former name): %d file(s) copied to %s, %d already there; old folder %s", copied,
        narrow(g_mod_dir).c_str(), kept, renamed ? "renamed to GyroUE.migrated" : "could not be renamed (copied again next start)");
}

const std::wstring& mod_dir() { return g_mod_dir; }
const std::wstring& install_dir() { return g_install_dir; }
const std::wstring& exe_stem() { return g_exe_stem; }
const std::wstring& settings_path() { return g_settings_path; }
const std::wstring& profile_path() { return g_profile_path; }

void ensure_file(const char* engine_block) {
    if (GetFileAttributesW(g_settings_path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_settings_path.c_str(), L"wb") != 0 || !file) {
        log("settings: could not create %s (defaults used)", narrow(g_settings_path).c_str());
        return;
    }
    const std::string text = std::string(kHeader) + (engine_block ? engine_block : "") + kCoreSettings;
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    log("settings: %s created with the defaults", narrow(g_settings_path).c_str());
}

bool read(const std::wstring& path, const wchar_t* key, std::wstring& value) {
    wchar_t buffer[512];
    for (const wchar_t* section : {kSection, kLegacySection})
        if (GetPrivateProfileStringW(section, key, L"", buffer, 512, path.c_str()) > 0) {
            value = buffer;
            return true;
        }
    return false;
}

int get(const wchar_t* key, int fallback) {
    std::wstring value;
    for (const std::wstring* path : {&g_profile_path, &g_settings_path})
        if (read(*path, key, value)) return _wtoi(value.c_str());
    return fallback;
}

std::string get_string(const wchar_t* key, const wchar_t* fallback) {
    std::wstring value;
    for (const std::wstring* path : {&g_profile_path, &g_settings_path})
        if (read(*path, key, value)) return narrow(value);
    return narrow(fallback);
}

std::string profile_setting(const wchar_t* key) {
    std::wstring value;
    return read(g_profile_path, key, value) ? narrow(value) : std::string{};
}

std::string profile_string(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(section, key, fallback, buffer, 2048, g_profile_path.c_str());
    return narrow(buffer);
}

int profile_int(const wchar_t* section, const wchar_t* key, int fallback) {
    return int(GetPrivateProfileIntW(section, key, fallback, g_profile_path.c_str()));
}

}  // namespace gyro4all::settings
