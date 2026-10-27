# Finds the game's copies of the right stick's X axis: floats that are ~0 with the stick released, positive while it is
# held right and negative while held left (whole private memory, read-only). Audio cues: 1 low beep = release the
# stick; 2 beeps = push it fully right (to the edge, steady until the next beep); 4 beeps = push it fully left the same
# way; long low beep = done. Writes stickscan.npz (addresses, values) to the current folder.
# Full deflection both ways is what makes the right and left values mirror each other (the game clamps the processed
# stick to +-1), which the filter relies on; with "half" deflection a player never matches both sides and the real
# copies were dropped (Dead Space, 2026-10-18).
# Usage: stickscan.py <tools dir>
import sys, time, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
from fbtypes import private_regions

p = Proc('Dead Space.exe')
regions = list(private_regions(p))
CH = 32 << 20
chunks = [(c, min(CH, e - c)) for b, e in regions for c in range(b, e, CH)]


def snap():
    out = []
    for c, n in chunks:
        r = p.read(c, n)
        out.append(np.frombuffer(r[: len(r) // 4 * 4], '<f4').copy() if r else None)
    return out


def beep(f, k):
    for _ in range(k):
        winsound.Beep(f, 110)
        time.sleep(0.05)


beep(440, 1); time.sleep(2); A = snap(); print('released', flush=True)
beep(880, 2); time.sleep(2); B = snap(); print('right', flush=True)
beep(660, 4); time.sleep(2); C = snap(); print('left', flush=True)
beep(440, 1); time.sleep(2); D = snap(); print('released', flush=True)
winsound.Beep(300, 600)

addrs, vals = [], []
for (c, n), a, b, cc, d in zip(chunks, A, B, C, D):
    if any(x is None for x in (a, b, cc, d)) or not (len(a) == len(b) == len(cc) == len(d)):
        continue
    with np.errstate(all='ignore'):
        # Up to 1000: also scaled copies (a turn speed in degrees or radians per second at full stick).
        m = (np.abs(a) < 1e-3) & (np.abs(d) < 1e-3) & (b > 0.15) & (b <= 1000) & (cc < -0.15) & (cc >= -1000)
        m &= np.abs(b + cc) < 0.25 * np.abs(b)
    idx = np.nonzero(m)[0]
    addrs.append(c + idx.astype(np.int64) * 4)
    vals.append(np.stack([b[idx], cc[idx]], 1))
addrs = np.concatenate(addrs) if addrs else np.zeros(0, np.int64)
vals = np.concatenate(vals) if vals else np.zeros((0, 2), np.float32)
np.savez('stickscan.npz', addrs=addrs, vals=vals)
print(len(addrs), 'candidates')
for a, (r, l) in list(zip(addrs, vals))[:40]:
    print(f'  0x{a:X}  right {r:+.4f}  left {l:+.4f}')
