"""Disassemble a managed method of a RUNNING RE Engine game (TDB 71+), read-only: where a getter reads a value, what
a setter writes. For games whose exe code is encrypted on disk (Capcom's anti-tamper; Monster Hunter Wilds), so only
the decrypted code in memory can be read.

How it works: the type and the TDB header come from the exe on disk (tdbdump.py). From TDB 71 a method's code address
is base + encoded_offset (REMethodDefinition +8, filled when the game starts). REFramework finds the base through a
helper in the exe ("85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ..."), which MH Wilds doesn't have; this tool scores every
16-byte aligned base instead: the right one puts nearly all method addresses right after int3/ret padding (MH Wilds
demo: exe+0x4BF2E0, 289 of 291 addresses). It opens the process with PROCESS_VM_READ only and writes nothing.

Usage (the game running; capstone optional, e.g. `uv run --with capstone python -I methoddump.py ...`):
  methoddump.py <exe on disk> --type app.PlayerCameraController                 list the methods
  methoddump.py <exe on disk> --type app.PlayerCameraController --method get_RotationDegree [--bytes 256]
"""
import argparse, ctypes, ctypes.wintypes as wt, os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdbdump  # noqa: E402

PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x0010, 0x0400


class Process:
    """Read-only view of a running process's memory."""

    def __init__(self, exe_name):
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.k32.OpenProcess.restype = wt.HANDLE
        self.k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                               ctypes.POINTER(ctypes.c_size_t)]
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe_name}", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        pids = [int(p.split('","')[1]) for p in out.splitlines() if p.lower().startswith(f'"{exe_name.lower()}"')]
        if not pids:
            raise SystemExit(f"{exe_name} is not running")
        self.h = self.k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pids[0])
        if not self.h:
            raise SystemExit(f"can't open {exe_name} for reading (error {ctypes.get_last_error()})")
        mods = (wt.HMODULE * 1)()
        need = wt.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        self.base = mods[0]

    def read(self, address, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        ok = self.k32.ReadProcessMemory(self.h, ctypes.c_void_p(address), buf, size, ctypes.byref(got))
        return buf.raw[:got.value] if ok else None

    def u16(self, a): return struct.unpack("<H", self.read(a, 2))[0]
    def i32(self, a): return struct.unpack("<i", self.read(a, 4))[0]
    def u32(self, a): return struct.unpack("<I", self.read(a, 4))[0]
    def u64(self, a): return struct.unpack("<Q", self.read(a, 8))[0]


def methods_of(proc, tdb, header, index):
    """(name, encoded_offset) of a type's own methods, from the TDB in memory (TDB 71+ layouts)."""
    types, types_impl, methods = (proc.u64(header + o) for o in (0x68, 0x70, 0x78))
    td = types + index * tdbdump.TYPEDEF_SIZE
    first = tdbdump.bits(proc.u64(td + 0x20), 22, 22)
    count = proc.u16(types_impl + tdbdump.bits(proc.u64(td + 8), 38, 18) * tdbdump.TYPEIMPL_SIZE + 0x18)
    names = [m["name"] for m in tdb.methods(index)]  # names from the file (same tables)
    return [(names[k] if k < len(names) else f"#{k}", proc.i32(methods + (first + k) * 0xC + 8)) for k in range(count)]


def find_base(proc, img, offsets):
    """The 16-byte aligned base (as an RVA) that puts the most method addresses right after int3/ret padding."""
    import numpy as np
    text = next(s for s in img.sections if s[0] == ".text")
    _, rva, size, _, _ = text
    data = bytearray()
    for off in range(0, size, 1 << 24):
        n = min(1 << 24, size - off)
        data += proc.read(proc.base + rva + off, n) or bytes(n)
    code = np.frombuffer(bytes(data), dtype=np.uint8)
    offs = np.array(sorted(set(o for o in offsets if o)), dtype=np.int64)
    lo, hi = rva + 1 - offs.min(), rva + size - 16 - offs.max()
    deltas = np.arange(lo - lo % 16, hi, 16, dtype=np.int64)
    score = np.zeros(len(deltas), dtype=np.int32)
    for o in offs:
        before = code[deltas + o - rva - 1]
        score += (before == 0xCC) | (before == 0xC3)
    best = int(np.argmax(score))
    return int(deltas[best]), int(score[best]), len(offs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", help="the game's exe on disk (for the TDB)")
    ap.add_argument("--type", required=True)
    ap.add_argument("--method", action="append", default=[])
    ap.add_argument("--bytes", type=int, default=256)
    a = ap.parse_args()

    img = tdbdump.Image(a.exe)
    tdb = tdbdump.TDB(img, tdbdump.find_tdb(img))
    if tdb.version < 71:
        raise SystemExit("TDB 69/70 keep plain code pointers: read them with a debugger instead")
    names = tdb.by_name()
    if a.type not in names:
        raise SystemExit(f"{a.type}: not in the TDB")
    proc = Process(os.path.basename(a.exe))
    header = proc.base + (img.va(tdb.off) - img.base)
    own = methods_of(proc, tdb, header, names[a.type])
    if not any(o for _, o in own):
        raise SystemExit("encoded offsets still 0: wait until the game has started")
    # Enough offsets to score the base: this type's and the next few hundred types'.
    offsets = [o for _, o in own]
    for i in range(names[a.type] + 1, min(tdb.n_types, names[a.type] + 400)):
        offsets += [o for _, o in methods_of(proc, tdb, header, i)]
    base, score, total = find_base(proc, img, offsets)
    print(f"TDB {tdb.version}; method base exe+{base:#x} ({score} of {total} addresses after padding)")
    if not a.method:
        for name, o in own:
            print(f"  {name:40} exe+{base + o:#x}" if o else f"  {name:40} (no code)")
        return
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        md = Cs(CS_ARCH_X86, CS_MODE_64)
    except ImportError:
        md = None
    for want in a.method:
        for name, o in own:
            if name != want or not o:
                continue
            address = proc.base + base + o
            code = proc.read(address, a.bytes) or b""
            print(f"== {a.type}.{name} at exe+{base + o:#x}")
            if not md:
                print("  " + code.hex(" "))
                continue
            for ins in md.disasm(code, address):
                note = ""
                if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                    note = f"   ; exe+{int(ins.op_str, 16) - proc.base:#x}"
                print(f"  +{ins.address - address:#05x}  {ins.mnemonic:8} {ins.op_str}{note}")
                if ins.mnemonic in ("ret", "int3"):
                    break


if __name__ == "__main__":
    main()
