# states.py: find a game's flag for a state (pause menu, weapon wheel, map, inventory) from a few sound-cued switches,
# read-only, no debugger, nothing saved to disk. The user plays (gameplay) and, at each high beep, goes into the state
# (opens the pause menu, holds the wheel button) and stays there; at two low beeps, back to gameplay. The exe's static
# data is sampled near the end of each phase; what is kept: bytes with one value in every gameplay sample and another
# one in every state sample. Engine-neutral, 64-bit Windows games; safe on games with anti-debugging. See README.md.
#
#   uv run --with numpy python -I tools/camtrace/states.py <exe> [--cycles 3] [--hold 5] [--max-section 128] [--limit 40]
#       [--out FILE] [--also FILE]  (--also: only what also changes that way in another state, e.g. pause and a wheel)
import argparse, os, sys, time, winsound
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import camtrace as ct


def sample(p, sections):
    out = []
    for base, size in sections:
        b = p.read(base, size)
        out.append(np.frombuffer(b, np.uint8).copy() if b and len(b) == size else None)
    return out


def main():
    ap = argparse.ArgumentParser(description="Find the flag of a game state (pause, weapon wheel) by sound-cued switches.")
    ap.add_argument("exe")
    ap.add_argument("--cycles", type=int, default=3, help="times into the state and back (default 3)")
    ap.add_argument("--hold", type=float, default=5, help="seconds per phase (default 5)")
    ap.add_argument("--max-section", type=float, default=128, help="skip writable sections over this many MB (a protector's VM)")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--out", help="write every candidate to this file (exe offset, gameplay and state values)")
    ap.add_argument("--also", help="keep only the candidates in this --out file of another state (one flag for both)")
    a = ap.parse_args()
    p = ct.Proc(a.exe)
    sections = [(b, s) for b, s in p.exe_rw() if s <= a.max_section * (1 << 20)]
    skipped = [(b, s) for b, s in p.exe_rw() if s > a.max_section * (1 << 20)]
    for b, s in skipped: print(f"skipping {p.name(b)} ({s >> 20} MB)")
    print(f"{len(sections)} sections, {sum(s for _, s in sections) >> 20} MB per sample", flush=True)
    for f in (800, 1000, 1200): winsound.Beep(f, 150); time.sleep(0.25)
    first = {}  # state -> first sample
    keep = None
    phases = ["game", "state"] * a.cycles + ["game"]
    for i, phase in enumerate(phases):
        if i:
            if phase == "state": winsound.Beep(1400, 350)
            else: winsound.Beep(600, 180); time.sleep(0.08); winsound.Beep(600, 180)
        time.sleep(a.hold - 1)
        for _ in range(2):
            s = sample(p, sections)
            if phase not in first:
                first[phase] = s
                if phase == "state":
                    keep = [None if g is None or t is None else g != t for g, t in zip(first["game"], s)]
            elif keep is not None:
                ref = first[phase]
                keep = [None if k is None or x is None else k & (x == r) for k, x, r in zip(keep, s, ref)]
            time.sleep(0.5)
        n = sum(int(k.sum()) for k in keep if k is not None) if keep else None
        print(f"phase {i} ({phase}): {'-' if n is None else n} candidate bytes", flush=True)
    for f in (1200, 1000, 800): winsound.Beep(f, 150); time.sleep(0.1)
    rows = []
    for (base, size), k, g, t in zip(sections, keep, first["game"], first["state"]):
        if k is None: continue
        for i in np.nonzero(k)[0]:
            rows.append((base + int(i), int(g[i]), int(t[i])))
    if a.also:
        other = {}
        for line in open(a.also):
            off, g, t = line.split()
            other[int(off, 16)] = (int(g, 16), int(t, 16))
        rows = [r for r in rows if other.get(r[0] - p.base, (None,))[0] == r[1] and other[r[0] - p.base][1] == r[2]]
        print(f"{len(rows)} of them change the same way in {a.also}'s state")
    if a.out:
        with open(a.out, "w") as f:
            for at, g, t in rows: f.write(f"{at - p.base:#x} {g:#x} {t:#x}\n")
    # Plain flags first (0/1 either way), then the rest.
    rows.sort(key=lambda r: (not ({r[1], r[2]} <= {0, 1}), r[0]))
    print(f"{len(rows)} bytes with one value in gameplay and another in the state:")
    for at, g, t in rows[: a.limit]:
        print(f"  {p.name(at):18s} gameplay {g:#04x}  state {t:#04x}")


if __name__ == "__main__":
    main()
