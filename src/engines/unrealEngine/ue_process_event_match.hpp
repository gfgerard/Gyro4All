#pragma once
// UObject::ProcessEvent's shape: it tests the UFunction's FunctionFlags for FUNC_Native and FUNC_HasOutParms. No game
// access, so it is unit-tested on hand-assembled bytes (tests/profile_tests.cpp).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gyro4all::unreal {

constexpr uint32_t kFuncNative = 0x400, kFuncHasOutParms = 0x400000, kFuncBlueprintCallable = 0x04000000;

// The code tests `flag` of the dword at [reg+flags_off]: `test dword [reg+disp32], imm32` (F7 /0) with the flag, or
// `test byte [reg+disp32], imm8` (F6 /0) on the flag's byte. A REX prefix before the opcode doesn't matter; SIB forms
// ([rsp+...], [r12+...]) aren't recognised.
inline bool tests_function_flag(const uint8_t* code, size_t n, int flags_off, uint32_t flag) {
    int byte = 0;
    while (byte < 3 && !((flag >> (byte * 8)) & 0xFF)) ++byte;
    const uint8_t imm8 = uint8_t(flag >> (byte * 8));
    for (size_t i = 0; i + 10 <= n; ++i) {
        const uint8_t modrm = code[i + 1];
        if ((modrm & 0xC0) != 0x80 || (modrm & 0x38) != 0 || (modrm & 7) == 4) continue;
        int32_t disp = 0;
        std::memcpy(&disp, code + i + 2, 4);
        if (code[i] == 0xF7) {
            uint32_t imm = 0;
            std::memcpy(&imm, code + i + 6, 4);
            if (disp == flags_off && imm == flag) return true;
        } else if (code[i] == 0xF6 && disp == flags_off + byte && code[i + 6] == imm8) {
            return true;
        }
    }
    return false;
}

// Both tests: the shape of ProcessEvent.
inline bool looks_like_process_event(const uint8_t* code, size_t n, int flags_off) {
    return tests_function_flag(code, n, flags_off, kFuncNative) && tests_function_flag(code, n, flags_off, kFuncHasOutParms);
}

}  // namespace gyro4all::unreal
