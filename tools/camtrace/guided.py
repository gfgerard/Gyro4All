# guided.py: find where a native game keeps its view from a few sound-cued poses, read-only and without saving any
# snapshot to disk. The user turns (and looks up or down) at a high beep and stays still at two low beeps; the game's
# memory is compared pose to pose and only the values that followed the view are kept. Engine-neutral, 64-bit Windows
# games; safe on games with anti-debugging (no debugger, no writes). See README.md, "Guided search".
#
#   uv run --with numpy python -I tools/camtrace/guided.py <exe> [--ref ADDR] [--eye ADDR] [--poses 6] [--out FILE]
#
# Poses 0-2 are full snapshots (the exe's static data and the heap, in memory: about 2x the game's working set), the
# rest re-read only the candidates. The view (heading, pitch) at each pose comes from a forward vector: --ref, or found
# by itself (the biggest group of identical unit vectors that turned in both of the first moves).
import argparse, math, os, struct, sys, time, warnings, winsound
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import camtrace as ct
warnings.simplefilter("ignore", RuntimeWarning)  # NaN and huge values in game memory

ANGLE_PERIODS = {"f4": (1.0, 2.0, 360.0, 2 * math.pi, 400.0, 3600.0, 4096.0, 16384.0, 65536.0),
                 "f8": (1.0, 2.0, 360.0, 2 * math.pi, 400.0, 3600.0, 4096.0, 16384.0, 65536.0),
                 "i2": (4096.0, 16384.0, 65536.0, 3600.0), "i4": (4096.0, 16384.0, 65536.0, 3600.0, 36000.0, 2.0 ** 32)}
LAYOUTS = {"rows": (0, 1, 2), "columns": (0, 4, 8), "columns3": (0, 3, 6)}
TOL = 0.8      # degrees, angles and directions
RTOL = 0.03    # relative, values proportional to the turn
MIN_MOVE = 5   # degrees each of the first two moves must change yaw (pitch) by for the yaw (pitch) tests


def wrap(d, period):
    return (d + period / 2) % period - period / 2


def arr(buf, dt):
    isz = np.dtype(dt).itemsize
    return np.frombuffer(buf[: len(buf) // isz * isz], dt)


def take(a, idx, offs):
    return np.stack([a[idx + o].astype(np.float64) for o in offs], 1)


def rot(h, pt):
    h, pt = math.radians(h), math.radians(pt); ch, sh, cp, sp = math.cos(h), math.sin(h), math.cos(pt), math.sin(pt)
    return np.array([[ch, 0, sh], [0, 1, 0], [-sh, 0, ch]]) @ np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])


class Move:
    """The view's change between two poses: yaw, pitch, the camera's rotation angle, and the eye before/after."""
    def __init__(self, a, b):
        self.dh, self.dp = wrap(b[0] - a[0], 360.0), b[1] - a[1]
        self.rot = math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(rot(*b[:2]) @ rot(*a[:2]).T) - 1) / 2))))
        self.e0, self.e1 = a[2], b[2]


# A test: (kind, dtype, element offsets, period or 0, sign, axis). Kinds: "angle" (a value in a known unit, wrapped),
# "ratio" (any unit: change / turn constant), "dir" (unit direction), "quat" (unit quaternion, camera or yaw-only
# rotation), "vector" (non-unit direction, constant length), "aim" (a point whose direction from the eye follows).
def make_tests(eye):
    t = []
    for dt, periods in ANGLE_PERIODS.items():
        for axis in ("yaw", "pitch"):
            t += [("angle", dt, (0,), P, sg, axis) for P in periods for sg in (1, -1)]
            t.append(("ratio", dt, (0,), 0, 1, axis))
    for dt in ("f4", "f8"):
        for offs in LAYOUTS.values():
            t += [("dir", dt, offs, 0, 1, "yaw"), ("vector", dt, offs, 0, 1, "yaw")] + ([("aim", dt, offs, 0, 1, "yaw")] if eye else [])
        t += [("quat", dt, (0, 1, 2, 3), 0, 1, "cam"), ("quat", dt, (0, 1, 2, 3), 0, 1, "yaw")]
    return t


def name(test):
    kind, dt, offs, P, sg, axis = test
    if kind == "angle": return f"{axis} angle {dt}, {P:g} per turn, sign {sg:+d}"
    if kind == "ratio": return f"{axis} in any unit {dt}"
    if kind == "quat": return f"quaternion ({axis} rotation) {dt}"
    lay = next(k for k, v in LAYOUTS.items() if v == offs)
    return {"dir": "direction", "vector": "non-unit direction", "aim": "aim point"}[kind] + f" ({lay}) {dt}"


def check(test, v0, v1, mv, r):
    """Keep mask for one move, and the per-candidate ratio (ratio tests: set on the first move, then checked)."""
    kind, dt, offs, P, sg, axis = test
    with np.errstate(all="ignore"):
        if kind in ("angle", "ratio"):
            t = mv.dh if axis == "yaw" else mv.dp; d = v1[:, 0] - v0[:, 0]
            if dt == "i2": d = wrap(d, 65536.0)
            if kind == "angle":
                k = P / 360
                return np.isfinite(d) & (np.abs(wrap(d, P) - sg * t * k) < TOL * k), r
            if r is None:
                ok = np.isfinite(d) & (np.abs(v1[:, 0]) < 1e9) & (d != 0) & (np.abs(d) > 1e-5 * (np.abs(v0[:, 0]) + 1e-3))
                return ok, d / t
            return np.isfinite(d) & (np.abs(d - r * t) < RTOL * np.abs(r) * max(abs(t), MIN_MOVE)), r
        if kind == "quat":
            ok = np.ones(len(v0), bool)
            for v in (v0, v1): ok &= (np.abs((v * v).sum(1) - 1) < 2e-3) & ((np.abs(v) > 0.02).sum(1) >= 2)
            ang = mv.rot if axis == "cam" else abs(mv.dh)
            c = math.cos(math.radians(ang) / 2); s = max(math.sin(math.radians(ang) / 2) * math.radians(TOL) / 2, 2e-4)
            return ok & (np.abs(np.abs((v0 * v1).sum(1)) - c) < s), r
        if kind == "aim": v0, v1 = v0 - mv.e0, v1 - mv.e1
        l0, l1 = np.sqrt((v0 * v0).sum(1)), np.sqrt((v1 * v1).sum(1))
        if kind == "dir": ok = (np.abs(l0 - 1) < 1e-3) & (np.abs(l1 - 1) < 1e-3)
        else: ok = (l0 > 0.05) & (l0 < 1e5) & (np.abs(l0 - 1) > 5e-3) & (np.abs(l1 - l0) < 0.02 * l0 + (0.3 if kind == "aim" else 0))
        ok &= np.hypot(v0[:, 0], v0[:, 2]) > 0.3 * l0
        if abs(mv.dh) < 3: return ok & np.all(np.abs(v1 - v0) < 1e-3 * (np.abs(v0) + 1), 1), r
        d = wrap(np.degrees(np.arctan2(v1[:, 0], v1[:, 2]) - np.arctan2(v0[:, 0], v0[:, 2])), 360.0)
        return ok & ((np.abs(d - mv.dh) < TOL) | (np.abs(d + mv.dh) < TOL)), r


def main():
    ap = argparse.ArgumentParser(description="Sound-guided, read-only search for a native game's view (see README.md).")
    ap.add_argument("exe", help="the game's exe name")
    ap.add_argument("--ref", help="a forward-vector copy of the view (default: found from the first moves)")
    ap.add_argument("--eye", help="the eye position (x, y, z floats), for the aim-point test (e.g. right after --ref)")
    ap.add_argument("--poses", type=int, default=6, help="moves after the first snapshot (at least 3)")
    ap.add_argument("--move", type=float, default=2.5, help="seconds to turn after the high beep")
    ap.add_argument("--exe-only", action="store_true", help="only the exe's static data (fast; misses heap objects)")
    ap.add_argument("--out", help="also write the log to this file")
    a = ap.parse_args()
    p = ct.Proc(a.exe); out = open(a.out, "w") if a.out else None
    def log(*x):
        s = " ".join(str(v) for v in x); print(s, flush=True)
        if out: out.write(s + "\n"); out.flush()
    ref = ct.addr(p, a.ref) if a.ref else None; eye = ct.addr(p, a.eye) if a.eye else None
    tests = make_tests(eye)

    def chunks():
        for s, n in p.exe_rw():
            for off in range(0, n, 64 << 20): yield s + off, min(64 << 20, n - off)
        if a.exe_only: return
        st = p.stacks()
        for base, size, typ, alloc in p.regions():
            if typ == ct.MEM_IMAGE or size >= 1 << 31 or any(lo <= base < hi for lo, hi in st): continue
            for off in range(0, size, 64 << 20): yield base + off, min(64 << 20, size - off)

    def snapshot():
        return {s: b for s, n in chunks() if (b := p.read(s, n))}

    def eye_now():
        return np.array(struct.unpack("<3f", p.read(eye, 12))) if eye else np.zeros(3)

    def view_of(vec, e):
        return ct.heading_pitch(*vec) + (e,)

    def view_now():
        return view_of(struct.unpack("<3f", p.read(ref, 12)), eye_now()) if ref else None

    def move():
        winsound.Beep(1400, 350); time.sleep(a.move)
        winsound.Beep(600, 180); time.sleep(0.08); winsound.Beep(600, 180); time.sleep(0.7)

    log("Moves: at each high beep turn left or right (30-170 degrees) AND look up or down (15-60 degrees), "
        "alternating; stay still at the two low beeps until the next high beep. The first two moves matter most.")
    log(f"waiting for {a.exe} in front")
    if not p.wait_front(timeout=900): sys.exit("the game never came to the front")
    for f in (800, 1000, 1200): winsound.Beep(f, 150); time.sleep(0.25)
    time.sleep(0.8)
    t = time.time(); s0 = snapshot(); e0 = eye_now(); v0 = view_now()
    log(f"pose 0: {sum(map(len, s0.values())) >> 20} MB in {time.time() - t:.1f} s")
    move(); s1 = snapshot(); e1 = eye_now(); v1 = view_now()
    move(); t = time.time(); e2 = eye_now(); v2 = view_now()
    common = [s for s in s0 if s in s1]

    if ref is None:  # the view's copies: identical unit vectors that turned >= 10 degrees in both moves
        groups = {}
        for s in common:
            b2 = p.read(s, len(s0[s]))
            if not b2: continue
            n = min(len(s0[s]), len(s1[s]), len(b2))
            f = [arr(b[:n], "f4").astype(np.float64) for b in (s0[s], s1[s], b2)]
            m = len(f[0]) - 2
            if m <= 0: continue
            with np.errstate(all="ignore"):
                ok = np.ones(m, bool); hs = []
                for x in f:  # unit, and not near vertical (an up vector's heading is noise)
                    ok &= np.abs(x[:m] ** 2 + x[1:m + 1] ** 2 + x[2:m + 2] ** 2 - 1) < 1e-3
                    ok &= x[:m] ** 2 + x[2:m + 2] ** 2 > 0.25; hs.append(np.degrees(np.arctan2(x[:m], x[2:m + 2])))
                ok &= (np.abs(wrap(hs[1] - hs[0], 360.0)) >= 10) & (np.abs(wrap(hs[2] - hs[1], 360.0)) >= 10)
            for i in np.nonzero(ok)[0]:
                key = tuple(round(float(x[i + j]), 3) for x in f for j in range(3))
                groups.setdefault(key, []).append(s + 4 * int(i))
        if not groups: sys.exit("no unit vector turned in both moves: turn more, or give --ref")
        # The forward vector changes pitch too; the camera's right vector (also turned by the yaw) stays level.
        def pitches(key): return [ct.heading_pitch(*key[3 * k: 3 * k + 3])[1] for k in range(3)]
        def pitched(key): ps = pitches(key); return max(abs(ps[1] - ps[0]), abs(ps[2] - ps[1])) >= MIN_MOVE
        best = sorted(groups.items(), key=lambda g: (not pitched(g[0]), -len(g[1]), min(g[1])))
        for key, where in best[:5]:
            hp = [ct.heading_pitch(*key[3 * k: 3 * k + 3]) for k in range(3)]
            log(f"view copies? x{len(where)} (" + " -> ".join(f"{h:.0f}/{pt:.0f}" for h, pt in hp) + "): "
                + ", ".join(p.name(w) for w in sorted(where)[:8]))
        key, where = best[0]
        ref = sorted(where, key=lambda w: (not (p.base <= w < p.base + 0x40000000), w))[0]
        v0, v1, v2 = (view_of(key[3 * k: 3 * k + 3], e) for k, e in ((0, e0), (1, e1), (2, e2)))
        log(f"reference: {p.name(ref)} ({len(where)} copies)")
    m1, m2 = Move(v0, v1), Move(v1, v2)
    log(f"views: {v0[0]:.1f}/{v0[1]:.1f} -> {v1[0]:.1f}/{v1[1]:.1f} -> {v2[0]:.1f}/{v2[1]:.1f} "
        f"(yaw {m1.dh:+.1f} {m2.dh:+.1f}, pitch {m1.dp:+.1f} {m2.dp:+.1f})")
    for axis, ch in (("yaw", (m1.dh, m2.dh)), ("pitch", (m1.dp, m2.dp))):
        if min(map(abs, ch)) < MIN_MOVE:
            log(f"the first two moves didn't change {axis} by {MIN_MOVE} degrees each: {axis} tests dropped")
            tests = [x for x in tests if not (x[5] == axis or axis == "yaw" and x[0] in ("dir", "vector", "aim", "quat"))]

    t = time.time(); cand = {x: {} for x in tests}  # test -> {chunk: (indices, last values, ratios)}
    for s in common:
        b2 = p.read(s, len(s0[s]))
        if not b2: continue
        n = min(len(s0[s]), len(s1[s]), len(b2))
        for dt in ("f4", "f8", "i2", "i4"):
            mine = [x for x in tests if x[1] == dt]
            if not mine: continue
            raw = [arr(b[:n], dt) for b in (s0[s], s1[s], b2)]
            f = [x.astype(np.float64) for x in raw]
            with np.errstate(all="ignore"):
                moved = (raw[0] != raw[1]) & np.isfinite(f[0]) & np.isfinite(f[1]) & np.isfinite(f[2])
                unit = {}
                for x in mine:
                    if x[0] in ("dir", "quat") and x[2] not in unit:
                        mm = len(f[1]) - max(x[2])
                        unit[x[2]] = np.nonzero(np.abs(sum(f[1][o: o + mm] ** 2 for o in x[2]) - 1) < 2e-3)[0] if mm > 0 else np.array([], int)
            moved_idx = np.nonzero(moved)[0]
            for x in mine:
                idx = unit[x[2]] if x[0] in ("dir", "quat") else moved_idx[moved_idx < len(f[0]) - max(x[2])]
                if not len(idx): continue
                w = [take(y, idx, x[2]) for y in f]
                ok, r = check(x, w[0], w[1], m1, None)
                idx, w, r = idx[ok], [y[ok] for y in w], (r[ok] if r is not None else None)
                if not len(idx): continue
                ok, r = check(x, w[1], w[2], m2, r)
                if ok.any(): cand[x][s] = (idx[ok], w[2][ok], r[ok] if r is not None else None)
    sizes = {c: len(s0[c]) for c in common}
    del s0, s1
    count = lambda: {name(x): sum(len(c[0]) for c in v.values()) for x, v in cand.items() if v}
    log(f"after pose 2 ({time.time() - t:.0f} s): {count()}")
    prev = v2

    def refresh():
        # Right before a move: the view and every candidate as they are now. A view that drifts while the user waits
        # (RDR2's camera recenters behind the character; pose 2's comparison takes minutes) otherwise drops them all.
        bufs = {}
        for x in cand:
            for s, (idx, last, r) in list(cand[x].items()):
                if s not in bufs: bufs[s] = p.read(s, sizes[s])
                if not bufs[s]: continue
                a_ = arr(bufs[s], x[1]); keep = idx + max(x[2]) < len(a_)
                cand[x][s] = (idx[keep], take(a_, idx[keep], x[2]), r[keep] if r is not None else None)
        return view_now()

    for k in range(3, a.poses + 1):
        now_v = refresh()
        if abs(wrap(now_v[0] - prev[0], 360.0)) > TOL or abs(now_v[1] - prev[1]) > TOL:
            log(f"the view drifted while waiting: {prev[0]:.1f}/{prev[1]:.1f} -> {now_v[0]:.1f}/{now_v[1]:.1f}")
        prev = now_v
        move(); cur = view_now(); mv = Move(prev, cur); bufs = {}
        for x in cand:
            nxt = {}
            for s, (idx, last, r) in cand[x].items():
                if s not in bufs: bufs[s] = p.read(s, sizes[s])
                b = bufs[s]
                if not b: continue
                a_ = arr(b, x[1]); keep = idx + max(x[2]) < len(a_)
                idx, last, r = idx[keep], last[keep], (r[keep] if r is not None else None)
                now = take(a_, idx, x[2])
                ok, r = check(x, last, now, mv, r)
                if ok.any(): nxt[s] = (idx[ok], now[ok], r[ok] if r is not None else None)
            cand[x] = nxt
        log(f"pose {k}: {cur[0]:.1f}/{cur[1]:.1f} (yaw {mv.dh:+.1f}, pitch {mv.dp:+.1f}): {count()}")
        prev = cur
    for f in (1200, 1000, 800): winsound.Beep(f, 150); time.sleep(0.1)

    log("\nWhat followed the view through every move. Copies of the view (directions, quaternions) are expected; a "
        "source is what keeps a write: try each with camtrace's write-test (--yaw for directions, --add for angles).")
    for x, c in cand.items():
        rows = [(s + int(i) * np.dtype(x[1]).itemsize, v, rr[j] if rr is not None else None)
                for s, (idx, vals, rr) in c.items() for j, (i, v) in enumerate(zip(idx, vals))]
        if not rows: continue
        log(f"== {name(x)}: {len(rows)}")
        for ad, v, r in sorted(rows, key=lambda y: y[0])[:40]:
            log(f"  {p.name(ad):18s}", " ".join(f"{y:.4f}" for y in v), "" if r is None else f"({r:.6g} per degree)")
    log("done")


if __name__ == "__main__":
    main()
