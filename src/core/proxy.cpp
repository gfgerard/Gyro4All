// System DLL proxy. Gyro4All ships as winmm.dll (imported by more games than version.dll), but some
// games or tools load the system winmm.dll first (RivaTuner's hooks in Gears of War: E-Day), or another
// mod already owns that name, so the same DLL also exports all of version.dll and can be renamed to
// version.dll. Every export forwards to %SystemRoot%\System32\<this DLL's own file name>; an export the
// real DLL doesn't have fails with ERROR_PROC_NOT_FOUND (nobody imports it under that name anyway).
// All version.dll and winmm.dll functions take at most 8 integer/pointer arguments (no floats), so one
// generic x64 forwarder signature covers them (extra slots read harmlessly from the caller's frame).
#include <windows.h>

namespace {

using Fn = INT_PTR(WINAPI*)(INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR);

HMODULE real_module() {
    static HMODULE module = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&real_module), &self);
        wchar_t own[MAX_PATH];
        GetModuleFileNameW(self, own, MAX_PATH);
        const wchar_t* name = own;
        for (const wchar_t* c = own; *c; ++c)
            if (*c == L'\\' || *c == L'/') name = c + 1;
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        path[n] = L'\\';
        lstrcpynW(path + n + 1, name, MAX_PATH - int(n) - 1);
        return LoadLibraryW(path);  // full path: the system copy, not this proxy
    }();
    return module;
}

Fn resolve(const char* name) { return reinterpret_cast<Fn>(GetProcAddress(real_module(), name)); }

}  // namespace

#define GYRO4ALL_FORWARD(name)                                                                          \
    extern "C" INT_PTR WINAPI proxy_##name(INT_PTR a, INT_PTR b, INT_PTR c, INT_PTR d, INT_PTR e, INT_PTR f, \
                                           INT_PTR g, INT_PTR h) {                                        \
        static const Fn real = resolve(#name);                                                            \
        if (!real) {                                                                                      \
            SetLastError(ERROR_PROC_NOT_FOUND);                                                           \
            return 0;                                                                                     \
        }                                                                                                 \
        return real(a, b, c, d, e, f, g, h);                                                              \
    }

// version.dll
GYRO4ALL_FORWARD(GetFileVersionInfoA)
GYRO4ALL_FORWARD(GetFileVersionInfoByHandle)
GYRO4ALL_FORWARD(GetFileVersionInfoExA)
GYRO4ALL_FORWARD(GetFileVersionInfoExW)
GYRO4ALL_FORWARD(GetFileVersionInfoSizeA)
GYRO4ALL_FORWARD(GetFileVersionInfoSizeExA)
GYRO4ALL_FORWARD(GetFileVersionInfoSizeExW)
GYRO4ALL_FORWARD(GetFileVersionInfoSizeW)
GYRO4ALL_FORWARD(GetFileVersionInfoW)
GYRO4ALL_FORWARD(VerFindFileA)
GYRO4ALL_FORWARD(VerFindFileW)
GYRO4ALL_FORWARD(VerInstallFileA)
GYRO4ALL_FORWARD(VerInstallFileW)
GYRO4ALL_FORWARD(VerLanguageNameA)
GYRO4ALL_FORWARD(VerLanguageNameW)
GYRO4ALL_FORWARD(VerQueryValueA)
GYRO4ALL_FORWARD(VerQueryValueW)

// winmm.dll
GYRO4ALL_FORWARD(CloseDriver)
GYRO4ALL_FORWARD(DefDriverProc)
GYRO4ALL_FORWARD(DriverCallback)
GYRO4ALL_FORWARD(DrvGetModuleHandle)
GYRO4ALL_FORWARD(GetDriverModuleHandle)
GYRO4ALL_FORWARD(OpenDriver)
GYRO4ALL_FORWARD(PlaySound)
GYRO4ALL_FORWARD(PlaySoundA)
GYRO4ALL_FORWARD(PlaySoundW)
GYRO4ALL_FORWARD(SendDriverMessage)
GYRO4ALL_FORWARD(WOWAppExit)
GYRO4ALL_FORWARD(auxGetDevCapsA)
GYRO4ALL_FORWARD(auxGetDevCapsW)
GYRO4ALL_FORWARD(auxGetNumDevs)
GYRO4ALL_FORWARD(auxGetVolume)
GYRO4ALL_FORWARD(auxOutMessage)
GYRO4ALL_FORWARD(auxSetVolume)
GYRO4ALL_FORWARD(joyConfigChanged)
GYRO4ALL_FORWARD(joyGetDevCapsA)
GYRO4ALL_FORWARD(joyGetDevCapsW)
GYRO4ALL_FORWARD(joyGetNumDevs)
GYRO4ALL_FORWARD(joyGetPos)
GYRO4ALL_FORWARD(joyGetPosEx)
GYRO4ALL_FORWARD(joyGetThreshold)
GYRO4ALL_FORWARD(joyReleaseCapture)
GYRO4ALL_FORWARD(joySetCapture)
GYRO4ALL_FORWARD(joySetThreshold)
GYRO4ALL_FORWARD(mciDriverNotify)
GYRO4ALL_FORWARD(mciDriverYield)
GYRO4ALL_FORWARD(mciExecute)
GYRO4ALL_FORWARD(mciFreeCommandResource)
GYRO4ALL_FORWARD(mciGetCreatorTask)
GYRO4ALL_FORWARD(mciGetDeviceIDA)
GYRO4ALL_FORWARD(mciGetDeviceIDFromElementIDA)
GYRO4ALL_FORWARD(mciGetDeviceIDFromElementIDW)
GYRO4ALL_FORWARD(mciGetDeviceIDW)
GYRO4ALL_FORWARD(mciGetDriverData)
GYRO4ALL_FORWARD(mciGetErrorStringA)
GYRO4ALL_FORWARD(mciGetErrorStringW)
GYRO4ALL_FORWARD(mciGetYieldProc)
GYRO4ALL_FORWARD(mciLoadCommandResource)
GYRO4ALL_FORWARD(mciSendCommandA)
GYRO4ALL_FORWARD(mciSendCommandW)
GYRO4ALL_FORWARD(mciSendStringA)
GYRO4ALL_FORWARD(mciSendStringW)
GYRO4ALL_FORWARD(mciSetDriverData)
GYRO4ALL_FORWARD(mciSetYieldProc)
GYRO4ALL_FORWARD(midiConnect)
GYRO4ALL_FORWARD(midiDisconnect)
GYRO4ALL_FORWARD(midiInAddBuffer)
GYRO4ALL_FORWARD(midiInClose)
GYRO4ALL_FORWARD(midiInGetDevCapsA)
GYRO4ALL_FORWARD(midiInGetDevCapsW)
GYRO4ALL_FORWARD(midiInGetErrorTextA)
GYRO4ALL_FORWARD(midiInGetErrorTextW)
GYRO4ALL_FORWARD(midiInGetID)
GYRO4ALL_FORWARD(midiInGetNumDevs)
GYRO4ALL_FORWARD(midiInMessage)
GYRO4ALL_FORWARD(midiInOpen)
GYRO4ALL_FORWARD(midiInPrepareHeader)
GYRO4ALL_FORWARD(midiInReset)
GYRO4ALL_FORWARD(midiInStart)
GYRO4ALL_FORWARD(midiInStop)
GYRO4ALL_FORWARD(midiInUnprepareHeader)
GYRO4ALL_FORWARD(midiOutCacheDrumPatches)
GYRO4ALL_FORWARD(midiOutCachePatches)
GYRO4ALL_FORWARD(midiOutClose)
GYRO4ALL_FORWARD(midiOutGetDevCapsA)
GYRO4ALL_FORWARD(midiOutGetDevCapsW)
GYRO4ALL_FORWARD(midiOutGetErrorTextA)
GYRO4ALL_FORWARD(midiOutGetErrorTextW)
GYRO4ALL_FORWARD(midiOutGetID)
GYRO4ALL_FORWARD(midiOutGetNumDevs)
GYRO4ALL_FORWARD(midiOutGetVolume)
GYRO4ALL_FORWARD(midiOutLongMsg)
GYRO4ALL_FORWARD(midiOutMessage)
GYRO4ALL_FORWARD(midiOutOpen)
GYRO4ALL_FORWARD(midiOutPrepareHeader)
GYRO4ALL_FORWARD(midiOutReset)
GYRO4ALL_FORWARD(midiOutSetVolume)
GYRO4ALL_FORWARD(midiOutShortMsg)
GYRO4ALL_FORWARD(midiOutUnprepareHeader)
GYRO4ALL_FORWARD(midiStreamClose)
GYRO4ALL_FORWARD(midiStreamOpen)
GYRO4ALL_FORWARD(midiStreamOut)
GYRO4ALL_FORWARD(midiStreamPause)
GYRO4ALL_FORWARD(midiStreamPosition)
GYRO4ALL_FORWARD(midiStreamProperty)
GYRO4ALL_FORWARD(midiStreamRestart)
GYRO4ALL_FORWARD(midiStreamStop)
GYRO4ALL_FORWARD(mixerClose)
GYRO4ALL_FORWARD(mixerGetControlDetailsA)
GYRO4ALL_FORWARD(mixerGetControlDetailsW)
GYRO4ALL_FORWARD(mixerGetDevCapsA)
GYRO4ALL_FORWARD(mixerGetDevCapsW)
GYRO4ALL_FORWARD(mixerGetID)
GYRO4ALL_FORWARD(mixerGetLineControlsA)
GYRO4ALL_FORWARD(mixerGetLineControlsW)
GYRO4ALL_FORWARD(mixerGetLineInfoA)
GYRO4ALL_FORWARD(mixerGetLineInfoW)
GYRO4ALL_FORWARD(mixerGetNumDevs)
GYRO4ALL_FORWARD(mixerMessage)
GYRO4ALL_FORWARD(mixerOpen)
GYRO4ALL_FORWARD(mixerSetControlDetails)
GYRO4ALL_FORWARD(mmDrvInstall)
GYRO4ALL_FORWARD(mmGetCurrentTask)
GYRO4ALL_FORWARD(mmTaskBlock)
GYRO4ALL_FORWARD(mmTaskCreate)
GYRO4ALL_FORWARD(mmTaskSignal)
GYRO4ALL_FORWARD(mmTaskYield)
GYRO4ALL_FORWARD(mmioAdvance)
GYRO4ALL_FORWARD(mmioAscend)
GYRO4ALL_FORWARD(mmioClose)
GYRO4ALL_FORWARD(mmioCreateChunk)
GYRO4ALL_FORWARD(mmioDescend)
GYRO4ALL_FORWARD(mmioFlush)
GYRO4ALL_FORWARD(mmioGetInfo)
GYRO4ALL_FORWARD(mmioInstallIOProcA)
GYRO4ALL_FORWARD(mmioInstallIOProcW)
GYRO4ALL_FORWARD(mmioOpenA)
GYRO4ALL_FORWARD(mmioOpenW)
GYRO4ALL_FORWARD(mmioRead)
GYRO4ALL_FORWARD(mmioRenameA)
GYRO4ALL_FORWARD(mmioRenameW)
GYRO4ALL_FORWARD(mmioSeek)
GYRO4ALL_FORWARD(mmioSendMessage)
GYRO4ALL_FORWARD(mmioSetBuffer)
GYRO4ALL_FORWARD(mmioSetInfo)
GYRO4ALL_FORWARD(mmioStringToFOURCCA)
GYRO4ALL_FORWARD(mmioStringToFOURCCW)
GYRO4ALL_FORWARD(mmioWrite)
GYRO4ALL_FORWARD(mmsystemGetVersion)
GYRO4ALL_FORWARD(sndPlaySoundA)
GYRO4ALL_FORWARD(sndPlaySoundW)
GYRO4ALL_FORWARD(timeBeginPeriod)
GYRO4ALL_FORWARD(timeEndPeriod)
GYRO4ALL_FORWARD(timeGetDevCaps)
GYRO4ALL_FORWARD(timeGetSystemTime)
GYRO4ALL_FORWARD(timeGetTime)
GYRO4ALL_FORWARD(timeKillEvent)
GYRO4ALL_FORWARD(timeSetEvent)
GYRO4ALL_FORWARD(waveInAddBuffer)
GYRO4ALL_FORWARD(waveInClose)
GYRO4ALL_FORWARD(waveInGetDevCapsA)
GYRO4ALL_FORWARD(waveInGetDevCapsW)
GYRO4ALL_FORWARD(waveInGetErrorTextA)
GYRO4ALL_FORWARD(waveInGetErrorTextW)
GYRO4ALL_FORWARD(waveInGetID)
GYRO4ALL_FORWARD(waveInGetNumDevs)
GYRO4ALL_FORWARD(waveInGetPosition)
GYRO4ALL_FORWARD(waveInMessage)
GYRO4ALL_FORWARD(waveInOpen)
GYRO4ALL_FORWARD(waveInPrepareHeader)
GYRO4ALL_FORWARD(waveInReset)
GYRO4ALL_FORWARD(waveInStart)
GYRO4ALL_FORWARD(waveInStop)
GYRO4ALL_FORWARD(waveInUnprepareHeader)
GYRO4ALL_FORWARD(waveOutBreakLoop)
GYRO4ALL_FORWARD(waveOutClose)
GYRO4ALL_FORWARD(waveOutGetDevCapsA)
GYRO4ALL_FORWARD(waveOutGetDevCapsW)
GYRO4ALL_FORWARD(waveOutGetErrorTextA)
GYRO4ALL_FORWARD(waveOutGetErrorTextW)
GYRO4ALL_FORWARD(waveOutGetID)
GYRO4ALL_FORWARD(waveOutGetNumDevs)
GYRO4ALL_FORWARD(waveOutGetPitch)
GYRO4ALL_FORWARD(waveOutGetPlaybackRate)
GYRO4ALL_FORWARD(waveOutGetPosition)
GYRO4ALL_FORWARD(waveOutGetVolume)
GYRO4ALL_FORWARD(waveOutMessage)
GYRO4ALL_FORWARD(waveOutOpen)
GYRO4ALL_FORWARD(waveOutPause)
GYRO4ALL_FORWARD(waveOutPrepareHeader)
GYRO4ALL_FORWARD(waveOutReset)
GYRO4ALL_FORWARD(waveOutRestart)
GYRO4ALL_FORWARD(waveOutSetPitch)
GYRO4ALL_FORWARD(waveOutSetPlaybackRate)
GYRO4ALL_FORWARD(waveOutSetVolume)
GYRO4ALL_FORWARD(waveOutUnprepareHeader)
GYRO4ALL_FORWARD(waveOutWrite)
