#pragma once
// Dependency check before gyrolib.dll is loaded. GyroLib links the Visual C++ runtime as DLLs (MSVCP140,
// VCRUNTIME140...) and delay-loads SDL3.dll. Windows shares a DLL by name within a process, so when the game has
// already loaded its own copy, or ships one next to its exe (searched before System32), GyroLib gets that copy.
// A Visual C++ runtime older than 14.40 crashes GyroLib's first std::mutex lock (Severed Steel ships 14.24): then
// GyroLib must not be loaded at all. Each finding is logged (`deps:` lines); a blocking one also shows a notice.
#include <string>

namespace gyro4all::deps {

// Returns false when loading gyrolib_path would crash the game: the gyro stays off for this session.
bool check_gyrolib(const std::wstring& gyrolib_path);

}  // namespace gyro4all::deps
