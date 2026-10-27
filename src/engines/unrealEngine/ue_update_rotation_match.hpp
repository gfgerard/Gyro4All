#pragma once
// Recognizes APlayerController::UpdateRotation by its shape, without addresses: its first statement copies the whole
// RotationInput (FRotator DeltaRot(RotationInput)), so the function reads at least two of its components from
// [this + offset] before any call, jump or return. AddPitchInput/AddYawInput/AddRollInput read one (Dead Island 2's
// overrides read their component, then call the engine's). No game memory access (it gets the code bytes), unit-tested
// (tests/profile_tests.cpp).
#include <hde64.h>

#include <cstddef>
#include <cstdint>

namespace gyro4all::unreal {

// code: the start of a function (x64, this in rcx). True when it reads at two or more different addresses in
// [offset, offset + size) of rcx within its first instructions, before any call, jump or return and before rcx changes.
inline bool reads_this_field_first(const uint8_t* code, size_t n, int32_t offset, int32_t size, int max_instructions = 48) {
    size_t at = 0;
    int32_t first = -1;  // the first address read in the field
    for (int i = 0; i < max_instructions && at + 16 <= n; ++i) {
        hde64s hs{};
        const unsigned len = hde64_disasm(code + at, &hs);
        if (!len || (hs.flags & F_ERROR)) return false;
        at += len;
        const uint8_t op = hs.opcode, op2 = hs.opcode2;
        const bool modrm = (hs.flags & F_MODRM) != 0;
        // Control flow ends the search: the copy comes first in UpdateRotation.
        if (op == 0xC3 || op == 0xC2 || op == 0xCC || op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F))
            return false;
        if (op == 0x0F && op2 >= 0x80 && op2 <= 0x8F) return false;
        if (op == 0xFF && modrm && hs.modrm_reg >= 2 && hs.modrm_reg <= 5) return false;
        // A memory operand at [rcx + disp32] (no SIB, no RIP): rm 1 without REX.B.
        if (modrm && hs.modrm_mod == 2 && hs.modrm_rm == 1 && !hs.rex_b) {
            const int32_t disp = int32_t(hs.disp.disp32);
            const bool store = op == 0x88 || op == 0x89 || op == 0xC6 || op == 0xC7 ||
                               (op == 0x0F && (op2 == 0x11 || op2 == 0x13 || op2 == 0x17 || op2 == 0x29 ||
                                               op2 == 0x2B || op2 == 0x7F || op2 == 0xD6 || op2 == 0xE7));
            if (!store && disp >= offset && disp < offset + size) {
                if (first >= 0 && disp != first) return true;
                first = disp;
            }
        }
        // rcx (this) overwritten: what follows no longer reads this object.
        if (op == 0xB9 && !hs.rex_b) return false;  // mov ecx, imm
        if (modrm && (op == 0x8B || op == 0x8D || op == 0x03 || op == 0x2B || op == 0x33) && hs.modrm_reg == 1 && !hs.rex_r)
            return false;
        if (modrm && (op == 0x89 || op == 0x31 || op == 0x87) && hs.modrm_mod == 3 && hs.modrm_rm == 1 && !hs.rex_b)
            return false;
    }
    return false;
}

}  // namespace gyro4all::unreal
