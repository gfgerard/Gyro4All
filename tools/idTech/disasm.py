#!/usr/bin/env python3
"""Disassembles the whole function containing an exe offset (from the exe's unwind table, .pdata), e.g. a writer or
caller that WriteWatch logged (`writewatch: writer NewColossus_x64vk.exe+0x126B004`). Reads a dumpimage.py dump or
an exe that is readable on disk. Needs capstone:

  uv run --with capstone python -I tools/idTech/disasm.py <scratchpad>/NewColossus_image.bin 126B004 1008F93

The instruction at (or just before) each offset is marked "<==" (a return address follows the call).
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    # A dump has the image layout (raw offset = RVA); for an exe on disk, map RVAs through the section table.
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    osz = struct.unpack_from("<H", img, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", img, pe + 24 + osz + i * 40 + 8) for i in range(nsec)]

    def off(rva):
        for vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        return rva

    exc_rva, exc_size = struct.unpack_from("<II", img, pe + 24 + 112 + 3 * 8)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for a in sys.argv[2:]:
        rva = int(a, 16)
        fn = None
        for o in range(off(exc_rva), off(exc_rva) + exc_size, 12):
            b, e, _ = struct.unpack_from("<III", img, o)
            if b <= rva < e:
                fn = (b, e)
                break
        if not fn:
            print(f"== {a}: no function in the unwind table")
            continue
        b, e = fn
        print(f"== {a} in function {b:x}..{e:x}")
        for i in md.disasm(img[off(b):off(e)], b):
            mark = " <==" if i.address <= rva < i.address + i.size or i.address + i.size == rva else ""
            print(f"{i.address:08x}  {i.mnemonic} {i.op_str}{mark}")


if __name__ == "__main__":
    main()
