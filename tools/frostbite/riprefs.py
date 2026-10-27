# Code in the running game that references given exe addresses rip-relatively (e.g. a global table): every
# instruction whose [rip+disp] target is one of them, read-only.
# Usage: riprefs.py <tools dir> <rva hex> ...   (needs: uv run --with numpy --with capstone)
import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import exec_regions
import capstone
p = Proc('Dead Space.exe')
targets = {p.base + int(a, 16) for a in sys.argv[2:]}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for rb, re_ in exec_regions(p):
    for c in range(rb, re_, 64 << 20):
        b = p.read(c, min(64 << 20, re_ - c) + 16)
        if not b: continue
        # a disp32 at offset k of an instruction ending at k+4+imm: try the common encodings ending with the disp
        for k in range(3, len(b) - 4):
            disp = struct.unpack_from('<i', b, k)[0]
            for tail in (0, 1, 4):  # disp last, or followed by an imm8 / imm32
                tgt = c + k + 4 + tail + disp
                if tgt in targets:
                    for back in range(2, 8):
                        start = c + k - back
                        ins = next(md.disasm(b[k - back:k + 4 + tail + 4], start), None)
                        if ins and ins.address + ins.size == c + k + 4 + tail and 'rip' in ins.op_str:
                            print(f'{p.off(ins.address)}  {ins.mnemonic} {ins.op_str}   -> {p.off(tgt)}')
                            break
