#!/usr/bin/env python3
"""Offsets in a record.py recording whose values follow the turns, grouped by identical change patterns: degrees
(wrapped), radians/unit values, and shorts (SHORT2ANGLE command angles). Needs numpy:

  uv run --with numpy python -I tools/idTech/analyse.py <scratchpad>/rec.pkl

Each group line: total change, first -> last value, one character per frame (+ rising, - falling, . still) and the
offsets that move the same way ("(inv)": the opposite way). Compare the pattern with the moves you did: a yaw falls
while turning right (id Tech: yaw positive left) and holds during up/down; a pitch the other way round. Copies of one
angle (view, input, a delta apart) share a group; series.py prints their values.
"""
import pickle
import sys

import numpy as np


def groups(F, D, ok, thr):
    s = np.sum(np.abs(D), axis=0)
    idx = np.where(ok & (s > thr))[0]
    out = []
    for i in idx[np.argsort(-s[idx])][:400]:
        d = D[:, i]
        for g in out:
            if np.std(d) and np.std(g[1]) and abs(np.corrcoef(g[1], d)[0, 1]) > 0.98:
                g[2].append((i, np.corrcoef(g[1], d)[0, 1]))
                break
        else:
            out.append([i, d, [(i, 1.0)]])
    return len(idx), out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    r = pickle.load(open(sys.argv[1], "rb"))
    fr = r["frames"]
    for name in r["spans"]:
        raw = np.stack([np.frombuffer(f[1][name], dtype=np.uint8) for f in fr])
        n = raw.shape[1] // 4 * 4
        with np.errstate(invalid="ignore"):
            F = raw[:, :n].copy().view(np.float32).astype(np.float64)
        ok = np.all(np.isfinite(F), axis=0) & np.all(np.abs(F) < 1e4, axis=0)
        Dd = np.diff(F, axis=0)
        for label, D, mask, thr in (("degrees", (Dd + 180) % 360 - 180, ok, 50.0),
                                    ("radians/unit", (Dd + np.pi) % (2 * np.pi) - np.pi,
                                     ok & np.all(np.abs(F) <= 7, axis=0), 1.5)):
            count, gs = groups(F, D, mask, thr)
            print(f"== {name} {label}: {count} offsets with total change > {thr}")
            for g in gs[:12]:
                d = g[1]
                sig = "".join("+" if x > thr / 200 else "-" if x < -thr / 200 else "." for x in d)
                offs = " ".join(f"{o * 4:#x}{'' if c > 0 else '(inv)'}" for o, c in g[2][:8])
                print(f"  total {np.sum(np.abs(d)):8.1f} values {F[0, g[0]]:9.3f}->{F[-1, g[0]]:9.3f}  {sig}  "
                      f"[{len(g[2])}] {offs}")
        S16 = raw[:, :n].copy().view(np.int16).astype(np.int64)
        D16 = (np.diff(S16, axis=0) + 32768) % 65536 - 32768
        s16 = np.sum(np.abs(D16), axis=0)
        idx = np.where(s16 > 20000)[0]
        print(f"== {name} int16: {len(idx)} offsets with total change > 20000")
        for i in idx[np.argsort(-s16[idx])][:15]:
            sig = "".join("+" if x > 100 else "-" if x < -100 else "." for x in D16[:, i])
            print(f"  {i * 2:#x} {S16[0, i]:6d}->{S16[-1, i]:6d} total {s16[i]:7d} {sig}")


if __name__ == "__main__":
    main()
