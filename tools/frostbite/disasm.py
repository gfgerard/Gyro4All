# Disassembles the running game's code (read-only), with rip-relative operands resolved (float value and module offset).
# Usage: disasm.py <tools dir> <rva hex> <bytes hex> [mark rva hex]  (with a mark: starts at the first offset from rva
# whose decoding lands on the mark, then shows <bytes> past it)   (needs: uv run --with numpy --with capstone)
import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
import capstone
p = Proc('Dead Space.exe')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
rva, n = int(sys.argv[2], 16), int(sys.argv[3], 16); mark = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
a = p.base + rva
if mark is not None:  # sync: the first start (from rva on) whose instruction stream lands exactly on mark
    for start in range(rva, mark):
        if any(i.address - p.base == mark for i in md.disasm(p.read(p.base + start, mark - start + 16), p.base + start)):
            a = p.base + start; n = mark - start + n; break
for i in md.disasm(p.read(a, n), a):
    r = i.address - p.base; extra = ''
    if 'rip' in i.op_str and i.disp:
        tgt = i.address + i.size + i.disp; v = p.read(tgt, 8)
        if v: extra = f'   ; [{p.off(tgt)}] = {struct.unpack("<f", v[:4])[0]:.6g} / 0x{struct.unpack("<Q", v)[0]:X}'
    elif i.mnemonic == 'call' and i.op_str.startswith('0x'):
        extra = f'   ; {p.off(int(i.op_str, 16))}'
    print(f'{"=>" if mark is not None and r == mark else "  "} {r:08X}: {i.mnemonic:8s} {i.op_str}{extra}')
