#pragma once
// MSVC RTTI in the game's exe: a class's vtable from its decorated name, an object's class name, and vtable slot
// patches. For engine modules whose games keep RTTI (Dawn, Foundation). DllMain or game thread; reads the exe image
// directly (it is mapped), objects through guarded reads.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gyro4all::rtti {

struct Image {
    uintptr_t base = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> data;  // non-code sections [begin, end)
};

Image exe_image();

// The primary vtable (complete object locator with offset 0) of the class with this RTTI name, e.g. ".?AVZFoo@@"; 0 if
// the exe has none.
uintptr_t primary_vtable(const Image& im, const char* rtti_name);

// The RTTI name of an object's class (from its vtable's complete object locator), for the log; "?" if unreadable.
std::string class_name(uintptr_t obj);

// Points slot `slot` of the vtable at `hook` and returns the function it held; nullptr (nothing changed) when the slot
// can't be read or doesn't point into a loaded module.
void* patch_slot(uintptr_t vtable, int slot, void* hook);

}  // namespace gyro4all::rtti
