#pragma once
// Diagnostics for finding a camera in a new game: which code writes a given address. WriteWatch=<hex address> in
// Gyro4All.ini (re-read every 500 ms while the game runs, so an address found with an external tool can be set
// without a restart) arms a hardware write breakpoint (DR0, 4 bytes) on every thread of the game for
// WriteWatchSeconds (default 10). Each instruction that writes there is logged once with its count, registers, its
// callers (stack unwind, with their non-volatile registers) and the code bytes around it ("writewatch:" lines).
// WriteWatch=x:<hex address> is an execute breakpoint instead (a function's entry): it also samples rcx (the method's
// this) and the floats at WriteWatchFields=<hex offset>,... from it on every call, logged every 200 ms when time is up.
// WriteWatch=r:<hex address> logs the instructions that read or write it (x86 has no read-only breakpoint), to follow
// an input value forward into the code that uses it.
// Instead of a hex address, di[+<hex offset>] is the buffer of the game's last DirectInput GetDeviceState (dinputpad):
// r:di+8 watches lZ of a DIJOYSTATE, the right stick's X on PlayStation controllers. Engine modules can provide more
// names (provide()); those are resolved again every 500 ms while armed and the breakpoint follows when the object moves
// (Frostbite's cam: the active camera's rotation interface, which the game replaces every few seconds).
// Not for playing: the game stops for an exception on every hit (the watch disarms itself after 100,000 hits).
// Off unless WriteWatch has a value when the game starts: WriteWatch=on starts the watcher with nothing to watch yet.
#include <cstdint>

namespace gyro4all::writewatch {

// DllMain (host_configure), when WriteWatch is set: starts the watcher thread.
void start();

// An engine's named address for WriteWatch (e.g. "cam"), any thread, before or after start(). fn runs on the watcher
// thread and returns the object's current address, 0 = none now. Up to 4 names.
using AddressFn = uintptr_t (*)();
void provide(const char* name, AddressFn fn);

}  // namespace gyro4all::writewatch
