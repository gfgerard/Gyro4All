#!/usr/bin/env python3
"""Read-only live memory helpers for a running MSVC x64 game (standard library only, Windows): RTTI vtables, heap
instances, pointers to them, and timed snapshots around a turn. Also imported by the other live tools here.

  python -I tools/idTech/livemem.py <exe> vtable <Class>                 vtable of the complete object (offset 0)
  python -I tools/idTech/livemem.py <exe> find <Class> [<Class>...]      heap instances (qword-aligned vtable hits)
  python -I tools/idTech/livemem.py <exe> ptrs <addr> <size> <Class>     offsets in [addr, addr+size) pointing at one
  python -I tools/idTech/livemem.py <exe> turn <out.json> <addr:size>... snapshots A (still), B (after a turn), C (still)

<exe> is the process's file name (e.g. NewColossus_x64vk.exe); addresses and sizes are hex. A stale object may still
carry a vtable: check another field (e.g. the embedded physics object's vtable) before trusting a hit.
`turn`: 5 s to switch to the game, then a high beep (turn now), 6 s, a middle beep, 1.5 s, a low beep. The game must
stay in front: id Tech games pause when they lose focus. Diff the result with turndiff.py.
"""
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


class NotRunning(Exception):
    pass


class Proc:
    """The running process and its main module. write=True also allows WriteProcessMemory (poke.py only)."""

    def __init__(self, name, write=False):
        snap = k32.CreateToolhelp32Snapshot(2, 0)
        e = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
        ok, self.pid = k32.Process32FirstW(snap, ctypes.byref(e)), None
        while ok:
            if e.szExeFile.lower() == name.lower():
                self.pid = e.th32ProcessID
                break
            ok = k32.Process32NextW(snap, ctypes.byref(e))
        k32.CloseHandle(snap)
        if not self.pid:
            raise NotRunning(f"{name} not running")
        snap = k32.CreateToolhelp32Snapshot(8, self.pid)
        m = MODULEENTRY32W(dwSize=ctypes.sizeof(MODULEENTRY32W))
        ok = k32.Module32FirstW(snap, ctypes.byref(m))
        while ok and m.szModule.lower() != name.lower():
            ok = k32.Module32NextW(snap, ctypes.byref(m))
        k32.CloseHandle(snap)
        self.base, self.size = m.modBaseAddr, m.modBaseSize
        access = 0x400 | 0x10 | ((0x20 | 0x8) if write else 0)  # query, read (+ write, operation)
        self.h = k32.OpenProcess(access, False, self.pid)
        if not self.h:
            raise NotRunning(f"OpenProcess failed ({ctypes.get_last_error()})")

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return buf.raw[:got.value]
        return b""

    def read_padded(self, addr, n, step=0x10000):
        """n bytes, unreadable chunks as zeros."""
        return b"".join(self.read(addr + o, min(step, n - o)).ljust(min(step, n - o), b"\0") for o in range(0, n, step))

    def write(self, addr, data):
        n = ctypes.c_size_t()
        return bool(k32.WriteProcessMemory(self.h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(n)))

    def addr(self, text):
        """A hex address, or exe+<hex offset>."""
        return self.base + int(text[4:], 16) if text.lower().startswith("exe+") else int(text, 16)

    def image(self):
        if not hasattr(self, "_img"):
            self._img = self.read_padded(self.base, self.size, 0x1000)
        return self._img

    def regions(self):
        """Committed private read/write regions (the heap), as (base, size)."""
        addr, m = 0, MBI()
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == 0x1000 and m.Type == 0x20000 and m.Protect in (0x04, 0x40):
                yield m.BaseAddress, m.RegionSize
            addr = m.BaseAddress + m.RegionSize
            if addr >= 1 << 47:
                break

    def vtable(self, cls):
        """The vtable of cls's complete object: TypeDescriptor (".?AV<cls>@@") -> complete object locator with offset 0
        -> the qword pointing at it; the vtable follows that qword."""
        img = self.image()
        i = img.find(f".?AV{cls}@@".encode() + b"\0")
        if i < 0:
            return None
        needle = struct.pack("<I", i - 0x10)
        j = img.find(needle)
        while j >= 0:
            col = j - 12
            if col >= 0 and j % 4 == 0:
                sig, off, _, _, _, self_rva = struct.unpack_from("<IIIIII", img, col)
                if sig == 1 and off == 0 and self_rva == col:
                    pat = struct.pack("<Q", self.base + col)
                    k = img.find(pat)
                    while k >= 0:
                        if k % 8 == 0:
                            return self.base + k + 8
                        k = img.find(pat, k + 1)
            j = img.find(needle, j + 1)
        return None

    def find(self, values):
        """Heap addresses holding each qword value (8-byte aligned)."""
        hits = {v: [] for v in values}
        pats = {struct.pack("<Q", v): v for v in values}
        for b, n in self.regions():
            for off in range(0, n, 1 << 24):
                data = self.read(b + off, min(1 << 24, n - off))
                for p, v in pats.items():
                    k = data.find(p)
                    while k >= 0:
                        if k % 8 == 0:
                            hits[v].append(b + off + k)
                        k = data.find(p, k + 1)
        return hits

    def live_objects(self, cls, inner_cls=None, inner_offset=0):
        """Heap objects of cls; with inner_cls, only those whose qword at inner_offset is inner_cls's vtable."""
        v = self.vtable(cls)
        if not v:
            return []
        hits = self.find([v])[v]
        if not inner_cls:
            return hits
        vi = self.vtable(inner_cls)
        return [a for a in hits if struct.unpack("<Q", self.read(a + inner_offset, 8).ljust(8, b"\0"))[0] == vi]


def beep(freq, ms):
    try:
        import winsound
        winsound.Beep(freq, ms)
    except (ImportError, RuntimeError):
        print("\a", end="", flush=True)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    p = Proc(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "vtable":
        v = p.vtable(sys.argv[3])
        print(f"{sys.argv[3]} vtable {v and hex(v)} (exe+0x{(v or p.base) - p.base:x})")
    elif cmd == "find":
        vts = {c: p.vtable(c) for c in sys.argv[3:]}
        hits = p.find([v for v in vts.values() if v])
        img = p.image()
        for c, v in vts.items():
            if not v:
                print(f"{c}: no vtable")
                continue
            static, pat = [], struct.pack("<Q", v)
            k = img.find(pat)
            while k >= 0 and len(static) < 4:
                if k % 8 == 0:
                    static.append(k)
                k = img.find(pat, k + 1)
            print(f"{c}: vtable exe+0x{v - p.base:x}, {len(hits[v])} heap instance(s): "
                  + " ".join(hex(a) for a in hits[v][:20])
                  + ("; in the exe at " + " ".join(f"exe+0x{k:x}" for k in static) if static else ""))
    elif cmd == "ptrs":
        addr, size = int(sys.argv[3], 16), int(sys.argv[4], 16)
        v = p.vtable(sys.argv[5])
        targets = sorted(p.find([v])[v])
        data = p.read(addr, size)
        for off in range(0, len(data) - 7, 8):
            q = struct.unpack_from("<Q", data, off)[0]
            near = [t for t in targets if t <= q < t + 0x100000]
            if near:
                print(f"+0x{off:x} -> {q:#x} ({sys.argv[5]}+0x{q - max(near):x})")
    elif cmd == "turn":
        spans = [(p.addr(a), int(n, 16)) for a, n in (s.rsplit(":", 1) for s in sys.argv[4:])]
        snap = lambda: {f"{a:x}:{n:x}": p.read_padded(a, n).hex() for a, n in spans}
        out = {}
        print("switch to the game and hold still", flush=True)
        time.sleep(5)
        out["A"] = snap()
        beep(880, 300)
        print("turn now, then stop", flush=True)
        time.sleep(6)
        beep(660, 300)
        out["B"] = snap()
        time.sleep(1.5)
        out["C"] = snap()
        beep(440, 600)
        json.dump(out, open(sys.argv[3], "w"))
        print("done", flush=True)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    try:
        main()
    except NotRunning as e:
        sys.exit(str(e))
