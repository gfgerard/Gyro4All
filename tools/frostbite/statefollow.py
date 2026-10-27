# Samples an object's floats (e.g. Dead Space's character input state) at ~30 Hz through a stick protocol and lists
# the floats that follow the right stick: ~0 at rest, one sign while it is right and the other while left (yaw), or
# nonzero while it is up (pitch). Read-only.
# Protocol (after a double low ready beep and 5 s): 1 low beep = release the stick (3 s); 2 beeps = stick right (3 s);
# 1 low beep = release (3 s); 4 beeps = stick left (3 s); 1 low beep = release (3 s); 3 high beeps = stick up (3 s);
# long low beep = done. Hold L2 throughout if the test is about aiming.
# Usage: statefollow.py <tools dir> <object hex> [size hex, default C00]
import sys, time, struct, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
p = Proc('Dead Space.exe'); obj = int(sys.argv[2], 16); size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xC00


def beep(f, k):
    for _ in range(k): winsound.Beep(f, 110); time.sleep(0.05)


winsound.Beep(330, 250); time.sleep(0.1); winsound.Beep(330, 250); time.sleep(5)
S, P, RAW = [], [], []
for name, f, k in [('rel', 440, 1), ('right', 880, 2), ('rel', 440, 1), ('left', 660, 4), ('rel', 440, 1), ('up', 1320, 3)]:
    beep(f, k); t0 = time.time()
    while time.time() - t0 < 3:
        b = p.read(obj, size)
        if b and len(b) == size and time.time() - t0 > 0.5:  # skip the reaction time
            S.append(np.frombuffer(b, '<f4').astype(np.float64)); P.append(name); RAW.append(b)
        time.sleep(0.03)
winsound.Beep(300, 700)
S = np.array(S); P = np.array(P)
np.savez('statefollow.npz', raw=np.array([np.frombuffer(r, np.uint8) for r in RAW]), P=P)  # raw bytes, for other readings
with np.errstate(all='ignore'):
    ok = np.all(np.isfinite(S), 0) & (np.max(np.abs(S), 0) < 1e4)
    med = {n: np.median(S[P == n], 0) for n in ('rel', 'right', 'left', 'up')}
    lo = {n: np.min(S[P == n], 0) for n in ('right', 'left', 'up')}
    hi = {n: np.max(S[P == n], 0) for n in ('right', 'left', 'up')}
rest = np.max(np.abs(S[P == 'rel']), 0)
for i in np.nonzero(ok)[0]:
    yaw = rest[i] < 0.05 and ((lo['right'][i] > 0.05 and hi['left'][i] < -0.05) or (hi['right'][i] < -0.05 and lo['left'][i] > 0.05))
    pitch = rest[i] < 0.05 and (abs(med['up'][i]) > 0.05) and abs(med['right'][i]) < 0.05
    moving = np.ptp(S[:, i]) > 1e-3
    if yaw or pitch or (moving and rest[i] < 0.05 and max(abs(med['right'][i]), abs(med['left'][i]), abs(med['up'][i])) > 0.05):
        tag = 'YAW' if yaw else 'PITCH' if pitch else 'other'
        print(f'+0x{4 * i:03X} {tag:5s} rest<{rest[i]:.3f} right {med["right"][i]:+.4f} left {med["left"][i]:+.4f} '
              f'up {med["up"][i]:+.4f}  (index from +0: {i})')
