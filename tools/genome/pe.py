# pe.py: offline analysis of a Genome game's exe or DLL (Piranha Bytes: ELEX, ELEX II, Risen 3, Risen): exports, functions
# disassembled with export and import names, code references to an address, RTTI vtables, strings, and a dump of a
# running game's decrypted image (Risen 3's exe is encrypted on disk by Steam's DRM). Read-only; see README.md.
#
#   uv run --with pefile --with capstone --with numpy python -I tools/genome/pe.py <exe|dll> <command> ...
#     exports [--grep REGEX]            named exports, parsed by hand (pefile stops at 8192; Risen's Game.dll has 10,858)
#     dis RVA[:END] ...                 a function (or a range), calls named by the exports and imports
#     refs RVA[:END]                    code with RIP-relative operands into [RVA, END)
#     vtable CLASS                      the RTTI vtables of .?AV<CLASS>@@ and their first slots
#     strings REGEX [--refs]            strings matching REGEX (and the code that loads them)
#   ... python -I tools/genome/pe.py <exe name> dump <out.exe>   the running game's image, as a file pe.py can read
import argparse, functools, os, re, struct, sys
import capstone, numpy as np, pefile


class PE:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.ib = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.secs = [(s.Name.rstrip(b"\0").decode(), s.VirtualAddress, s.Misc_VirtualSize, s.Characteristics)
                     for s in self.pe.sections]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def sec(self, name):
        for n, va, sz, _ in self.secs:
            if n == name: return va, sz

    def u32(self, rva): return struct.unpack_from("<I", self.img, rva)[0]
    def u64(self, rva): return struct.unpack_from("<Q", self.img, rva)[0]

    def cstr(self, rva):
        e = self.img.index(b"\0", rva)
        return self.img[rva:e].decode("latin1")

    @functools.cached_property
    def exports(self):
        """rva -> first name (named exports, parsed by hand)."""
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[0]
        out = {}
        if not d.VirtualAddress: return out
        _, nnames, afun, anames, aord = struct.unpack_from("<IIIII", self.img, d.VirtualAddress + 20)
        for i in range(nnames):
            nm = self.u32(anames + 4 * i)
            o = struct.unpack_from("<H", self.img, aord + 2 * i)[0]
            out.setdefault(self.u32(afun + 4 * o), self.cstr(nm))
        return out

    @functools.cached_property
    def imports(self):
        """IAT slot rva -> 'dll!name'."""
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        return {i.address - self.ib: f"{d.dll.decode()}!{i.name.decode() if i.name else '#%d' % i.ordinal}"
                for d in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []) for i in d.imports}

    @functools.cached_property
    def rdata_u64(self):
        va, sz = self.sec(".rdata")
        b = self.img[va:va + sz]
        return va, np.frombuffer(b[: len(b) // 8 * 8], "<u8")

    def vtables(self, name):
        """[(vtable rva, offset in the class)] of RTTI class `name` (e.g. .?AVgCCameraAdmin@@)."""
        out = []
        va, arr = self.rdata_u64
        raw = self.img[va: va + len(arr) * 8]
        for m in re.finditer(re.escape(name.encode()) + b"\0", self.img):
            td = m.start() - 0x10
            for c in re.finditer(re.escape(struct.pack("<I", td)), raw):
                col = va + c.start() - 0xC
                if col % 4 or self.u32(col) != 1: continue
                for i in np.nonzero(arr == self.ib + col)[0]:
                    out.append((va + int(i) * 8 + 8, self.u32(col + 4)))
        return out

    def class_of_vtable(self, vt):
        col = self.u64(vt - 8) - self.ib
        return self.cstr(self.u32(col + 0xC) + 0x10)

    @functools.cached_property
    def pdata(self):
        va, sz = self.sec(".pdata")
        return np.frombuffer(self.img[va:va + sz // 12 * 12], "<u4").reshape(-1, 3)

    def func_of(self, rva):
        """(start, end) of the .pdata chunk holding rva (a function or one of its chunks), else None."""
        a = self.pdata
        i = np.searchsorted(a[:, 0], rva, side="right") - 1
        return (int(a[i, 0]), int(a[i, 1])) if i >= 0 and a[i, 0] <= rva < a[i, 1] else None

    def name(self, rva):
        return self.exports.get(rva) or self.imports.get(rva) or ""

    def dis(self, start, end):
        lines = []
        for ins in self.md.disasm(bytes(self.img[start:end]), self.ib + start):
            r = ins.address - self.ib
            s = f" {r:#x}: {ins.mnemonic} {ins.op_str}"
            m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
            if m:
                t = r + ins.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                s += f"   ; -> {t:#x}" + self.annot(t)
            m = re.match(r"(0x[0-9a-f]+)$", ins.op_str)
            if ins.mnemonic in ("call", "jmp") and m and self.name(int(m.group(1), 16) - self.ib):
                s += "   ; " + self.name(int(m.group(1), 16) - self.ib)
            lines.append(s)
        return "\n".join(lines)

    def annot(self, t):
        if t in self.imports: return " " + self.imports[t]
        if t in self.exports: return " " + self.exports[t]
        for n, va, sz, _ in self.secs:
            if va <= t < va + sz:
                if n == ".rdata":
                    try:
                        if 0 < self.u64(t) - self.ib < len(self.img): return f" [{n}] vt? {self.class_of_vtable(t)}"
                    except Exception: pass
                    try:
                        s = self.cstr(t)
                        if len(s) >= 3 and all(32 <= ord(c) < 127 for c in s): return f' "{s[:60]}"'
                    except Exception: pass
                return f" [{n}]"
        return ""

    def refs(self, lo, hi, limit=400):
        """[(rva, instruction, target)] of code with RIP-relative operands into [lo, hi)."""
        res = []
        for _, tva, sz, ch in self.secs:
            if not ch & 0x20000000: continue
            b = self.img[tva:tva + sz]
            u = np.frombuffer(bytes(b) + b"\0" * 8, np.uint8).astype(np.int64)
            disp = u[:-7] | (u[1:-6] << 8) | (u[2:-5] << 16) | (u[3:-4] << 24)
            disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
            pos = np.arange(len(disp)) + tva
            seen = set()
            for k in range(5):  # bytes of immediate after the displacement
                t = pos + 4 + k + disp
                for i in np.nonzero((t >= lo) & (t < hi))[0]:
                    for back in range(7, 0, -1):
                        st = int(i) - back
                        ins = next(self.md.disasm(bytes(b[st:st + 16]), self.ib + tva + st), None)
                        if ins and ins.size == back + 4 + k and "rip" in ins.op_str and tva + st not in seen:
                            seen.add(tva + st)
                            res.append((tva + st, f"{ins.mnemonic} {ins.op_str}", tva + st + ins.size + int(disp[i])))
                            break
        res.sort()
        return res[:limit]


def parse_range(s):
    a, _, b = s.partition(":")
    return int(a, 16), (int(b, 16) if b else None)


def dump(exe, out):
    """The running game's image (decrypted) as a PE file: sections at their virtual addresses."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "camtrace"))
    import camtrace as ct
    p = ct.Proc(exe)
    img = bytearray(p.size)
    for o in range(0, p.size, 0x10000):
        b = p.read(p.base + o, min(0x10000, p.size - o))
        if b: img[o:o + len(b)] = b
    nt = struct.unpack_from("<I", img, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<HH", img, nt + 6)[0], struct.unpack_from("<H", img, nt + 20)[0]
    for i in range(nsec):
        o = nt + 24 + optsize + 40 * i
        vsize, va = struct.unpack_from("<II", img, o + 8)
        struct.pack_into("<II", img, o + 16, vsize, va)  # SizeOfRawData, PointerToRawData = the virtual layout
    open(out, "wb").write(img)
    print(f"{exe}: {len(img):#x} bytes from {p.base:#x} -> {out}")


def main():
    ap = argparse.ArgumentParser(description="Offline analysis of a Genome game's exe or DLL.")
    ap.add_argument("file", help="the exe or DLL (for dump: the running exe's name)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("exports"); e.add_argument("--grep")
    d = sub.add_parser("dis"); d.add_argument("ranges", nargs="+")
    r = sub.add_parser("refs"); r.add_argument("range")
    v = sub.add_parser("vtable"); v.add_argument("cls"); v.add_argument("--slots", type=int, default=16)
    s = sub.add_parser("strings"); s.add_argument("regex"); s.add_argument("--refs", action="store_true")
    u = sub.add_parser("dump"); u.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "dump": return dump(a.file, a.out)
    g = PE(a.file)
    if a.cmd == "exports":
        for rva, n in sorted(g.exports.items(), key=lambda x: x[1]):
            if not a.grep or re.search(a.grep, n): print(f"{rva:#x} {n}")
    elif a.cmd == "dis":
        for spec in a.ranges:
            start, end = parse_range(spec)
            fr = g.func_of(start)
            end = end or (fr[1] if fr else start + 0x100)
            print(f"==== {start:#x} {g.name(start)} (.pdata chunk {fr and [hex(x) for x in fr]})")
            print(g.dis(start, end))
    elif a.cmd == "refs":
        lo, hi = parse_range(a.range)
        for at, ins, t in g.refs(lo, hi or lo + 1):
            fr = g.func_of(at)
            print(f"{at:#x} {ins:50s} -> {t:#x}  in {fr and hex(fr[0])} {g.name(fr[0]) if fr else ''}")
    elif a.cmd == "vtable":
        cls = a.cls if a.cls.startswith(".?A") else f".?AV{a.cls}@@"
        for vt, off in g.vtables(cls):
            print(f"{cls} vtable {vt:#x} (offset in the class {off:#x})")
            for i in range(a.slots):
                fn = g.u64(vt + 8 * i) - g.ib
                print(f"  slot {i:2d} (+{8 * i:#x}): {fn:#x} {g.name(fn)}")
    elif a.cmd == "strings":
        rx = re.compile(a.regex.encode())
        for _, va, sz, ch in g.secs:
            if ch & 0x20000000: continue
            for m in re.finditer(rb"[\x20-\x7e]{4,}\0", bytes(g.img[va:va + sz])):
                if not rx.search(m.group(0)[:-1]): continue
                at = va + m.start()
                print(f"{at:#x} {m.group(0)[:-1].decode()!r}")
                if a.refs:
                    for ref, ins, _ in g.refs(at, at + 1, limit=8):
                        fr = g.func_of(ref)
                        print(f"    {ref:#x} {ins}  in {fr and hex(fr[0])}")


if __name__ == "__main__":
    main()
