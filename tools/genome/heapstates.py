# heapstates.py: finds what tells a game state (aiming, a menu) apart in a Genome game's heap objects: the camera admin
# and what it points to, and the player entity's property sets and what they point to. Sound-cued and read-only, no
# debugger: at each high beep the player goes into the state (holds aim, opens the menu) and stays; at the low beep, back.
# The objects are snapshotted in both, a few times per round; what is kept: dwords steady within each round that differ
# between the state and idle (exact values, e.g. a mode or a flag), and floats that separate the two cleanly (e.g. a
# camera distance that eases). camtrace/states.py does the same for the exe's static data only. See README.md.
#
#   uv run --with numpy python -I tools/genome/heapstates.py <exe> --root EXPR [--root EXPR...] [--entity EXPR
#       --tables 0x30,0x38] [--state aim] [--cycles 3] [--hold 2.5] [--data] [--out snaps.npz]
#
# EXPR: camtrace addresses (exe+0x1AC2500, pointer paths like [[exe+0x1AC2500+0x80]+8]). ELEX II, the camera admin and
# the player: --root exe+0x1AC2500 --entity "[[exe+0x1AC2500+0x80]+8]" --tables 0x30,0x38 (the module's log line "layout
# from code" names the offsets of a game: the camera target +0x80, its proxy +8 with the handle at +0, the entity's
# property set tables from "player entity ...: property set tables +0x30 / +0x38").
import argparse, os, struct, sys, time, winsound
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "camtrace"))
import camtrace as ct

ROOT_SIZE, OBJ_SIZE, SET_SIZE = 0x1000, 0x400, 0x600


def mark(t): print(f"{time.strftime('%H:%M:%S')} {t}", flush=True)


def fail(t):
    mark("FAILED: " + t)
    for _ in range(3): winsound.Beep(400, 500); time.sleep(0.2)
    sys.exit(1)


def try_addr(p, s):
    try:
        return ct.addr(p, s)
    except SystemExit:
        return 0


def gather(p, a):
    """{name: (address, size)}: the roots (and what they point to, a.depth levels) and the entity's property sets."""
    exe_lo, exe_hi = p.base, p.base + p.size
    heap = lambda v: 0x10000 < v < 0x7fffffffffff and not (exe_lo <= v < exe_hi) and v % 8 == 0
    out, seen = {}, set()

    def follow(name, base, size, depth):
        level = {name: (base, size)}
        for _ in range(depth):
            nxt = {}
            for n, (b, sz) in level.items():
                for off in range(0, sz, 8):
                    v = p.q(b + off) or 0
                    if heap(v) and v not in seen:
                        seen.add(v)
                        nxt[f"{n}+{off:#x}"] = (v, OBJ_SIZE)
            out.update(nxt)
            level = nxt

    for i, expr in enumerate(a.root):
        base = try_addr(p, expr)
        if not base: continue
        name = f"root{i}" if len(a.root) > 1 else "root"
        seen.add(base)
        out[name] = (base, ROOT_SIZE)
        follow(name, base, ROOT_SIZE, a.depth)
    sets = 0
    if a.entity:
        e = try_addr(p, a.entity)
        idx_off, arr_off = (int(x, 16) for x in a.tables.split(","))
        idx, arr = (p.q(e + idx_off) or 0, p.q(e + arr_off) or 0) if e else (0, 0)
        tbl = p.read(idx, 0x101) if idx else None
        if e: out["entity"] = (e, SET_SIZE)
        if tbl and arr:
            for t in range(0x100):
                s = tbl[t + 1] if t + 1 < len(tbl) else 0xFF
                ps = p.q(arr + s * 8) if s != 0xFF else 0
                if not ps or not heap(ps) or ps in seen: continue
                seen.add(ps)
                out[f"ps{t:#04x}"] = (ps, SET_SIZE)
                sets += 1
                follow(f"ps{t:#04x}", ps, SET_SIZE, 1)
    return out, sets


def snap(p, objs, data):
    out = {}
    if data:
        parts = []
        for base, n in p.exe_rw():
            b = p.read(base, n) or b""
            parts.append(np.frombuffer(b + bytes(n - len(b)), np.uint32)[: n // 4])
        out["data"] = np.concatenate(parts)
    for k, (base, n) in objs.items():
        b = p.read(base, n) or b""
        out[k] = np.frombuffer(b + bytes(n - len(b)), np.uint32)
    return out


def analyse(p, snaps, objs, cycles, per_state, per_idle, state):
    f32 = lambda v: struct.unpack("<f", struct.pack("<I", int(v)))[0]
    keys = sorted({k for (_, k) in snaps})
    exact, floats = [], []
    for key in keys:
        idle, st = np.stack(snaps[("idle", key)]), np.stack(snaps[(state, key)])
        I = [idle[per_idle * r:per_idle * (r + 1)] for r in range(cycles + 1)]
        S = [st[per_state * r:per_state * (r + 1)] for r in range(cycles)]
        steady = lambda blk: np.all(blk == blk[0], axis=0)
        passes = [steady(S[r]) & steady(I[r]) & steady(I[r + 1]) & (I[r][0] == I[r + 1][0]) & (S[r][0] != I[r][0])
                  for r in range(cycles)]
        count = np.sum(np.stack(passes), axis=0)
        for i in np.nonzero(count >= 1)[0]:
            exact.append((int(count[i]), key, int(i), [(int(I[r][0][i]), int(S[r][0][i])) for r in range(cycles)]))
        with np.errstate(all="ignore"):
            fi, fs = idle.view(np.float32).astype(np.float64), st.view(np.float32).astype(np.float64)
            ok = np.all(np.isfinite(fi), axis=0) & np.all(np.isfinite(fs), axis=0)
            ok &= (np.abs(fi).max(axis=0) < 1e5) & (np.abs(fs).max(axis=0) < 1e5)
            ok &= (np.abs(fi).max(axis=0) > 1e-4) | (np.abs(fs).max(axis=0) > 1e-4)
            seps, gaps = [], []
            for r in range(cycles):
                Ir = np.concatenate([fi[per_idle * r:per_idle * (r + 1)], fi[per_idle * (r + 1):per_idle * (r + 2)]])
                Sr = fs[per_state * r:per_state * (r + 1)]
                gap = Sr.mean(axis=0) - Ir.mean(axis=0)
                spread = np.maximum(Ir.max(axis=0) - Ir.min(axis=0), Sr.max(axis=0) - Sr.min(axis=0))
                seps.append(np.abs(gap) / (spread + 1e-9))
                gaps.append(gap)
            sign = np.all(np.stack([np.sign(g) == np.sign(gaps[0]) for g in gaps]), axis=0) & (gaps[0] != 0)
            second = np.sort(np.stack(seps), axis=0)[-2] if cycles > 1 else seps[0]  # one round may flicker
        for i in np.nonzero(ok & sign & (second > 3))[0]:
            floats.append((float(second[i]), key, int(i), [float(fi[0][i])] + [float(fs[per_state * r][i]) for r in range(cycles)]))

    def where(key, i):
        if key != "data": return f"{key}+{i * 4:#x} ({objs[key][0] + i * 4:#x})"
        o = i * 4
        for base, n in p.exe_rw():
            if o < n: return p.name(base + o)
            o -= n

    exact.sort(key=lambda r: -r[0])
    # two clean rounds or more; only when there are none, the one-round ones (a round can be spoiled: an early press, a
    # state that flickers)
    strong = [r for r in exact if r[0] >= 2]
    exact = strong or exact
    print(f"== exact: {len(exact)} dwords steady in {'two or more rounds' if strong else 'one round only (weaker)'} and "
          f"different in the {state} ({len(exact[:40])} shown)")
    for c, key, i, vals in exact[:40]:
        print(f"  [{c}/{cycles}] {where(key, i)}: " + " | ".join(f"idle {iv:#x} {state} {sv:#x}" for iv, sv in vals))
    floats.sort(key=lambda r: -r[0])
    print(f"== floats: {len(floats)} that separate cleanly in two rounds, same direction in all ({len(floats[:40])} shown)")
    for s, key, i, v in floats[:40]:
        print(f"  sep {s:7.1f} {where(key, i)}: idle {v[0]:.4g} {state} " + " / ".join(f"{x:.4g}" for x in v[1:]))


def main():
    ap = argparse.ArgumentParser(description="Find what tells a game state apart in a Genome game's heap objects.")
    ap.add_argument("exe")
    ap.add_argument("--root", action="append", default=[], help="an object to snapshot with what it points to (EXPR)")
    ap.add_argument("--depth", type=int, default=2, help="pointer levels followed from each root (default 2)")
    ap.add_argument("--entity", help="the player entity (EXPR): its property sets and what they point to")
    ap.add_argument("--tables", default="0x30,0x38", help="the entity's property set index table and array offsets")
    ap.add_argument("--state", default="aim", help="the state's name in the output (default aim)")
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold", type=float, default=2.5, help="seconds in the state before its snapshots (easing settles)")
    ap.add_argument("--data", action="store_true", help="also the exe's static data")
    ap.add_argument("--out", help="save the snapshots (.npz) for another look")
    a = ap.parse_args()
    if not a.root and not a.entity: ap.error("give at least one --root or --entity")
    mark(f"waiting for {a.exe}")
    for _ in range(900):
        try:
            p = ct.Proc(a.exe)
            break
        except (Exception, SystemExit):
            time.sleep(1)
    else:
        fail(f"{a.exe} didn't start")
    mark("waiting for the game in front (and the player loaded, with --entity)")
    deadline, said = time.time() + 900, time.time()
    while True:
        if time.time() > deadline: fail("not ready within 15 minutes")
        if p.in_front():
            objs, sets = gather(p, a)
            if objs and (not a.entity or sets > 3): break
        if time.time() - said > 30:
            said = time.time()
            mark("  still waiting (load a save, the game in front)")
        time.sleep(1)
    time.sleep(1.5)
    objs, sets = gather(p, a)
    mark(f"{len(objs)} objects ({sets} property sets)")
    per_idle, per_state = 3, 4
    snaps = {}

    def take(label, n, gap):
        for _ in range(n):
            for k, v in snap(p, objs, a.data).items(): snaps.setdefault((label, k), []).append(v)
            time.sleep(gap)

    winsound.Beep(1400, 300)
    mark("start")
    for k in range(a.cycles):
        take("idle", per_idle, 0.4)
        winsound.Beep(1600, 400)
        mark(f"{a.state} {k + 1}")
        time.sleep(a.hold)
        take(a.state, per_state, 0.5)
        winsound.Beep(500, 400)
        mark(f"back {k + 1}")
        time.sleep(a.hold)
    take("idle", per_idle, 0.4)
    for fr in (1200, 1000, 800): winsound.Beep(fr, 150); time.sleep(0.1)
    mark("snapshots done; analysing")
    if a.out:
        np.savez_compressed(a.out, **{f"{l}|{k}": np.stack(v) for (l, k), v in snaps.items()})
        print("saved", a.out, flush=True)
    analyse(p, snaps, objs, a.cycles, per_state, per_idle, a.state)


if __name__ == "__main__":
    main()
