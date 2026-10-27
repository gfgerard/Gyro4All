#pragma once
// Gyro4All\Gyro4All.ini (settings for every game in the folder) and the game's profile (Gyro4All\profiles\<exe stem>.ini),
// whose [Gyro4All] section overrides Gyro4All.ini. Engine modules read their own profile sections through here too.
#include <string>
#include <vector>

namespace gyro4all::settings {

// DllMain, first: the mod folder for everything Gyro4All writes (settings, profiles, logs, dumps) and the game exe's
// stem. install_dir: the Gyro4All folder next to the game, with the DLLs and the shipped profiles; the same folder
// unless the game's folder isn't writable (e.g. EA app installs in Program Files), then mod_dir is in %LOCALAPPDATA%.
void configure(const std::wstring& mod_dir, const std::wstring& exe_stem, const std::wstring& install_dir = {});

// DllMain, after configure: GyroUE (this project's former name) kept its files in <Win64>\GyroUE. Copies the user's
// GyroUE.ini (as Gyro4All.ini), gyrolib.ini and profiles into Gyro4All\, never over an existing file, then renames
// the old folder to GyroUE.migrated so it runs once. Nothing to do without a GyroUE folder.
void migrate_from_gyroue(const std::wstring& win64_dir);

// DllMain, once the engine is known: when profiles\<exe stem>.ini doesn't exist, the first of these stems (the same
// game's exe in other stores, Engine::profile_aliases) that has one becomes the profile.
void use_profile_alias(const std::vector<std::wstring>& aliases);

const std::wstring& mod_dir();
const std::wstring& install_dir();  // gyrolib.dll, gyro4all.dll
const std::wstring& exe_stem();
const std::wstring& settings_path();  // Gyro4All\Gyro4All.ini
const std::wstring& profile_path();   // Gyro4All\profiles\<exe stem>.ini (may not exist)

// Creates Gyro4All.ini with the defaults if it doesn't exist (never overwritten). engine_block: the engine's own
// commented settings, placed first in the [Gyro4All] section.
void ensure_file(const char* engine_block);

// The section of the mod's own settings, in Gyro4All.ini and in profiles. Files from GyroUE (the project's former name)
// have [GyroUE] instead: it is still read, and [Gyro4All] wins when a file has both. New values go to [Gyro4All].
inline constexpr wchar_t kSection[] = L"Gyro4All";
inline constexpr wchar_t kLegacySection[] = L"GyroUE";

// A mod setting from one file ([Gyro4All], else [GyroUE]). False if neither section has it (or it is empty).
bool read(const std::wstring& path, const wchar_t* key, std::wstring& value);

// A mod setting: the game's profile wins over Gyro4All.ini, then the fallback.
int get(const wchar_t* key, int fallback);
std::string get_string(const wchar_t* key, const wchar_t* fallback = L"");
// A mod setting from the game's profile only ("" if it has none).
std::string profile_setting(const wchar_t* key);

// A value from the game's profile.
std::string profile_string(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"");
int profile_int(const wchar_t* section, const wchar_t* key, int fallback);

}  // namespace gyro4all::settings
