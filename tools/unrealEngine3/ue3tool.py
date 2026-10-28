#!/usr/bin/env python3
"""Inspect a RUNNING Unreal Engine 3 game (64-bit) for Gyro4All's Unreal3 module (src/engines/unrealEngine3/): its
reflection (objects, names, properties), the player's scene and camera rotation, and which fields change with a menu.
Nothing is fixed per game: GObjects, the UObject layout, the name table (BioWare's SFXName chunks or UE3's GNames), the
property offset field and the bool bitmask field are found in memory, then cached per process in %TEMP%.

Read-only (PROCESS_VM_READ) except `write-test`, which writes the controller's Rotation. Standard library only.

  ue3tool.py <exe> find                              find and print the reflection layout
  ue3tool.py <exe> objects [--class RE] [--name RE]  list live objects (class, full name, address)
  ue3tool.py <exe> props <Class>                     a class's own properties: type, offset, size, bool mask
  ue3tool.py <exe> scene                             GameEngine -> LocalPlayer -> controller, pawn, camera, WorldInfo
  ue3tool.py <exe> snap <out.pkl>                    snapshot: every object, plus the scene objects' bytes
  ue3tool.py <exe> diff <a.pkl> <b.pkl>              what differs: object counts, new objects, named fields and bools
  ue3tool.py <exe> write-test [--wait 15] [--hold 3] [--pitch 20] [--yaw 30]   WRITES the controller's Rotation

<exe> is the process name, e.g. MassEffect1.exe. See README.md in this folder for the workflow.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import json
import os
import pickle
import re
import struct
import subprocess
import sys
import tempfile
import time

PROCESS_VM_READ, PROCESS_VM_WRITE, PROCESS_VM_OPERATION, PROCESS_QUERY_INFORMATION = 0x10, 0x20, 0x08, 0x400
IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE = 0x80000000, 0x20000000


# ------------------------------------------------------------------ process

class Process:
    def __init__(self, exe_name, write=False):
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.k32.OpenProcess.restype = wt.HANDLE
        self.k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                               ctypes.POINTER(ctypes.c_size_t)]
        self.k32.WriteProcessMemory.argtypes = self.k32.ReadProcessMemory.argtypes
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe_name}", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        pids = [int(p.split('","')[1]) for p in out.splitlines() if p.lower().startswith(f'"{exe_name.lower()}"')]
        if not pids:
            raise SystemExit(f"{exe_name} is not running")
        self.pid = pids[0]
        access = PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | (PROCESS_VM_WRITE | PROCESS_VM_OPERATION if write else 0)
        self.h = self.k32.OpenProcess(access, False, self.pid)
        if not self.h:
            raise SystemExit(f"can't open {exe_name} (error {ctypes.get_last_error()})")
        mods = (wt.HMODULE * 1)()
        need = wt.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        self.base = mods[0]

    def read(self, address, size):
        if not address:
            return None
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        ok = self.k32.ReadProcessMemory(self.h, ctypes.c_void_p(address), buf, size, ctypes.byref(got))
        return buf.raw[:got.value] if ok else None

    def write(self, address, data):
        n = ctypes.c_size_t()
        return bool(self.k32.WriteProcessMemory(self.h, ctypes.c_void_p(address), data, len(data), ctypes.byref(n)))

    def _unpack(self, fmt, a):
        b = self.read(a, struct.calcsize(fmt))
        return struct.unpack(fmt, b)[0] if b and len(b) == struct.calcsize(fmt) else None

    def i32(self, a): return self._unpack("<i", a)
    def u32(self, a): return self._unpack("<I", a)
    def u64(self, a): return self._unpack("<Q", a)

    def ptr(self, a):
        v = self.u64(a)
        return v if v and 0x10000 <= v < 0x7FFFFFFF0000 else 0

    def sections(self):
        """(name, address, size, characteristics) of the exe's sections, from its headers in memory."""
        lfanew = self.i32(self.base + 0x3C)
        count = struct.unpack("<H", self.read(self.base + lfanew + 6, 2))[0]
        opt_size = struct.unpack("<H", self.read(self.base + lfanew + 20, 2))[0]
        first = self.base + lfanew + 24 + opt_size
        out = []
        for i in range(count):
            h = self.read(first + i * 40, 40)
            name = h[:8].rstrip(b"\0").decode("latin-1")
            vsize, rva = struct.unpack_from("<II", h, 8)
            out.append((name, self.base + rva, vsize, struct.unpack_from("<I", h, 36)[0]))
        return out

    def writable(self):
        """(address, bytes) of the exe's writable, non-executable sections (where engine globals live)."""
        for name, addr, size, ch in self.sections():
            if ch & IMAGE_SCN_MEM_WRITE and not ch & IMAGE_SCN_MEM_EXECUTE:
                data = self.read(addr, size)
                if data:
                    yield addr, data

    def where(self, a):
        return f"exe+0x{a - self.base:X}" if self.base <= a < self.base + 0x10000000 else f"0x{a:X}"


# ------------------------------------------------------------------ reflection

class UE3:
    """The layout, found once per process (cached), and readers on top of it."""

    def __init__(self, proc, refresh=False):
        self.p = proc
        self._names, self._class_names = {}, {}
        cache = os.path.join(tempfile.gettempdir(), f"ue3tool-{proc.pid}.json")
        if not refresh and os.path.exists(cache):
            self.layout = json.load(open(cache))
        else:
            self.layout = {}
            self._find()
            json.dump(self.layout, open(cache, "w"), indent=1)
        L = self.layout
        self.o_index, self.o_outer, self.o_name, self.o_class = L["index"], L["outer"], L["name"], L["class"]
        self.refresh_objects()

    # -- objects
    def refresh_objects(self):
        data, num = self.p.ptr(self.layout["gobjects"]), self.p.i32(self.layout["gobjects"] + 8)
        raw = self.p.read(data, 8 * num) if data and num else b""
        self.objs = list(struct.unpack(f"<{len(raw) // 8}Q", raw))

    def outer(self, o): return self.p.ptr(o + self.o_outer) if o else 0
    def cls(self, o): return self.p.ptr(o + self.o_class) if o else 0

    # -- names
    def fname(self, value):
        if value in self._names:
            return self._names[value]
        L, text = self.layout, None
        if L["names_kind"] == "sfx":  # BioWare: 29-bit byte offset into a chunk, 3-bit chunk index
            chunk = self.p.u64(L["names"] + ((value >> 29) & 7) * 8)
            b = self.p.read(chunk + (value & 0x1FFFFFFF) + L["name_text"], 64) if chunk else None
        else:  # UE3: GNames TArray<FNameEntry*>, indexed
            data = self.p.ptr(L["names"])
            entry = self.p.ptr(data + value * 8) if data else 0
            b = self.p.read(entry + L["name_text"], 64) if entry else None
        if b:
            text = b.split(b"\0")[0].decode("latin-1")
        self._names[value] = text or f"?{value:x}"
        return self._names[value]

    def name(self, o):
        b = self.p.read(o + self.o_name, 8) if o else None
        if not b:
            return "?"
        value, number = struct.unpack("<Ii", b)
        s = self.fname(value)
        return s if number == 0 else f"{s}_{number - 1}"

    def class_name(self, o):
        c = self.cls(o)
        if c not in self._class_names:
            self._class_names[c] = self.name(c) if c else "?"
        return self._class_names[c]

    def full(self, o):
        parts, x = [], o
        while x and len(parts) < 10:
            parts.append(self.name(x))
            x = self.outer(x)
        return ".".join(reversed(parts))

    def class_chain(self, o):
        """Names of an object's class and its super classes (needs UStruct.SuperField)."""
        out, c = [], self.cls(o)
        while c and len(out) < 32:
            out.append(self.name(c))
            c = self.p.ptr(c + self.layout["super"]) if self.layout.get("super") else 0
        return out

    def find_object(self, class_name, skip_defaults=True):
        """The first object of that class or a subclass (Mass Effect LE 2's engine is an SFXEngine)."""
        for o in self.objs:
            if o and class_name in self.class_chain(o) and not (skip_defaults and self.name(o).startswith("Default__")):
                return o
        return 0

    # -- properties
    def properties(self, owner_class=None):
        """(object, class, name, owner name, offset, element size, array dim, bool mask) of every property."""
        L = self.layout
        for o in self.objs:
            if not o:
                continue
            c = self.class_name(o)
            if not c.endswith("Property"):
                continue
            outer = self.outer(o)
            if self.class_name(outer) in ("Function", "State"):  # parameters and locals, not fields
                continue
            owner = self.name(outer)
            if owner_class and owner != owner_class:
                continue
            mask = self.p.u32(o + L["bool_mask"]) if c == "BoolProperty" and L.get("bool_mask") else None
            size = self.p.i32(o + L["elem_size"]) if L.get("elem_size") else None
            dim = self.p.i32(o + L["array_dim"]) if L.get("array_dim") else None
            yield o, c, self.name(o), owner, self.p.i32(o + L["prop_offset"]), size, dim, mask

    def prop_index(self):
        """{(owner, name): property row}, built once (one pass over every object)."""
        if not hasattr(self, "_prop_index"):
            self._prop_index = {}
            for row in self.properties():
                self._prop_index.setdefault((row[3], row[2]), row)
        return self._prop_index

    def offset_of(self, owner, name):
        row = self.prop_index().get((owner, name))
        return row[4] if row else None

    # -- the search
    def _find(self):
        p, L = self.p, self.layout
        # GObjects: a TArray of pointers whose first objects hold their own index at a small offset.
        for addr, data in p.writable():
            for off in range(0, len(data) - 16, 8):
                ptr, num, mx = struct.unpack_from("<Qii", data, off)
                if not (0x10000 < ptr < 0x7FFFFFFF0000 and 20000 <= num <= 5000000 and num <= mx <= num * 4):
                    continue
                first = p.read(ptr, 8 * 17)
                if not first:
                    continue
                objs = struct.unpack("<17Q", first)
                hits = collections.Counter()
                for i in range(1, 17):
                    b = p.read(objs[i], 0x40) if objs[i] else None
                    if b:
                        for f in range(4, 0x40, 4):
                            if struct.unpack_from("<i", b, f)[0] == i:
                                hits[f] += 1
                if hits and hits.most_common(1)[0][1] >= 14:
                    L["gobjects"], L["index"] = addr + off, hits.most_common(1)[0][0]
                    break
            if "gobjects" in L:
                break
        else:
            raise SystemExit("GObjects not found")
        data = p.ptr(L["gobjects"])
        objs = struct.unpack("<4096Q", p.read(data, 8 * 4096))
        objset = set(o for o in objs if o)
        # Class: the pointer field where some object points to itself (the class Class); Outer and Name before it.
        for f in range((L["index"] + 4) & ~3, 0x100, 4):  # 4-aligned too: Dishonored packs UObject (Class +0x4C)
            if any(o and p.u64(o + f) == o for o in objs[:2048]) and sum(1 for o in objs[:512] if p.u64(o + f) in objset) > 400:
                L["class"], L["name"], L["outer"] = f, f - 8, f - 16
                break
        else:
            raise SystemExit("UObject.Class not found")
        class_class = next(o for o in objs if o and p.u64(o + L["class"]) == o)
        value = p.u32(class_class + L["name"])
        # Names from every chunk in use, spread over all of GObjects (the first objects' are all in chunk 0; Mass Effect
        # LE 2 has a decoy array whose chunk 0 is right and chunk 1 isn't).
        count = p.i32(L["gobjects"] + 8)
        spread = struct.unpack(f"<{count}Q", p.read(data, 8 * count) or bytes(8 * count))[::max(1, count // 4000)]
        per_chunk, samples = collections.Counter(), []
        for o in spread:
            v = p.u32(o + L["name"]) if o else None
            if v is not None and per_chunk[v >> 29] < 24:
                per_chunk[v >> 29] += 1
                samples.append(v)
        ident = re.compile(r"[A-Za-z_][A-Za-z0-9_\-]*")
        chunk_slot = ((value >> 29) & 7) * 8

        def try_names(kind, a, text):
            L.update(names=a, names_kind=kind, name_text=text)
            if self._read_raw(value) != "Class":
                return False
            if kind == "sfx":  # a decoy array can repeat a chunk in two slots (Mass Effect LE 3)
                used = {v >> 29 for v in samples}
                ptrs = [p.u64(a + 8 * s) for s in used]
                if not all(ptrs) or len(set(ptrs)) != len(ptrs):
                    return False
            # Most, not all: Mass Effect LE 1 has objects named "<uninitialized>".
            return sum(1 for v in samples if ident.fullmatch(self._read_raw(v) or "?")) * 4 >= len(samples) * 3

        # Names: BioWare's chunk array (Mass Effect LE: 29-bit byte offset + 3-bit chunk, text at entry +12) or UE3's
        # GNames (TArray<FNameEntry*> indexed by the value), whichever reads "Class" for the class Class and identifiers
        # for other objects. Candidates are filtered in the section's bytes; only they are read from the game.
        found = False
        for addr, blob in p.writable():
            for off in range(0, len(blob) - 64, 8):
                ptr = struct.unpack_from("<Q", blob, off + chunk_slot)[0]
                if 0x10000 < ptr < 0x7FFFFFFF0000 and try_names("sfx", addr + off, 12):
                    found = True
                    break
                data, num, mx = struct.unpack_from("<Qii", blob, off)
                if (0x10000 < data < 0x7FFFFFFF0000 and value < num <= mx <= num * 4
                        and any(try_names("gnames", addr + off, t) for t in (0x10, 0x14, 0x18, 0x0C))):
                    found = True
                    break
            if found:
                break
        if not found:
            raise SystemExit("name table not found")
        self._names = {}
        self.o_index, self.o_outer, self.o_name, self.o_class = L["index"], L["outer"], L["name"], L["class"]
        self._class_names = {}
        self.refresh_objects()
        # UProperty's offset field: FMatrix's planes are at 0, 0x10, 0x20, 0x30.
        planes = {}
        for o in self.objs:
            if o and self.class_name(o) == "StructProperty" and self.name(self.outer(o)) == "Matrix":
                n = self.name(o)
                if n in ("XPlane", "YPlane", "ZPlane", "WPlane"):
                    planes["XYZW".index(n[0])] = o
            if len(planes) == 4:
                break
        L["prop_offset"] = next((f for f in range(0x40, 0x120, 4)
                                 if all(p.i32(planes[k] + f) == k * 0x10 for k in range(4))), None)
        if L["prop_offset"] is None:
            raise SystemExit("UProperty offset field not found")
        # ElementSize (0x10 for an FPlane) and ArrayDim (1) before it.
        L["elem_size"] = next((f for f in range(L["prop_offset"] - 4, 0x30, -4)
                               if all(p.i32(planes[k] + f) == 0x10 for k in range(4))), None)
        L["array_dim"] = L["elem_size"] - 4 if L["elem_size"] and all(p.i32(planes[k] + L["elem_size"] - 4) == 1
                                                                        for k in range(4)) else None
        # UStruct.SuperField: the field through which the class PlayerController reaches Controller.
        pcc = next((o for o in self.objs if o and self.class_name(o) == "Class" and self.name(o) == "PlayerController"), 0)
        L["super"] = next((f for f in range(L["class"] + 8, 0x120, 8)
                           if pcc and self.name(p.ptr(pcc + f)) == "Controller"), None)
        # UBoolProperty's bitmask: distinct powers of two among the bools sharing one offset in Actor.
        bools = [o for o in self.objs if o and self.class_name(o) == "BoolProperty" and self.name(self.outer(o)) == "Actor"]
        by_off = collections.defaultdict(list)
        for o in bools:
            by_off[p.i32(o + L["prop_offset"])].append(o)
        group = max(by_off.values(), key=len) if by_off else []
        L["bool_mask"] = next((f for f in range(L["prop_offset"] + 4, 0x140, 4)
                               if len(group) > 2 and len({p.u32(o + f) for o in group}) == len(group)
                               and all((m := p.u32(o + f)) and not m & (m - 1) for o in group)), None)

    def _read_raw(self, value):
        self._names.pop(value, None)
        s = self.fname(value)
        return None if s.startswith("?") else s


# ------------------------------------------------------------------ scene

def scene(u):
    """The scene objects by name and reflected offsets."""
    p = u.p
    off = {k: u.offset_of(*k.split(".")) for k in ("Engine.GamePlayers", "Player.Actor", "Controller.Pawn", "Actor.WorldInfo",
                                                    "PlayerController.PlayerCamera", "WorldInfo.Game", "Engine.GameViewport",
                                                    "PlayerController.PlayerInput", "PlayerController.myHUD")}
    s = {"GameEngine": u.find_object("GameEngine")}
    players = p.ptr(s["GameEngine"] + off["Engine.GamePlayers"]) if s["GameEngine"] and off["Engine.GamePlayers"] else 0
    s["LocalPlayer"] = p.ptr(players) if players else 0
    s["Controller"] = p.ptr(s["LocalPlayer"] + off["Player.Actor"]) if s["LocalPlayer"] else 0
    pc = s["Controller"]
    for key, field, base in (("Pawn", "Controller.Pawn", pc), ("Camera", "PlayerController.PlayerCamera", pc),
                             ("WorldInfo", "Actor.WorldInfo", pc), ("PlayerInput", "PlayerController.PlayerInput", pc),
                             ("HUD", "PlayerController.myHUD", pc), ("Viewport", "Engine.GameViewport", s["GameEngine"])):
        s[key] = p.ptr(base + off[field]) if base and off[field] else 0
    s["Game"] = p.ptr(s["WorldInfo"] + off["WorldInfo.Game"]) if s["WorldInfo"] and off["WorldInfo.Game"] else 0
    return {k: v for k, v in s.items() if v}


def rotation_text(p, a):
    b = p.read(a, 12)
    if not b:
        return "?"
    pitch, yaw, roll = struct.unpack("<iii", b)
    return f"pitch {((pitch + 32768) % 65536 - 32768) * 360 / 65536:.1f} yaw {yaw * 360 / 65536:.1f} (raw {pitch}, {yaw}, {roll})"


def world_bools(u, world):
    out = []
    for n in ("bPlayersOnly", "bIsMenuLevel", "bPlayersOnlyPending"):
        row = u.prop_index().get(("WorldInfo", n))
        if row and row[1] == "BoolProperty" and row[7]:
            word = u.p.u32(world + row[4])
            out.append(f"{n}={int(bool(word is not None and word & row[7]))}")
    pauser = u.offset_of("WorldInfo", "Pauser")
    if pauser:
        out.append(f"Pauser={u.p.ptr(world + pauser):x}")
    return " ".join(out)


# ------------------------------------------------------------------ commands

def cmd_find(u, args):
    L, p = u.layout, u.p
    print(f"GObjects       {p.where(L['gobjects'])}  ({len(u.objs)} objects)")
    print(f"UObject        index +0x{L['index']:X}, Outer +0x{L['outer']:X}, Name +0x{L['name']:X}, Class +0x{L['class']:X}")
    print(f"names          {L['names_kind']} at {p.where(L['names'])}, text at entry +0x{L['name_text']:X}")
    opt = lambda k: f"+0x{L[k]:X}" if L.get(k) else "not found"
    print(f"UProperty      Offset +0x{L['prop_offset']:X}, ElementSize {opt('elem_size')}, ArrayDim {opt('array_dim')}; "
          f"UBoolProperty mask {opt('bool_mask')}; UStruct.SuperField {opt('super')}")
    for k in ("Engine.GamePlayers", "Player.Actor", "Actor.Rotation", "Controller.Pawn", "Actor.WorldInfo", "WorldInfo.Pauser"):
        o = u.offset_of(*k.split("."))
        print(f"  {k:<22} " + (f"+0x{o:X}" if o is not None else "not found"))


def cmd_objects(u, args):
    cre, nre = (re.compile(r, re.I) if r else None for r in (args.cls, args.name))
    for o in u.objs:
        if not o:
            continue
        c = u.class_name(o)
        if cre and not cre.search(c):
            continue
        f = u.full(o)
        if nre and not nre.search(f):
            continue
        print(f"{o:016x}  {c:<32} {f}")


def cmd_props(u, args):
    rows = sorted(u.properties(args.cls), key=lambda r: (r[4], r[7] or 0))
    for o, c, n, ow, off, size, dim, mask in rows:
        extra = f" mask 0x{mask:X}" if mask else ""
        print(f"+0x{off:04X}  size 0x{size:X}{f' x{dim}' if dim and dim > 1 else ''}  {c:<16} {n}{extra}")


def cmd_scene(u, args):
    p, s = u.p, scene(u)
    for k, v in s.items():
        print(f"{k:<12} {v:016x}  {u.class_name(v):<28} {u.full(v)}")
    rot = u.offset_of("Actor", "Rotation")
    for k in ("Controller", "Pawn"):
        if k in s and rot is not None:
            print(f"{k}.Rotation  {rotation_text(p, s[k] + rot)}")
    if "WorldInfo" in s:
        print(f"WorldInfo    {world_bools(u, s['WorldInfo'])}")


def cmd_snap(u, args):
    s = scene(u)
    objs = {o: (u.class_name(o), u.full(o)) for o in u.objs if o}
    data = {k: u.p.read(v, 0x1800) for k, v in s.items()}
    fields = {}  # class -> [(offset, size, type, name, mask)], to name the differences
    for o, c, n, ow, off, size, dim, mask in u.prop_index().values():
        fields.setdefault(ow, []).append((off, (size or 0) * max(1, dim or 1), c, n, mask))
    chains = {k: u.class_chain(v) for k, v in s.items()}
    pickle.dump({"objs": objs, "scene": s, "data": data, "fields": fields, "chains": chains}, open(args.out, "wb"))
    print(f"snapshot {args.out}: {len(objs)} objects, scene {', '.join(s)}")


def cmd_diff(u_unused, args):
    a, b = pickle.load(open(args.a, "rb")), pickle.load(open(args.b, "rb"))
    ca = collections.Counter(c for c, n in a["objs"].values())
    cb = collections.Counter(c for c, n in b["objs"].values())
    print("object count changes:")
    for c in sorted(set(ca) | set(cb), key=lambda c: -abs(cb[c] - ca[c]))[:25]:
        if cb[c] != ca[c]:
            print(f"  {cb[c] - ca[c]:+5d} {c}")
    names_a = set(n for c, n in a["objs"].values())
    new = sorted(n for c, n in b["objs"].values() if n not in names_a)
    print(f"new objects ({len(new)}):")
    for n in new[:40]:
        print(f"  {n}")
    fields = b["fields"]
    for k in a["data"]:
        if k not in b["data"] or a["scene"].get(k) != b["scene"].get(k):
            print(f"{k}: a different object in b")
            continue
        x, y = a["data"][k] or b"", b["data"][k] or b""
        named = []
        chain = b["chains"].get(k) or list(fields)
        for owner, props in ((ow, fields.get(ow, [])) for ow in chain):
            for off, size, c, n, mask in props:
                if off is None or off + 4 > min(len(x), len(y)):
                    continue
                if c == "BoolProperty" and mask:
                    va, vb = struct.unpack_from("<I", x, off)[0] & mask, struct.unpack_from("<I", y, off)[0] & mask
                    if bool(va) != bool(vb):
                        named.append(f"{owner}.{n} {int(bool(va))}->{int(bool(vb))}")
                elif c in ("IntProperty", "FloatProperty", "ObjectProperty", "ByteProperty") and size:
                    if x[off:off + size] != y[off:off + size]:
                        named.append(f"{owner}.{n} (+0x{off:X})")
        print(f"{k} ({' < '.join(chain[:4])}):")
        for line in named[:40]:
            print(f"  {line}")


def cmd_write_test(u, args):
    p = u.p
    s = scene(u)
    pc, cam = s.get("Controller"), s.get("Camera")
    rot = u.offset_of("Actor", "Rotation")
    if not pc or rot is None:
        raise SystemExit("no controller or Actor.Rotation")
    print(f"controller {u.full(pc)}; waiting {args.wait} s: put the game in front", flush=True)
    time.sleep(args.wait)
    pitch0, yaw0 = struct.unpack("<ii", p.read(pc + rot, 8))

    def show(tag):
        print(f"{time.strftime('%H:%M:%S')} {tag:<10} controller {rotation_text(p, pc + rot)}", flush=True)

    show("start")
    for tag, deg in (("pitch+", args.pitch), ("pitch-", -args.pitch)):
        value = int(round(deg * 65536 / 360)) & 0xFFFF
        end = time.time() + args.hold
        while time.time() < end:
            p.write(pc + rot, struct.pack("<i", value))
            time.sleep(0.016)
        show(tag)
    p.write(pc + rot, struct.pack("<i", pitch0))
    time.sleep(0.5)
    show("pitch back")
    p.write(pc + rot + 4, struct.pack("<i", p.i32(pc + rot + 4) + int(round(args.yaw * 65536 / 360))))
    time.sleep(args.hold)
    show("yaw")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", help="process name, e.g. MassEffect1.exe")
    ap.add_argument("--refresh", action="store_true", help="search the layout again instead of using the cache")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("find")
    o = sub.add_parser("objects")
    o.add_argument("--class", dest="cls")
    o.add_argument("--name")
    pr = sub.add_parser("props")
    pr.add_argument("cls", metavar="Class")
    sub.add_parser("scene")
    sn = sub.add_parser("snap")
    sn.add_argument("out")
    df = sub.add_parser("diff")
    df.add_argument("a")
    df.add_argument("b")
    wt_ = sub.add_parser("write-test")
    wt_.add_argument("--wait", type=float, default=15)
    wt_.add_argument("--hold", type=float, default=3)
    wt_.add_argument("--pitch", type=float, default=20)
    wt_.add_argument("--yaw", type=float, default=30)
    args = ap.parse_args()
    if args.cmd == "diff":
        return cmd_diff(None, args)
    proc = Process(args.exe, write=args.cmd == "write-test")
    u = UE3(proc, refresh=args.refresh)
    {"find": cmd_find, "objects": cmd_objects, "props": cmd_props, "scene": cmd_scene, "snap": cmd_snap,
     "write-test": cmd_write_test}[args.cmd](u, args)


if __name__ == "__main__":
    main()
