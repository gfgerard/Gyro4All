# Follows stickscan.py's candidates at ~20 Hz through release/right/release/left/release (3 s each, same beeps) and
# keeps those that are ~0 when released, positive when right and negative when left in every sample of each phase.
# Usage: stickfollow.py <tools dir>
import sys, time, struct, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
d = np.load('stickscan.npz'); addrs = d['addrs']
p = Proc('Dead Space.exe')
pages = {}
for a in addrs: pages.setdefault(int(a) & ~0xFFF, []).append(int(a))
def sample():
    out = {}
    for pg, al in pages.items():
        r = p.read(pg, 0x1000)
        if r:
            for a in al: out[a] = struct.unpack_from('<f', r, a - pg)[0]
    return out
def beep(f, k):
    for _ in range(k): winsound.Beep(f, 110); time.sleep(0.05)
S, P = [], []
print('starting in 3 s', flush=True); time.sleep(3)
for name, f, k in [('rel', 440, 1), ('right', 880, 2), ('rel', 440, 1), ('left', 660, 4), ('rel', 440, 1)]:
    beep(f, k); end = time.time() + 3.0; t0 = time.time()
    while time.time() < end:
        v = sample()
        if time.time() - t0 > 0.6: S.append(v); P.append(name)  # skip the reaction time
        time.sleep(0.03)
winsound.Beep(300, 600)
P = np.array(P)
np.savez('stickfollow.npz', addrs=addrs, S=np.array([[s.get(int(a), np.nan) for a in addrs] for s in S]), P=P)
good = []
for a in addrs:
    x = np.array([s.get(int(a), np.nan) for s in S])
    if np.isnan(x).any(): continue
    rel, r, l = x[P == 'rel'], x[P == 'right'], x[P == 'left']
    if np.max(np.abs(rel)) < 0.02 and np.min(r) > 0.1 and np.max(l) < -0.1:
        good.append((int(a), float(np.median(r)), float(np.median(l)), float(np.std(r))))
print(len(good), 'follow the stick')
for a, r, l, sd in good: print(f'  0x{a:X}  right {r:+.4f}  left {l:+.4f}  (sd while right {sd:.4f})')
np.save('stick_good.npy', np.array([g[0] for g in good], dtype=np.int64))
