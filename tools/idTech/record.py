#!/usr/bin/env python3
"""Records memory spans of a running game at 4 Hz while the player turns, for analyse.py / series.py. Read-only,
standard library only, Windows. Waits for the game and for a live player first.

  python -I tools/idTech/record.py NewColossus_x64vk.exe <scratchpad>/rec.pkl \
      --player idPlayer --inner idHavokPhysics_Player@eb68 --span player:60000 --span exe+3db7980:4000

--player: the player's RTTI class; --inner Class@<hex offset>: an embedded object whose vtable must be there (tells the
live player from stale vtable hits). --span <base>:<hex size>, base = "player", "player+<hex>", "exe+<hex>" or a hex
address (named after the base in the output). Then: two short beeps (ready), 5 s, a high beep, --seconds (15) of
snapshots, a low beep. Do the moves in a fixed order (e.g. right, left, up, down, 2 s each at full stick) with the pad
still on the table and the game in front.
"""
import argparse
import os
import pickle
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import livemem  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--player", default="idPlayer")
    ap.add_argument("--inner", help="Class@<hex offset> embedded in the live player")
    ap.add_argument("--span", action="append", required=True)
    ap.add_argument("--seconds", type=float, default=15)
    a = ap.parse_args()
    while True:
        try:
            p = livemem.Proc(a.exe)
            break
        except livemem.NotRunning:
            time.sleep(3)
    print(f"game pid {p.pid}", flush=True)
    inner_cls, inner_off = (a.inner.split("@")[0], int(a.inner.split("@")[1], 16)) if a.inner else (None, 0)
    player = None
    if any(s.startswith("player") for s in a.span):
        while True:
            live = p.live_objects(a.player, inner_cls, inner_off)
            if live:
                player = live[0]
                break
            time.sleep(5)
        print(f"player {player:#x} ({len(live)} live)", flush=True)
    spans = {}
    for s in a.span:
        base, size = s.rsplit(":", 1)
        addr = (player + (int(base[7:], 16) if base.startswith("player+") else 0)) if base.startswith("player") \
            else p.addr(base)
        spans[base] = (addr, int(size, 16))
    livemem.beep(700, 150)
    time.sleep(0.1)
    livemem.beep(700, 150)
    time.sleep(5)
    livemem.beep(880, 300)
    frames, t0 = [], time.time()
    while time.time() - t0 < a.seconds:
        frames.append((time.time() - t0, {k: p.read_padded(ad, n) for k, (ad, n) in spans.items()}))
        time.sleep(0.25)
    livemem.beep(440, 600)
    pickle.dump({"spans": spans, "player": player, "frames": frames}, open(a.out, "wb"))
    print(len(frames), "frames", flush=True)


if __name__ == "__main__":
    main()
