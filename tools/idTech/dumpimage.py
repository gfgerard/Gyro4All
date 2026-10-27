#!/usr/bin/env python3
"""Read-only dump of a running game's main module, rewritten so offline PE tools (typeinfo.py, disasm.py) read it:
ImageBase = the real load base, every section's raw offset/size = its virtual address/size. For exes that can't be
read from disk (Xbox app / PC Game Pass encryption: they are decrypted once mapped). Standard library only, Windows.

  python -I tools/idTech/dumpimage.py NewColossus_x64vk.exe <scratchpad>/NewColossus_image.bin
  python -I tools/idTech/typeinfo.py <scratchpad>/NewColossus_image.bin --find Player

The dump is the game's code: keep it in a scratchpad, never in the repository. Pointers in it are those of this run
(the load base changes per run; offsets from the exe base don't).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import livemem  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        p = livemem.Proc(sys.argv[1])
    except livemem.NotRunning as e:
        sys.exit(str(e))
    img = bytearray(p.read_padded(p.base, p.size, 0x1000))
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    osz = struct.unpack_from("<H", img, pe + 20)[0]
    struct.pack_into("<Q", img, pe + 24 + 24, p.base)
    for i in range(nsec):
        s = pe + 24 + osz + i * 40
        vsize, va = struct.unpack_from("<II", img, s + 8)
        struct.pack_into("<II", img, s + 16, vsize, va)  # SizeOfRawData, PointerToRawData
    open(sys.argv[2], "wb").write(img)
    print(f"pid {p.pid} base 0x{p.base:x} size 0x{p.size:x} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
