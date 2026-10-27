#!/usr/bin/env python3
"""Live write test of a view angle candidate: adds degrees to floats in the running game, one step at a time, and
prints them (and an optional watched copy) before, right after and 2 s after each write. **Writes game memory**:
only for testing on your own save, with the game in front; standard library only, Windows.

  python -I tools/idTech/poke.py NewColossus_x64vk.exe exe+3db7990 1:30 0:15 \
      --watch idPlayer,idHavokPhysics_Player@eb68,eb68+ff8

<base> is the first float (exe+<hex> or a hex address); each <index>:<degrees> adds to float <index> after 5 s and
then every 2 s, with a beep. In id Tech's senses (idAngles: pitch, yaw, roll) +yaw turns left and +pitch looks
down. --watch Class,Inner@<hex>,<hex>[+<hex>]: also print two floats at that offset of the live player (e.g. the
physics object's view) to see whether a copy follows. A write that holds and turns the screen is the view's source.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import livemem  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("base")
    ap.add_argument("steps", nargs="+")
    ap.add_argument("--watch")
    a = ap.parse_args()
    try:
        p = livemem.Proc(a.exe, write=True)
    except livemem.NotRunning as e:
        sys.exit(str(e))
    base = p.addr(a.base)
    watch = None
    if a.watch:
        cls, inner, off = a.watch.split(",")
        live = p.live_objects(cls, inner.split("@")[0], int(inner.split("@")[1], 16))
        if live:
            watch = live[0] + sum(int(x, 16) for x in off.split("+"))
        print(f"watched copy at {watch and hex(watch)}", flush=True)
    count = max(int(s.split(":")[0]) for s in a.steps) + 1

    def show(label):
        v = struct.unpack(f"<{count}f", p.read(base, 4 * count))
        w = struct.unpack("<2f", p.read(watch, 8)) if watch else ()
        print(f"{label:18s} " + " ".join(f"[{i}] {x:9.3f}" for i, x in enumerate(v))
              + (" | watched " + " ".join(f"{x:9.3f}" for x in w) if w else ""), flush=True)

    time.sleep(5)
    show("before")
    for s in a.steps:
        i, deg = int(s.split(":")[0]), float(s.split(":")[1])
        livemem.beep(880, 200)
        at = base + 4 * i
        ok = p.write(at, struct.pack("<f", struct.unpack("<f", p.read(at, 4))[0] + deg))
        show(f"after [{i}] {deg:+g}" + ("" if ok else " FAILED"))
        time.sleep(2)
        show("2 s later")
    livemem.beep(440, 400)


if __name__ == "__main__":
    main()
