# camtrace.py: find where a native game keeps its camera by following the data flow back from any copy of the view,
# with write tests and one-hit hardware write breakpoints (cdb). Engine-neutral, 64-bit Windows games. See README.md.
#
#   uv run --with numpy python -I tools/camtrace/camtrace.py <exe> <command> ...
#
# Addresses: absolute hex (0x1e05ae260), exe-relative (exe+0x30021e0) or pointer paths ([[exe+0x1696ce0]+0x30]+0x9e0).
import argparse, ctypes, ctypes.wintypes as wt, math, os, pickle, re, struct, subprocess, sys, tempfile, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi")
ntdll = ctypes.WinDLL("ntdll")
u32 = ctypes.WinDLL("user32")
PROCESS_ALL_READ = 0x0400 | 0x0010  # query information, VM read
PROCESS_WRITE = 0x0008 | 0x0020  # VM operation, VM write
MEM_COMMIT, MEM_IMAGE = 0x1000, 0x1000000
RW_PROTECT = (0x04, 0x08)  # read/write, write-copy


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


class Proc:
    """A running process by exe name: read, write, regions, thread stacks, exe sections, foreground check."""

    def __init__(self, exe, write=False):
        self.exe = exe
        self.pid = self._find(exe)
        if not self.pid: sys.exit(f"{exe} is not running")
        self.h = k32.OpenProcess(PROCESS_ALL_READ | (PROCESS_WRITE if write else 0), False, self.pid)
        if not self.h: sys.exit(f"can't open {exe} (pid {self.pid}): error {ctypes.get_last_error()}")
        mods = (ctypes.c_void_p * 1)(); n = wt.DWORD()
        psapi.EnumProcessModules(self.h, mods, ctypes.sizeof(mods), ctypes.byref(n))
        self.base = mods[0]
        hdr = self.read(self.base, 0x1000)
        nt = self.base + struct.unpack_from("<I", hdr, 0x3C)[0]
        h = self.read(nt, 0x400)
        nsec, optsize = struct.unpack_from("<H", h, 6)[0], struct.unpack_from("<H", h, 20)[0]
        self.size = struct.unpack_from("<I", h, 24 + 56)[0]
        self.sections = []
        for i in range(nsec):
            o = 24 + optsize + 40 * i
            name = h[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, chars = struct.unpack_from("<II", h, o + 8) + (struct.unpack_from("<I", h, o + 36)[0],)
            self.sections.append((name, va, vsize, chars))

    @staticmethod
    def _find(exe):
        arr = (wt.DWORD * 4096)(); n = wt.DWORD()
        psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(n))
        for pid in arr[: n.value // 4]:
            h = k32.OpenProcess(PROCESS_ALL_READ, False, pid)
            if not h: continue
            buf = ctypes.create_unicode_buffer(260)
            psapi.GetModuleBaseNameW(h, None, buf, 260); k32.CloseHandle(h)
            if buf.value.lower() == exe.lower(): return pid

    def read(self, a, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)): return None
        return buf.raw[: got.value]

    def q(self, a):
        b = self.read(a, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

    def write(self, a, data):
        return bool(k32.WriteProcessMemory(self.h, ctypes.c_void_p(a), data, len(data), None))

    def name(self, a):
        """exe+off for addresses in the exe image, else hex."""
        return f"exe+{a - self.base:#x}" if self.base <= a < self.base + self.size else hex(a)

    def in_front(self):
        pid = ctypes.c_ulong(); u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), ctypes.byref(pid))
        return pid.value == self.pid

    def wait_front(self, timeout=120):
        if self.in_front(): return True
        print(f"waiting for {self.exe} to be the window in front (up to {timeout} s)...", flush=True)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.in_front(): time.sleep(1.5); return True  # games that pause in the background need a moment to resume
            time.sleep(0.2)
        return False

    def regions(self, rw=True):
        a, m = 0, MBI()
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
            base, size = m.BaseAddress or 0, m.RegionSize
            if m.State == MEM_COMMIT and (not rw or m.Protect in RW_PROTECT):
                yield base, size, m.Type, m.AllocationBase or 0
            a = base + size

    def stacks(self):
        """[(lo, hi)] of every thread's stack reservation (the TEB's StackBase down to its allocation)."""
        TH32CS_SNAPTHREAD = 4
        class TE(ctypes.Structure):
            _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                        ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]
        class TBI(ctypes.Structure):
            _fields_ = [("ExitStatus", wt.LONG), ("TebBaseAddress", ctypes.c_void_p), ("UniqueProcess", ctypes.c_void_p),
                        ("UniqueThread", ctypes.c_void_p), ("AffinityMask", ctypes.c_size_t), ("Priority", wt.LONG),
                        ("BasePriority", wt.LONG)]
        k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); te = TE(); te.dwSize = ctypes.sizeof(te); out = []
        ok = k32.Thread32First(snap, ctypes.byref(te))
        while ok:
            if te.th32OwnerProcessID == self.pid:
                th = k32.OpenThread(0x0040, False, te.th32ThreadID)  # THREAD_QUERY_INFORMATION
                if th:
                    tbi = TBI()
                    if ntdll.NtQueryInformationThread(th, 0, ctypes.byref(tbi), ctypes.sizeof(tbi), None) == 0 and tbi.TebBaseAddress:
                        hi, lo = self.q(tbi.TebBaseAddress + 8), self.q(tbi.TebBaseAddress + 16)
                        if hi and lo:
                            m = MBI()
                            if k32.VirtualQueryEx(self.h, ctypes.c_void_p(lo), ctypes.byref(m), ctypes.sizeof(m)):
                                lo = min(lo, m.AllocationBase or lo)
                            out.append((lo, hi))
                    k32.CloseHandle(th)
            ok = k32.Thread32Next(snap, ctypes.byref(te))
        k32.CloseHandle(snap)
        return out

    def exe_rw(self):
        """The exe's writable sections as (start, size)."""
        return [(self.base + va, vs) for n, va, vs, ch in self.sections if ch & 0x80000000]


def addr(p, s):
    """Parse an address: hex, exe+off, or a pointer path with [..] dereferences and +/- offsets."""
    s = s.replace(" ", "")
    def expr(i):
        val, sign = 0, 1
        while i < len(s) and s[i] != "]":
            if s[i] in "+-": sign = 1 if s[i] == "+" else -1; i += 1; continue
            if s[i] == "[":
                inner, i = expr(i + 1)
                if s[i] != "]": sys.exit(f"bad address {s}")
                i += 1; v = p.q(inner)
                if v is None: sys.exit(f"can't read the pointer at {inner:#x} in {s}")
            else:
                m = re.match(r"exe|0x[0-9a-fA-F]+|[0-9a-fA-F]+", s[i:])
                if not m: sys.exit(f"bad address {s}")
                tok = m.group(0); i += len(tok)
                v = p.base if tok == "exe" else int(tok, 16)
            val += sign * v; sign = 1
        return val, i
    v, i = expr(0)
    if i != len(s): sys.exit(f"bad address {s}")
    return v


def heading_pitch(x, y, z):
    """Degrees: heading atan2(x, z) and elevation asin(y) of a direction (axes vary per engine; compare changes)."""
    n = math.sqrt(x * x + y * y + z * z) or 1
    return math.degrees(math.atan2(x, z)), math.degrees(math.asin(max(-1, min(1, y / n))))


# --- snap / vectors: unit 3-vectors in the exe's static data that turned between two poses -----------------------------

def cmd_snap(p, a):
    data = [(s, p.read(s, n)) for s, n in p.exe_rw()]
    if a.heap:
        st = p.stacks()
        for base, size, typ, alloc in p.regions():
            if typ == MEM_IMAGE or size >= 1 << 31 or any(lo <= base < hi for lo, hi in st): continue
            for off in range(0, size, 64 << 20):
                data.append((base + off, p.read(base + off, min(64 << 20, size - off))))
    with open(a.out, "wb") as f: pickle.dump({"pid": p.pid, "base": p.base, "data": data}, f)
    print(f"saved {sum(len(b or b'') for _, b in data) >> 20} MB ({'exe static data + heap' if a.heap else 'exe static data'}) to {a.out}")


def unit_vectors(np, b, stride):
    with np.errstate(all="ignore"):
        f = np.frombuffer(b[: len(b) // 4 * 4], np.float32).astype(np.float64)
    if len(f) < 2 * stride + 1: return {}
    x, y, z = f[: -2 * stride], f[stride: len(f) - stride], f[2 * stride:]
    with np.errstate(all="ignore"):
        ok = np.abs(x * x + y * y + z * z - 1) < 1e-3
        ok &= (np.abs(x) > 1e-3) | (np.abs(z) > 1e-3)
    return {int(i): (x[i], y[i], z[i]) for i in np.nonzero(ok)[0]}


def cmd_vectors(p, a):
    import numpy as np
    A, B = (pickle.load(open(x, "rb")) for x in (a.a, a.b))
    if A["pid"] != B["pid"]: sys.exit("the two snapshots are from different runs of the game")
    hits = []
    for (s, ba), (_, bb) in zip(A["data"], B["data"]):
        if not ba or not bb: continue
        for st in (1, 4):
            va, vb = unit_vectors(np, ba, st), unit_vectors(np, bb, st)
            for i in va.keys() & vb.keys():
                ha, pa = heading_pitch(*va[i]); hb, pb = heading_pitch(*vb[i])
                dh = (hb - ha + 180) % 360 - 180; dp = pb - pa
                if abs(dh) < a.min_turn: continue
                hits.append((s + 4 * i, st, ha, pa, hb, pb, dh, dp))
    # Copies of one view turn the same way: group them. The view is pitched (look some degrees up or down and keep that
    # pitch while turning) while most world objects stay level: pitched groups that kept their pitch come first, then
    # the groups with more copies, then the closest to the turn you made.
    groups = {}
    for h in hits:
        groups.setdefault(tuple(round(x, 1) for x in h[2:6]), []).append(h)
    def rank(g):
        h = g[0]; view_like = min(abs(h[3]), abs(h[5])) >= a.min_pitch and abs(h[7]) < 2
        return (not view_like, -len(g), abs(abs(h[6]) - abs(a.turn)) if a.turn is not None else 0)
    order = sorted(groups.values(), key=rank)
    print(f"{len(hits)} unit vectors turned by >= {a.min_turn} deg, in {len(order)} groups (heading/pitch: before -> after,"
          f" change; pitched groups that kept their pitch first)")
    for g in order[: a.limit]:
        h, st, ha, pa, hb, pb, dh, dp = g[0]
        where = ", ".join(f"{p.name(x[0])} (stride {x[1]})" for x in sorted(g)[: a.show]) + (f", +{len(g) - a.show} more" if len(g) > a.show else "")
        print(f"  {ha:7.1f}/{pa:6.1f} -> {hb:7.1f}/{pb:6.1f}  turn {dh:+7.1f} pitch {dp:+6.1f}  x{len(g)}: {where}")


# --- find-vec: every copy of a direction in memory, thread stacks marked ---------------------------------------------

def cmd_find_vec(p, a):
    import numpy as np
    if re.fullmatch(r"\s*-?[\d.]+\s*,\s*-?[\d.]+\s*,\s*-?[\d.]+\s*", a.vec):
        v = np.array([float(x) for x in a.vec.split(",")])
    else:  # an address: the direction it holds right now (the view may sway while the game runs)
        v = np.array(struct.unpack("<3f", p.read(addr(p, a.vec), 12)))
        print("searching for %.4f, %.4f, %.4f" % tuple(v))
    st = p.stacks(); hits = []
    for base, size, typ, alloc in p.regions():
        if size >= 1 << 31: continue
        for off in range(0, size, 64 << 20):
            b = p.read(base + off, min(64 << 20, size - off))
            if not b: continue
            f = np.frombuffer(b[: len(b) // 4 * 4], np.float32)
            for stride in (1, 2, 4):
                L = 2 * stride + 1; N = len(f) - L + 1
                if N <= 0: continue
                with np.errstate(all="ignore"):
                    m = np.abs(f[:N] - v[0]) < a.tol
                    for j in range(1, 3): m &= np.abs(f[j * stride: j * stride + N] - v[j]) < a.tol
                for i in np.nonzero(m)[0]:
                    h = base + off + 4 * int(i)
                    hits.append((h, stride, "stack" if any(lo <= h < hi for lo, hi in st) else ("exe" if typ == MEM_IMAGE else "heap")))
    order = {"exe": 0, "heap": 1, "stack": 2}
    hits.sort(key=lambda h: order[h[2]])
    print(f"{len(hits)} copies ({sum(h[2] == 'stack' for h in hits)} on thread stacks: temporary, never a breakpoint target)")
    for h, stride, where in hits[: a.limit]:
        print(f"  {where:5s} {p.name(h):18s} stride {stride}")


# --- dump / watch ------------------------------------------------------------------------------------------------------

def cmd_dump(p, a):
    at = addr(p, a.addr)
    for r in range(at - a.before, at + a.size, 16):
        b = p.read(r, 16)
        if not b or len(b) < 16: print(f"{p.name(r):18s} ??"); continue
        fl = struct.unpack("<4f", b); qs = struct.unpack("<2Q", b)
        print(f"{p.name(r):18s} {'>' if r == at else ' '} " + " ".join(f"{x:11.4f}" for x in fl) + "   " + " ".join(f"{x:#018x}" for x in qs))


def cmd_watch(p, a):
    at = addr(p, a.addr); fmt = {"f32": "f", "f64": "d", "i32": "i"}[a.type]; n = struct.calcsize(fmt) * a.count
    print(f"watching {p.name(at)} ({a.count} x {a.type}) for {a.seconds} s; values print when they change")
    last, t0 = None, time.time()
    while time.time() - t0 < a.seconds:
        b = p.read(at, n)
        v = struct.unpack(f"<{a.count}{fmt}", b) if b and len(b) == n else None
        if v != last: print(f"{time.time() - t0:6.2f}s", " ".join(f"{x:.4f}" if isinstance(x, float) else str(x) for x in v) if v else "unreadable"); last = v
        time.sleep(a.interval)


# --- write-test: write once with the game in front, see whether the game keeps it -------------------------------------

def cmd_write_test(p, a):
    at = addr(p, a.addr)
    if not p.wait_front(): sys.exit("the game never came to the front: nothing written")
    if a.yaw is not None:
        x, y, z = struct.unpack("<3f", p.read(at, 12)); c, s = math.cos(math.radians(a.yaw)), math.sin(math.radians(a.yaw))
        new = struct.pack("<3f", c * x + s * z, y, -s * x + c * z); shown = lambda b: "heading %.1f pitch %.1f" % heading_pitch(*struct.unpack("<3f", b))
        n = 12
    else:
        fmt = {"f32": "f", "f64": "d"}[a.type]; n = struct.calcsize(fmt)
        new = struct.pack("<" + fmt, struct.unpack("<" + fmt, p.read(at, n))[0] + a.add); shown = lambda b: "%.5f" % struct.unpack("<" + fmt, b)[0]
    watch = [addr(p, w) for w in a.watch or []]
    def line(): return shown(p.read(at, n)) + "".join(f" | {p.name(w)} " + " ".join("%.4f" % x for x in struct.unpack("<3f", p.read(w, 12))) for w in watch)
    print("before:  ", line())
    if not p.write(at, new): sys.exit(f"write failed: error {ctypes.get_last_error()}")
    for dt in (0.03, 0.3, 1.0):
        time.sleep(dt); print(f"+{dt:4.2f} s:  ", line())
    print("kept = a source (or a copy nothing rewrites: check the screen moved); undone within a frame = a copy: put a"
          " write breakpoint on it (bp)")


# --- bp: one-hit hardware write breakpoint through cdb, then detach ----------------------------------------------------

def find_cdb():
    for c in (os.environ.get("CDB"), os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WindowsApps\cdbX64.exe"),
              r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"):
        if c and os.path.exists(c): return c
    sys.exit("cdb not found: install WinDbg (Microsoft Store) or the Windows SDK debuggers, or set CDB")


def cmd_bp(p, a):
    at = addr(p, a.addr)
    kind = "e1" if a.exec else f"w{a.len}"  # --exec: when that instruction runs (e.g. right after a getter returns)
    if not a.exec and at % a.len: sys.exit(f"a {a.len}-byte breakpoint needs a {a.len}-aligned address ({at:#x})")
    out = a.out or os.path.join(tempfile.gettempdir(), f"camtrace-bp-{p.pid}-{at:x}.txt")
    script = os.path.join(tempfile.gettempdir(), f"camtrace-bp-{p.pid}.cdb")
    act = f'.echo CAMTRACE-HIT; r; kn 30; u @rip-0x{a.back:x} L{a.lines}; dq @rsp L40; .detach; q'
    if a.skip:  # writes from this code range (e.g. a mod's own DLL writing the same value) don't count
        lo, size = (int(x, 16) for x in a.skip.split(":"))
        act = f".if ((@rip >= 0x{lo:x}) & (@rip < 0x{lo + size:x})) {{gc}} .else {{{act}}}"
    if a.cond:  # only hits where this cdb expression is true, e.g. "poi(@rcx+0x70) == 0x22b1cff4c00"
        act = f".if ({a.cond}) {{{act}}} .else {{gc}}"
    with open(script, "w") as f:
        f.write("\n".join(["sxi *", "sxi ld", "sxi ud", "sxi ct", "sxi et", "sxi cpr", "sxi epr",
                           f'ba {kind} {at:#x} "{act}"', "g", ".echo CAMTRACE-NOT-HIT", ".detach", "q", ""]))
    print(f"breakpoint on {'execution of' if a.exec else 'writes to'} {p.name(at)} ({at:#x}); switch to the game (it"
          f" fires on the next {'run' if a.exec else 'write'}, one hit, then the debugger detaches). Output: {out}", flush=True)
    with open(out, "w") as f:
        r = subprocess.run([find_cdb(), "-pd", "-p", str(p.pid), "-cf", script], stdout=f, stderr=subprocess.STDOUT)
    text = open(out, errors="replace").read()
    # The marker on a line of its own: cdb also echoes the breakpoint command, which contains it.
    m = re.search(r"^CAMTRACE-HIT\s*$", text, re.M)
    if not m: sys.exit(f"no hit (cdb exit {r.returncode}); see {out}")
    hit = text[m.end():]
    regs = dict(re.findall(r"\b(r[a-z0-9]{1,2})=([0-9a-f]{16})", hit))
    rip = int(regs.get("rip", "0"), 16)
    print(f"HIT: {'at' if a.exec else 'written by'} {p.name(rip)}")
    print("registers (exe-relative where they point into the exe):")
    print("  " + "  ".join(f"{k}={p.name(int(v, 16))}" for k, v in regs.items() if k != "rip"))
    print("call stack (return addresses):")
    for m in re.finditer(r"^[0-9a-f]{2} [0-9a-f`]+ ([0-9a-f`]{17})", hit, re.M):
        print("  " + p.name(int(m.group(1).replace("`", ""), 16)))
    print(f"disassembly before the write and the stack are in {out}")


# --- refs / sig: offline, the exe file (pefile + capstone) -------------------------------------------------------------

def load_pe(path):
    import pefile
    pe = pefile.PE(path, fast_load=True); return pe, pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase


def cmd_refs(p, a):
    import numpy as np, capstone
    pe, img, ib = load_pe(a.exe)
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for s in pe.sections:
        if not s.Characteristics & 0x20000000: continue
        tva = s.VirtualAddress; b = img[tva:tva + s.Misc_VirtualSize]
        u = np.frombuffer(b + b"\0" * 8, np.uint8).astype(np.int64)
        disp = u[:-7] | (u[1:-6] << 8) | (u[2:-5] << 16) | (u[3:-4] << 24); disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
        pos = np.arange(len(disp)) + tva; seen = set()
        for k in range(5):
            t = pos + 4 + k + disp
            for i in np.nonzero((t >= lo) & (t < hi))[0]:
                for back in range(7, 0, -1):  # longest decode first: keeps REX prefixes
                    st = int(i) - back
                    ins = next(md.disasm(b[st:st + 16], ib + tva + st), None)
                    if ins and ins.size == back + 4 + k and "rip" in ins.op_str and tva + st not in seen:
                        seen.add(tva + st); print(f"{tva + st:#x}: {ins.mnemonic} {ins.op_str}  -> {tva + st + ins.size + int(disp[i]):#x}"); break


def cmd_sig(p, a):
    pe, img, ib = load_pe(a.exe)
    pat = re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in a.pattern.split()), re.S)
    targets = set()
    for s in pe.sections:
        if not s.Characteristics & 0x20000000: continue
        lo = s.VirtualAddress; b = img[lo:lo + s.Misc_VirtualSize]
        for m in pat.finditer(b):
            r = lo + m.start(); d = int.from_bytes(b[m.start() + a.disp:m.start() + a.disp + 4], "little", signed=True)
            targets.add(r + a.end + d); print(f"match at {r:#x} -> {r + a.end + d:#x}")
    print(f"{len(targets)} different target(s): {'usable' if len(targets) == 1 else 'not usable as is'}")


def main():
    ap = argparse.ArgumentParser(description="Find where a native game keeps its camera (see README.md).")
    ap.add_argument("exe", help="the game's exe name (for refs/sig: the exe file's path)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snap", help="snapshot the exe's static data (and --heap) to a file"); s.add_argument("out"); s.add_argument("--heap", action="store_true")
    s = sub.add_parser("vectors", help="unit vectors that turned between two snapshots"); s.add_argument("a"); s.add_argument("b")
    s.add_argument("--min-turn", type=float, default=20); s.add_argument("--min-pitch", type=float, default=3); s.add_argument("--show", type=int, default=40); s.add_argument("--turn", type=float, help="the turn you made (sorts by it)"); s.add_argument("--limit", type=int, default=40)
    s = sub.add_parser("find-vec", help="every copy of a direction (x,y,z, or an address holding one) in memory"); s.add_argument("vec"); s.add_argument("--tol", type=float, default=2e-3); s.add_argument("--limit", type=int, default=60)
    s = sub.add_parser("dump", help="memory around an address"); s.add_argument("addr"); s.add_argument("--size", type=lambda x: int(x, 0), default=0x80); s.add_argument("--before", type=lambda x: int(x, 0), default=0x20)
    s = sub.add_parser("watch", help="print values while you move"); s.add_argument("addr"); s.add_argument("--count", type=int, default=3)
    s.add_argument("--type", choices=["f32", "f64", "i32"], default="f32"); s.add_argument("--seconds", type=float, default=30); s.add_argument("--interval", type=float, default=0.1)
    s = sub.add_parser("write-test", help="write once with the game in front; kept or undone?"); s.add_argument("addr")
    g = s.add_mutually_exclusive_group(required=True); g.add_argument("--add", type=float, help="add to a float (e.g. 0.35 rad)"); g.add_argument("--yaw", type=float, help="turn a direction vector about the vertical by degrees")
    s.add_argument("--type", choices=["f32", "f64"], default="f32"); s.add_argument("--watch", action="append", help="also show this vec3 (e.g. the view's forward)")
    s = sub.add_parser("bp", help="one-hit hardware write breakpoint (cdb -pd), then detach"); s.add_argument("addr")
    s.add_argument("--len", type=int, choices=[1, 2, 4, 8], default=4); s.add_argument("--back", type=lambda x: int(x, 0), default=0x180); s.add_argument("--lines", type=int, default=110); s.add_argument("--out")
    s.add_argument("--skip", help="base:size (hex) of code whose writes are ignored, e.g. a mod DLL's")
    s.add_argument("--exec", action="store_true", help="break when the instruction at addr runs instead (read registers there)")
    s.add_argument("--cond", help="cdb expression: hits where it is false are let go (e.g. \"poi(@rcx+0x70) == 0x...\")")
    s = sub.add_parser("refs", help="offline: code with RIP-relative refs into [lo, hi) RVAs"); s.add_argument("lo"); s.add_argument("hi")
    s = sub.add_parser("sig", help="offline: a code pattern's matches and the RVA each RIP-relative disp resolves to"); s.add_argument("pattern")
    s.add_argument("--disp", type=int, default=3); s.add_argument("--end", type=int, default=7)
    a = ap.parse_args()
    offline = a.cmd in ("refs", "sig")
    p = None if offline else Proc(a.exe, write=a.cmd == "write-test")
    {"snap": cmd_snap, "vectors": cmd_vectors, "find-vec": cmd_find_vec, "dump": cmd_dump, "watch": cmd_watch,
     "write-test": cmd_write_test, "bp": cmd_bp, "refs": cmd_refs, "sig": cmd_sig}[a.cmd](p, a)


if __name__ == "__main__":
    main()
