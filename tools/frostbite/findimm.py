# Code that uses 32-bit constants (e.g. EntryInputAction hashes from Gyro4All-types.txt) as immediates: every place in the
# running game's code (read-only) where the little-endian value appears, disassembled around it so only real
# instructions with that immediate are listed, with the function-ish context before.
# Usage: findimm.py <tools dir> <name=decimal or 0xhex> ...   (needs: uv run --with numpy --with capstone)
import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import exec_regions
import capstone
p = Proc('Dead Space.exe')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
wanted = {}
for arg in sys.argv[2:]:
    name, v = arg.split('=')
    wanted[struct.pack('<I', int(v, 0) & 0xFFFFFFFF)] = name
for rb, re_ in exec_regions(p):
    for c in range(rb, re_, 64 << 20):
        b = p.read(c, min(64 << 20, re_ - c))
        if not b: continue
        for pat, name in wanted.items():
            at = b.find(pat)
            while at >= 0:
                addr = c + at
                # an instruction containing the immediate: decode from a few starts before it
                hit = None
                for back in range(1, 12):
                    for i in md.disasm(p.read(addr - back, 32) or b'', addr - back):
                        if i.address <= addr < i.address + i.size and i.address + i.size >= addr + 4:
                            hit = i
                        break
                    if hit: break
                if hit:
                    print(f'{name}: {p.off(hit.address)}  {hit.mnemonic} {hit.op_str}')
                at = b.find(pat, at + 1)
