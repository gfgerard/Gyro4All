#!/usr/bin/env python3
"""Floats that changed between snapshots A and B of `livemem.py turn` and then held (B ~= C): view angle candidates.
Standard library only.

  python -I tools/idTech/turndiff.py <turn.json> [--min 5] [--unit]

--min: the smallest change in degrees (wrapped to +-180) to list. --unit also lists values in [-1, 1] that changed by
more than 0.05 (rotation matrix rows, quaternions, radians). A matrix entry that moves with the turn is not the view
itself: check its writer with WriteWatch before using it (Wolfenstein II's player+0xd6ec was a bone transform).
"""
import argparse
import json
import math
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("turn")
    ap.add_argument("--min", type=float, default=5.0)
    ap.add_argument("--unit", action="store_true")
    a = ap.parse_args()
    d = json.load(open(a.turn))
    for key in d["A"]:
        base = int(key.split(":")[0], 16)
        sa, sb, sc = (bytes.fromhex(d[s][key]) for s in "ABC")
        n = min(len(sa), len(sb), len(sc)) // 4 * 4
        print(f"== span {base:#x} ({n:#x} bytes)")
        for off in range(0, n, 4):
            fa, fb, fc = (struct.unpack_from("<f", x, off)[0] for x in (sa, sb, sc))
            if not all(math.isfinite(v) and abs(v) < 1e5 for v in (fa, fb, fc)) or abs(fc - fb) > 0.05:
                continue
            dw = (fb - fa + 180) % 360 - 180
            unit = a.unit and max(abs(fa), abs(fb)) <= 1.0 and abs(fb - fa) > 0.05
            if (abs(dw) > a.min and (fa or fb)) or unit:
                print(f"+0x{off:05x} A {fa:10.3f} B {fb:10.3f} C {fc:10.3f}  change {dw:8.3f}{'  (unit)' if unit else ''}")


if __name__ == "__main__":
    main()
