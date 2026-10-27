# Offline look at a game exe whose code is plain on disk (no game running): function bounds from the exe's x64 unwind
# table (.pdata), disassembly, and who refers to an address (direct calls/jumps, rip-relative operands, qwords that
# hold it). Addresses are RVAs (exe+0x...). Read-only.
# Usage: uv run --with numpy --with capstone python -I exefile.py "<exe>" <command> <rva hex> [...]
#   func <rva> ...         the function containing each address (primary start, chunk)
#   dis <rva> [count]      disassembly of the function chunk containing the address (or count instructions from it)
#   calls <rva>            direct call/jmp sites to the address, with their functions
#   refs <rva>             rip-relative operands that point at the address (lea/mov/cmp...), with their functions
#   holders <rva>          8-byte aligned qwords in the image equal to the address (vtable slots, function tables)
#   find <hex bytes>       places of a byte string in the code, with their functions
#   path <from> <to> [n]   chains of direct calls from one function to another, up to n calls deep (default 4)
import struct, sys
import numpy as np
import capstone


class Image:
    def __init__(self, path):
        data = open(path, 'rb').read()
        e = struct.unpack_from('<I', data, 0x3C)[0]
        nsec, = struct.unpack_from('<H', data, e + 6)
        optsz, = struct.unpack_from('<H', data, e + 20)
        opt = e + 24
        self.base, = struct.unpack_from('<Q', data, opt + 24)
        size, = struct.unpack_from('<I', data, opt + 56)
        self.mem = np.zeros(size, np.uint8)
        self.sections = []
        for i in range(nsec):
            name, vs, va, rs, ro = struct.unpack_from('<8sIIII', data, opt + optsz + i * 40)
            ch, = struct.unpack_from('<I', data, opt + optsz + i * 40 + 36)
            n = min(vs, rs)
            self.mem[va:va + n] = np.frombuffer(data, np.uint8, n, ro)
            self.sections.append((name.rstrip(b'\0').decode(errors='replace'), va, vs, ch))
        exc_rva, exc_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
        t = self.mem[exc_rva:exc_rva + exc_size].tobytes()
        self.funcs = np.frombuffer(t, '<u4')[:len(t) // 12 * 3].reshape(-1, 3)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def code_ranges(self):
        # executable sections, skipping the protection's huge one (its code is not the game's)
        return [(va, va + vs) for n, va, vs, ch in self.sections if ch & 0x20000000 and vs < 0x10000000]

    def u32(self, rva): return struct.unpack('<I', self.mem[rva:rva + 4].tobytes())[0]

    def chunk(self, rva):
        i = int(np.searchsorted(self.funcs[:, 0], rva, 'right')) - 1
        if i < 0 or not (self.funcs[i, 0] <= rva < self.funcs[i, 1]): return None
        return tuple(int(x) for x in self.funcs[i])

    def primary(self, rva):
        c = self.chunk(rva)
        if not c: return None, None
        b, e, u = c
        start = b
        while (self.mem[u] >> 3) & 4:  # UNW_FLAG_CHAININFO
            n = int(self.mem[u + 2])
            start, _, u = struct.unpack('<III', self.mem[u + 4 + ((n + 1) & ~1) * 2:][:12].tobytes())
        return start, (b, e)

    def where(self, rva):
        s, c = self.primary(rva)
        return f'exe+0x{rva:X} (in exe+0x{s:X})' if s is not None else f'exe+0x{rva:X} (no function)'

    def disasm(self, rva, end=None, count=None):
        end = end or rva + 0x4000
        code = self.mem[rva:end].tobytes()
        for k, ins in enumerate(self.md.disasm(code, self.base + rva)):
            if count is not None and k >= count: break
            yield ins

    def _rel32(self):
        # (position of each 4-byte window, its signed value) over the code, in pieces
        for lo, hi in self.code_ranges():
            for start in range(lo, hi - 4, 1 << 23):
                m = self.mem[start:min(hi, start + (1 << 23) + 3)].astype(np.int64)
                r = m[:-3] | (m[1:-2] << 8) | (m[2:-1] << 16) | (m[3:] << 24)
                yield start, np.where(r >= 1 << 31, r - (1 << 32), r)

    def calls(self, target):
        out = []
        for start, r in self._rel32():
            pos = np.arange(len(r)) + start  # position of the rel32
            for h in np.nonzero(pos + 4 + r == target)[0]:
                if self.mem[start + h - 1] in (0xE8, 0xE9): out.append(int(start + h - 1))
        return out

    def rip_target(self, ins):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp - self.base
        return None

    def refs(self, target):
        out = set()
        for start, r in self._rel32():
            pos = np.arange(len(r)) + start  # position of the disp32
            for imm in (0, 1, 2, 4):
                for h in np.nonzero(pos + 4 + imm + r == target)[0]:
                    d = int(start + h)
                    for back in range(1, 8):  # decode from a few bytes back
                        ins = next(self.md.disasm(self.mem[d - back:d + 4 + imm].tobytes(), self.base + d - back), None)
                        if ins and ins.size == back + 4 + imm and self.rip_target(ins) == target:
                            out.add((d - back, f'{ins.mnemonic} {ins.op_str}'))
                            break
        return sorted(out)

    def callees(self, fn):
        c = self.chunk(fn)
        if not c: return []
        out = []
        for ins in self.disasm(c[0], c[1]):
            if ins.mnemonic == 'call' or (ins.mnemonic == 'jmp' and ins.op_str.startswith('0x')):
                try:
                    t = int(ins.op_str, 16) - self.base
                except ValueError:
                    continue
                if not (c[0] <= t < c[1]): out.append(t)
        return out

    def find(self, pattern):
        out = []
        for lo, hi in self.code_ranges():
            m = self.mem[lo:hi].tobytes()
            i = m.find(pattern)
            while i != -1:
                out.append(lo + i)
                i = m.find(pattern, i + 1)
        return out

    def holders(self, target):
        q = self.mem[:len(self.mem) // 8 * 8].view('<u8')
        return [int(i) * 8 for i in np.nonzero(q == self.base + target)[0]]


def show(img, ins):
    rva = ins.address - img.base
    note = ''
    if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
        try:
            t = int(ins.op_str, 16) - img.base
            note = f'   ; exe+0x{t:X}'
        except ValueError:
            pass
    t = img.rip_target(ins)
    if t is not None: note = f'   ; [exe+0x{t:X}]'
    print(f'{rva:9X}  {ins.bytes.hex():<22} {ins.mnemonic} {ins.op_str}{note}')


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    args = [] if cmd == 'find' else [int(a, 16) for a in sys.argv[3:]]
    if cmd == 'func':
        for a in args:
            s, c = img.primary(a)
            print(f'exe+0x{a:X}: ' + (f'function exe+0x{s:X}, chunk exe+0x{c[0]:X}..0x{c[1]:X}' if c else 'no function'))
    elif cmd == 'dis':
        a = args[0]
        if len(args) > 1:
            for ins in img.disasm(a, count=args[1]): show(img, ins)
        else:
            c = img.chunk(a)
            for ins in img.disasm(c[0], c[1]): show(img, ins)
    elif cmd == 'calls':
        for site in img.calls(args[0]): print(img.where(site))
    elif cmd == 'refs':
        for site, text in img.refs(args[0]): print(f'{img.where(site)}: {text}')
    elif cmd == 'find':
        for a in img.find(bytes.fromhex(''.join(sys.argv[3:]))): print(img.where(a))
    elif cmd == 'path':
        start, goal, depth = args[0], args[1], args[2] if len(args) > 2 else 4
        seen, frontier = {start: None}, [start]
        for _ in range(depth):
            nxt = []
            for f in frontier:
                for t in img.callees(f):
                    t = img.primary(t)[0] if img.primary(t)[0] is not None else t
                    if t in seen: continue
                    seen[t] = f
                    nxt.append(t)
            frontier = nxt
            if goal in seen: break
        if goal not in seen:
            print(f'no chain within {depth} calls ({len(seen)} functions reached)')
        else:
            chain, f = [], goal
            while f is not None: chain.append(f); f = seen[f]
            print(' -> '.join(f'exe+0x{f:X}' for f in reversed(chain)))
    elif cmd == 'holders':
        for h in img.holders(args[0]): print(f'exe+0x{h:X}')


if __name__ == '__main__':
    main()
