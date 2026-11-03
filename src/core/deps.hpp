#pragma once
// Dependency check before gyrolib.dll is loaded. GyroLib links the Visual C++ runtime as DLLs (MSVCP140,
// VCRUNTIME140...) and delay-loads SDL3.dll. Windows shares a DLL by name within a process, so when the game has
// already loaded its own copy, or ships one next to its exe (searched before System32), GyroLib gets that copy.
// A Visual C++ runtime older than 14.40 crashes GyroLib's first std::mutex lock (Severed Steel ships 14.24): then
// GyroLib must not be loaded at all. Each finding is logged (`deps:` lines); a blocking one also shows a notice.
#include <windows.h>

#include <string>

namespace gyro4all::deps {

// Returns false when loading gyrolib_path would crash the game: the gyro stays off for this session.
bool check_gyrolib(const std::wstring& gyrolib_path);

// Call once gyrolib.dll is loaded, before GyroLib starts SDL. SDL names each thread it starts with SetThreadDescription
// and also raises the old debugger exception 0x406D1388, which its own vectored handler is meant to swallow. Some games'
// crash reporters see it anyway and show an error report while the game runs on (Onimusha: Way of the Sword): SDL3.dll's
// RaiseException import skips that code, patched as soon as GyroLib loads SDL3.
void guard_sdl_thread_names(HMODULE gyrolib);

}  // namespace gyro4all::deps
