"""Offline dump of an RE Engine type database (TDB) from a game exe on disk: types, parents, fields (offset, type,
static) and methods (code RVA). Supports TDB 69 and 70 (RE7/RE2/RE3 RT builds, RE8, MHRise TDB70), 71 (RE4 remake),
81 (Monster Hunter Wilds) and 83 (RE Requiem, Pragmata), whose layouts follow REFramework's SDK (shared/sdk/RETypeDB.hpp
namespaces tdb69/tdb70/tdb71/tdb81/tdb83, RETypeDefinition.hpp RETypeDefVersion69/71/74/83).

The TDB is static data in the exe's image, found by its "TDB\\0" magic + version. On disk its table "pointers" are
offsets from the TDB header, which the game turns into pointers at startup;
runtime-only data (managed vtables, so the fieldptr offset of objects) is absent. Reads the file only (no process,
nothing written into the game folder).

Usage:
  tdbdump.py <exe> --type app.PlayerCamera [--type ...] [--no-inherited] [--methods]
  tdbdump.py <exe> --find "regex"          type names matching regex
  tdbdump.py <exe> --field "regex"         fields whose name matches, with their declaring type
  tdbdump.py <exe> --all out.txt           every type with its fields
"""
import argparse, mmap, re, struct, sys

SUPPORTED = (69, 70, 71, 81, 83)
TYPEIMPL_SIZE = 0x30
FIELD_SIZE = 8
FIELDIMPL_SIZE = 12
METHODIMPL_SIZE = 12
FIELD_STATIC = 0x10
FIELD_LITERAL = 0x40


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


class Image:
    """VA <-> file offset for a PE file mapped from disk."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        pe = struct.unpack_from("<I", self.m, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.m, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", self.m, opt + 24)[0]
        sec = opt + struct.unpack_from("<H", self.m, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            o = sec + i * 40
            name = self.m[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsize, rva, rsize, raw = struct.unpack_from("<IIII", self.m, o + 8)
            self.sections.append((name, rva, max(vsize, rsize), raw, rsize))

    def off(self, va):
        rva = va - self.base
        for _, srva, size, raw, rsize in self.sections:
            if srva <= rva < srva + size:
                if rva - srva >= rsize:
                    return None  # uninitialised data: not in the file
                return raw + (rva - srva)
        return None

    def va(self, off):
        for _, srva, _, raw, rsize in self.sections:
            if raw <= off < raw + rsize:
                return self.base + srva + (off - raw)
        return None

    def u32(self, o):
        return struct.unpack_from("<I", self.m, o)[0]

    def u64(self, o):
        return struct.unpack_from("<Q", self.m, o)[0]


# Header offsets per version: counts, then the table pointers (REFramework RETypeDB.hpp tdb69/tdb70/tdb81/tdb83).
HEADERS = {
    69: dict(n_types=0x0C, n_fields=0x14, n_methods=0x10, n_strings=0x50, types=0x60, types_impl=0x68, methods=0x70,
             methods_impl=0x78, fields=0x80, fields_impl=0x88, strings=0xC8),
    70: dict(n_types=0x0C, n_fields=0x14, n_methods=0x10, n_strings=0x50, types=0x60, types_impl=0x68, methods=0x70,
             methods_impl=0x78, fields=0x80, fields_impl=0x88, strings=0xD0),
    # TDB 71 (RE4 remake): TDB 70's header.
    71: dict(n_types=0x0C, n_fields=0x14, n_methods=0x10, n_strings=0x50, types=0x60, types_impl=0x68, methods=0x70,
             methods_impl=0x78, fields=0x80, fields_impl=0x88, strings=0xD0),
    # TDB 81 (MH Wilds): typesStartOfGenerics and the TDB 74 counter move the counts; a padding u32 keeps the pointers
    # where TDB 83 has them.
    81: dict(n_types=0x08, n_fields=0x18, n_methods=0x14, n_strings=0x54, types=0x68, types_impl=0x70, methods=0x78,
             methods_impl=0x80, fields=0x88, fields_impl=0x90, strings=0xD8),
    # TDB 83 (RE Requiem): new counters (typesStartOfGenerics, one from TDB 74, one from TDB 82) move everything.
    83: dict(n_types=0x08, n_fields=0x18, n_methods=0x14, n_strings=0x58, types=0x68, types_impl=0x70, methods=0x78,
             methods_impl=0x80, fields=0x88, fields_impl=0x90, strings=0xD8),
}


class TDB:
    def __init__(self, img, off):
        self.img = img
        self.off = off
        self.version = img.u32(off + 4)
        self.new = self.version >= 71  # 19-bit type ids, field offset in the impl record (checked with 81 and 83)
        self.packed_names = self.version >= 83  # RETypeImpl names: 28-bit fields of one qword, else two int32
        # RETypeDefVersion71 is RETypeDefVersion74 without its last pointer: 0x48 bytes, managed vtable at 0x40.
        self.typedef_size = 0x48 if self.version == 71 else 0x50
        h = HEADERS[self.version]
        self.n_types = img.u32(off + h["n_types"])
        self.n_fields = img.u32(off + h["n_fields"])
        self.n_methods = img.u32(off + h["n_methods"])
        self.n_string_pool = img.u32(off + h["n_strings"])
        # On disk: offsets from the header; in a memory dump: absolute VAs. The header's "initialized" field
        # can't tell (RE7 leaves it at 0 in memory too), the size of the types "pointer" can.
        absolute = img.u64(off + h["types"]) > img.va(off)
        ptr = lambda key: img.u64(off + h[key]) + (0 if absolute else img.va(off))
        self.p_types, self.p_types_impl = ptr("types"), ptr("types_impl")
        self.p_methods, self.p_methods_impl = ptr("methods"), ptr("methods_impl")
        self.p_fields, self.p_fields_impl = ptr("fields"), ptr("fields_impl")
        self.p_strings = ptr("strings")
        mask = 1
        while mask < self.n_string_pool:
            mask <<= 1
        self.string_mask = mask - 1
        for name, p in (("types", self.p_types), ("typesImpl", self.p_types_impl), ("fields", self.p_fields),
                        ("fieldsImpl", self.p_fields_impl), ("stringPool", self.p_strings)):
            if img.off(p) is None:
                raise SystemExit(f"TDB {name} pointer {p:#x} is not in the file's initialised data")
        self.o_types = img.off(self.p_types)
        self.o_types_impl = img.off(self.p_types_impl)
        self.o_fields = img.off(self.p_fields)
        self.o_fields_impl = img.off(self.p_fields_impl)
        self.o_methods = img.off(self.p_methods)
        self.o_methods_impl = img.off(self.p_methods_impl)
        self.o_strings = img.off(self.p_strings)
        self._names = {}
        self._by_name = None

    def string(self, so):
        o = self.o_strings + (so & self.string_mask)
        end = self.img.m.find(b"\0", o, o + 4096)
        return self.img.m[o:end].decode("utf-8", errors="replace")

    def typedef(self, i):
        o = self.o_types + i * self.typedef_size
        m = self.img.m
        a, b = struct.unpack_from("<QQ", m, o)
        flags, size = struct.unpack_from("<II", m, o + 0x10)
        if self.new:
            # RETypeDefVersion71/74/83 (same fields): ids 19 bits; default_ctor:22, member_method:22, member_field:20 at 0x20.
            packed = self.img.u64(o + 0x20)
            parent, declaring, impl_index = bits(a, 19, 19), bits(a, 38, 19), bits(b, 38, 18)
            element = bits(b, 19, 19)
            member_method, member_field = bits(packed, 22, 22), bits(packed, 44, 20)
            managed_vt = self.img.u64(o + 0x40)
            io = self.o_types_impl + impl_index * TYPEIMPL_SIZE
            if self.packed_names:
                names = self.img.u64(io)
                name_off, ns_off = bits(names, 0, 28), bits(names, 28, 28)
            else:
                name_off, ns_off = struct.unpack_from("<ii", m, io)
            field_size = struct.unpack_from("<i", m, io + 8)[0]
            num_fields = bits(self.img.u64(io + 0x10), 33, 24)
            num_methods = struct.unpack_from("<H", m, io + 0x18)[0]
        else:
            # RETypeDefVersion69: ids 18 bits; member_method / member_field u32 at 0x28 / 0x2C.
            parent, declaring, impl_index = bits(a, 18, 18), bits(a, 36, 18), bits(b, 36, 18)
            member_method, member_field = struct.unpack_from("<II", m, o + 0x28)
            managed_vt = self.img.u64(o + 0x48)
            io = self.o_types_impl + impl_index * TYPEIMPL_SIZE
            name_off, ns_off, field_size = struct.unpack_from("<iii", m, io)
            num_methods = struct.unpack_from("<H", m, io + 0x12)[0]
            num_fields = struct.unpack_from("<i", m, io + 0x14)[0]
            element = 0
        return {
            "parent": parent, "declaring": declaring, "flags": flags, "size": size,
            "member_method": member_method, "member_field": member_field,
            "name": self.string(name_off), "namespace": self.string(ns_off), "num_fields": num_fields,
            "num_methods": num_methods, "field_size": field_size, "managed_vt": managed_vt,
            "element": element,
        }

    def full_name(self, i, depth=0):
        if i in self._names:
            return self._names[i]
        t = self.typedef(i)
        if not t["name"] and t["element"] not in (0, i) and depth < 16:
            # Arrays (TDB 71+) share a nameless impl record; the element type is in the second qword.
            name = self.full_name(t["element"], depth + 1) + "[]"
        elif t["declaring"] not in (0, i) and depth < 16:
            name = self.full_name(t["declaring"], depth + 1) + "." + t["name"]
        else:
            name = (t["namespace"] + "." if t["namespace"] else "") + t["name"]
        self._names[i] = name
        return name

    def by_name(self):
        if self._by_name is None:
            self._by_name = {}
            for i in range(self.n_types):
                self._by_name.setdefault(self.full_name(i), i)
        return self._by_name

    def fieldptr_offset(self, t):
        """Where the declared field offsets start in an object (REFramework get_fieldptr_offset: an int32 just before
        the type's managed vtable). None when the vtable is not in the file (filled at runtime)."""
        if not t["managed_vt"]:
            return None
        o = self.img.off(t["managed_vt"] - 8)
        return None if o is None else struct.unpack_from("<i", self.img.m, o)[0]

    def fields(self, i):
        t = self.typedef(i)
        out = []
        for k in range(t["num_fields"]):
            v = self.img.u64(self.o_fields + (t["member_field"] + k) * FIELD_SIZE)
            fi = self.o_fields_impl
            if self.new:
                # REField: declaring:19, impl_id:19, field_typeid:19. REFieldImpl: u16 unk:1 flags:15,
                # u32 offset:26, u32 name_offset:28.
                impl, ftype = bits(v, 19, 19), bits(v, 38, 19)
                _, fl, off, nm = struct.unpack_from("<HHII", self.img.m, fi + impl * FIELDIMPL_SIZE)
                flags, offset, name = fl >> 1, bits(off, 0, 26), bits(nm, 0, 28)
            else:
                # REField: declaring:18, impl_id:20, offset:26. REFieldImpl: u16 flags, u32 field_typeid:18,
                # u32 name_offset:30.
                impl, offset = bits(v, 18, 20), bits(v, 38, 26)
                _, flags, ft, nm = struct.unpack_from("<HHII", self.img.m, fi + impl * FIELDIMPL_SIZE)
                ftype, name = bits(ft, 0, 18), bits(nm, 0, 30)
            out.append({"name": self.string(name), "offset": offset, "flags": flags, "type": self.full_name(ftype)})
        return out

    def methods(self, i):
        t = self.typedef(i)
        out = []
        for k in range(t["num_methods"]):
            if self.new:
                # REMethodDefinition (0xC): u32 declaring:19 params_lo:13, u32 impl_id:19 params_hi:13, i32 offset.
                mo = self.o_methods + (t["member_method"] + k) * 0xC
                impl, fn = bits(self.img.u32(mo + 4), 0, 19), 0
            else:
                mo = self.o_methods + (t["member_method"] + k) * 16
                v, fn = struct.unpack_from("<QQ", self.img.m, mo)
                impl = bits(v, 18, 20)
            io = self.o_methods_impl + impl * METHODIMPL_SIZE
            _, vtable_index, flags, impl_flags, nm = struct.unpack_from("<HhHHI", self.img.m, io)
            out.append({"name": self.string(nm), "rva": (fn - self.img.base) if fn else 0,
                        "vtable": vtable_index, "flags": flags})
        return out

    def chain(self, i):
        out, seen = [], set()
        while i and i not in seen and len(out) < 64:
            seen.add(i)
            out.append(i)
            p = self.typedef(i)["parent"]
            if p == i:
                break
            i = p
        return out


def find_tdb(img):
    for mo in re.finditer(rb"TDB\x00", img.m):
        o = mo.start()
        if img.u32(o + 4) in SUPPORTED and img.va(o) is not None:
            return o
    for mo in re.finditer(rb"TDB\x00", img.m):
        v = img.u32(mo.start() + 4)
        if 30 <= v <= 120:
            raise SystemExit(f"TDB version {v} found; this tool only reads {SUPPORTED}")
    raise SystemExit("no TDB found in this file")


def fmt_field(tdb, f, base):
    tags = []
    if f["flags"] & FIELD_LITERAL:
        tags.append("const")
    elif f["flags"] & FIELD_STATIC:
        tags.append("static")
    off = f"+{f['offset']:#06x}"
    if base is not None and not tags:
        off += f" (obj {base + f['offset']:#06x})"
    return f"    {off:22} {f['name']:48} {f['type']}" + (f"  [{' '.join(tags)}]" if tags else "")


def dump_type(tdb, i, inherited, with_methods, out):
    name = tdb.full_name(i)
    chain = tdb.chain(i)
    t = tdb.typedef(i)
    out.write(f"{name}  (type {i}, size {t['size']:#x}, fields {t['field_size']:#x})\n")
    out.write("  parents: " + " <- ".join(tdb.full_name(c) for c in chain[1:]) + "\n")
    for c in (chain if inherited else chain[:1]):
        ct = tdb.typedef(c)
        fs = tdb.fields(c)
        ms = tdb.methods(c) if with_methods else []
        if not fs and not ms:
            continue
        base = tdb.fieldptr_offset(ct)
        out.write(f"  [{tdb.full_name(c)}]" + (f" fieldptr {base:#x}" if base is not None else "") + "\n")
        for f in fs:
            out.write(fmt_field(tdb, f, base) + "\n")
        for mth in ms:
            # Code pointers are filled at startup: only a memory dump has them.
            rva = f"  rva {mth['rva']:#x}" if mth["rva"] else ""
            out.write(f"    {mth['name']}(){rva}" + (f"  vtable {mth['vtable']}" if mth["vtable"] >= 0 else "") + "\n")
    out.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--type", action="append", default=[])
    ap.add_argument("--find")
    ap.add_argument("--field")
    ap.add_argument("--all")
    ap.add_argument("--no-inherited", action="store_true")
    ap.add_argument("--methods", action="store_true")
    a = ap.parse_args()

    img = Image(a.exe)
    tdb = TDB(img, find_tdb(img))
    print(f"TDB {tdb.version} at file {tdb.off:#x} (va {img.va(tdb.off):#x}): {tdb.n_types} types, "
          f"{tdb.n_fields} fields, {tdb.n_methods} methods, image base {img.base:#x}")
    out = sys.stdout
    names = tdb.by_name()
    for tn in a.type:
        if tn not in names:
            print(f"{tn}: not found")
            continue
        dump_type(tdb, names[tn], not a.no_inherited, a.methods, out)
    if a.find:
        rx = re.compile(a.find)
        for n in sorted(n for n in names if rx.search(n)):
            t = tdb.typedef(names[n])
            print(f"  {n}  <- {tdb.full_name(t['parent']) if t['parent'] else '-'}")
    if a.field:
        rx = re.compile(a.field)
        for i in range(tdb.n_types):
            for f in tdb.fields(i):
                if rx.search(f["name"]):
                    print(f"  {tdb.full_name(i)}.{f['name']}  +{f['offset']:#x}  {f['type']}")
    if a.all:
        with open(a.all, "w", encoding="utf-8") as fo:
            for i in range(1, tdb.n_types):
                dump_type(tdb, i, False, a.methods, fo)
        print(f"wrote {a.all}")


if __name__ == "__main__":
    main()
