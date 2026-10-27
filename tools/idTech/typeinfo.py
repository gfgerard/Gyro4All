#!/usr/bin/env python3
"""id Tech type info, read offline from a game's exe (DOOM 2016, DOOM Eternal, DOOM: The Dark Ages; standard library
only).

id Tech exes carry generated type info for the game code, as static tables (the descendant of Doom 3's TypeInfo).
Two layouts are known; both are tried and the one that finds more classes wins:

  id Tech 6/7 (DOOM 2016, Eternal):
    classTypeInfo_t     { const char* name; const char* superName; u64 size; u64 0; classVariableInfo_t* variables; ... }
    classVariableInfo_t { const char* type; const char* ops; const char* name; u32 offset; u32 size; ... } stride 0x48
  id Tech 8 (The Dark Ages):
    classTypeInfo_t     { const char* name; const char* superName; u64 hash; u64 size; u64 0; variables*; ... }
    classVariableInfo_t { type; ops; name; u32 offset; u32 size; u64 -1; u64 0; const char* comment; ... } stride 0x58

The variables array ends at the first entry whose type or name isn't a string.

  python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --class idPlayer [--inherited]
  python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --class idPlayer --field "view|angle"
  python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --find "Camera"          (class names)
  python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --type playerPState_t    (fields of that type)
  python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --all types.txt
"""
import argparse
import re
import struct
import sys

# (name, qwords in the class header up to and including the variables pointer, index of size, variables stride)
LAYOUTS = [("id Tech 6/7", 5, 2, 0x48), ("id Tech 8", 6, 3, 0x58)]


class Exe:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        osz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        self.secs = []
        for i in range(nsec):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, pe + 24 + osz + i * 40)
            self.secs.append((name.rstrip(b"\0").decode(errors="replace"), va, rsize, raw))

    def off(self, va):
        rva = va - self.base
        for _, sva, rsize, raw in self.secs:
            if sva <= rva < sva + rsize:
                return rva - sva + raw
        return None

    def q(self, va, n=1):
        o = self.off(va)
        if o is None or o + 8 * n > len(self.d):
            return None
        return struct.unpack_from("<%dQ" % n, self.d, o)

    def cstr(self, va):
        """The printable C string at va ("" allowed), else None."""
        o = self.off(va) if va > self.base else None
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + 512)
        if e < 0:
            return None
        s = self.d[o:e]
        return s.decode() if all(32 <= c < 127 for c in s) else None


class TypeInfo:
    def __init__(self, exe):
        self.exe = exe
        best = None
        for layout in LAYOUTS:
            classes = self._scan(*layout[1:])
            if best is None or len(classes) > len(best[1]):
                best = (layout, classes)
        (self.layout, _, _, self.stride), self.classes = best  # classes: name -> (super, size, variables va)

    def _scan(self, words, size_at, stride):
        exe, d = self.exe, self.exe.d
        self.stride = stride
        classes = {}
        name_re = re.compile(r"[A-Za-z_][A-Za-z0-9_:<>, *]*")
        fmt = "<%dQ" % words
        for sname, va, rsize, raw in exe.secs:
            if sname not in (".rdata", ".data"):
                continue
            end = raw + rsize - 8 * words
            for o in range(raw, end, 8):
                q = struct.unpack_from(fmt, d, o)
                nm, sup, size, zero, vars_ = q[0], q[1], q[size_at], q[words - 2], q[words - 1]
                if zero or not 0 < size < 0x10000000 or not exe.base < nm < exe.base + 0x40000000:
                    continue
                if vars_ and not self._entry(vars_):
                    continue
                cn = exe.cstr(nm)
                if not cn or not name_re.fullmatch(cn) or exe.cstr(sup) is None:
                    continue
                classes.setdefault(cn, (exe.cstr(sup), size, vars_))
        return classes

    def _entry(self, va):
        q = self.exe.q(va, 4)
        if not q:
            return None
        t, ops, nm, offsz = q
        tn, name = self.exe.cstr(t), self.exe.cstr(nm)
        if not tn or not name:
            return None
        return tn, self.exe.cstr(ops) or "", name, offsz & 0xFFFFFFFF, offsz >> 32

    def fields(self, cn):
        _, _, va = self.classes[cn]
        out = []
        while va:
            e = self._entry(va)
            if not e:
                break
            out.append(e)
            va += self.stride
        return out

    def chain(self, cn):
        """cn and its bases, most derived first."""
        out = []
        while cn and cn in self.classes and cn not in out:
            out.append(cn)
            cn = self.classes[cn][0]
        return out


def show(ti, cn, pat, inherited, file=sys.stdout):
    for c in ti.chain(cn) if inherited else [cn]:
        sup, size, _ = ti.classes[c]
        print(f"{c} : {sup or '-'}  size 0x{size:x}", file=file)
        for t, ops, name, off, size in ti.fields(c):
            if pat is None or re.search(pat, name, re.I):
                print(f"    0x{off:06x} {size:6d}  {t}{ops} {name}", file=file)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--class", dest="cls")
    ap.add_argument("--field", help="regex on field names (with --class)")
    ap.add_argument("--inherited", action="store_true", help="also the base classes' fields")
    ap.add_argument("--find", help="regex on class names")
    ap.add_argument("--type", help="regex on field types: every class field of that type")
    ap.add_argument("--all", metavar="OUT", help="write every class and field to a text file")
    a = ap.parse_args()

    ti = TypeInfo(Exe(a.exe))
    print(f"{len(ti.classes)} classes ({ti.layout} layout)", file=sys.stderr)
    if a.cls:
        if a.cls not in ti.classes:
            sys.exit(f"no class {a.cls}; close names: {[c for c in ti.classes if a.cls.lower() in c.lower()][:10]}")
        show(ti, a.cls, a.field, a.inherited)
    if a.find:
        for c in sorted(ti.classes):
            if re.search(a.find, c, re.I):
                sup, size, _ = ti.classes[c]
                print(f"{c} : {sup or '-'}  size 0x{size:x}")
    if a.type:
        for c in sorted(ti.classes):
            for t, ops, name, off, size in ti.fields(c):
                if re.search(a.type, t):
                    print(f"{c}.{name} @0x{off:x} : {t}{ops}")
    if a.all:
        with open(a.all, "w", encoding="utf-8") as f:
            for c in sorted(ti.classes):
                show(ti, c, None, False, f)
        print(f"wrote {a.all}", file=sys.stderr)


if __name__ == "__main__":
    main()
