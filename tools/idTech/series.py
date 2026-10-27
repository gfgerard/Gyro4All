#!/usr/bin/env python3
"""Values of chosen floats in a record.py recording, frame by frame (standard library only):

  python -I tools/idTech/series.py <scratchpad>/rec.pkl exe+3db7980:10 exe+3db7980:14 player:fb60 player:fb64

Each column is <span name>:<hex offset in that span>. Two copies of one angle a constant apart (view = input + delta)
show the same steps; the one the game rebuilds from the other is the copy, the other the source (test with poke.py).
"""
import pickle
import struct
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    r = pickle.load(open(sys.argv[1], "rb"))
    cols = [(s, int(o, 16)) for s, o in (a.rsplit(":", 1) for a in sys.argv[2:])]
    print("    t  " + " ".join(f"{s[-8:]}+{o:x}".rjust(14) for s, o in cols))
    for t, f in r["frames"]:
        print(f"{t:5.2f}  " + " ".join(f"{struct.unpack_from('<f', f[s], o)[0]:14.3f}" for s, o in cols))


if __name__ == "__main__":
    main()
