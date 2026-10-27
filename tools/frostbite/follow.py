# Follows the yaw/pitch candidates and the reference matrix while the user moves the camera freely, then ranks them.
import sys, time, struct, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
REF, SECS = int(sys.argv[2], 16), float(sys.argv[3])
d = np.load('deltascan.npz'); cands = {'yaw': d['yaw'], 'pitch': d['pitch']}
p = Proc('Dead Space.exe')
allc = np.unique(np.concatenate(list(cands.values())))
# group into page reads
pages = {}
for a in allc: pages.setdefault(int(a) & ~0xFFF, []).append(int(a))
print(len(allc), 'addresses in', len(pages), 'pages', flush=True)
def sample():
    vals = {}
    for pg, al in pages.items():
        r = p.read(pg, 0x1000)
        if r:
            for a in al: vals[a] = struct.unpack_from('<f', r, a - pg)[0]
    m = np.array(struct.unpack('<12f', p.read(REF, 48))).reshape(3, 4); f = m[2, :3]
    return vals, np.arctan2(f[0], f[2]), np.arcsin(np.clip(f[1], -1, 1))
winsound.Beep(880, 300); T, Y, P, V = [], [], [], []
end = time.time() + SECS
while time.time() < end:
    v, y, pt = sample(); V.append(v); Y.append(y); P.append(pt); T.append(time.time())
winsound.Beep(300, 600)
Y = np.unwrap(np.array(Y)); P = np.array(P)
print(f'{len(T)} samples, {len(T)/SECS:.1f}/s; yaw range {np.degrees(np.ptp(Y)):.0f}, pitch range {np.degrees(np.ptp(P)):.0f}')
for name, ref in (('yaw', Y), ('pitch', P)):
    X = np.stack([ref, np.ones_like(ref)], 1); rows = []
    for a in cands[name]:
        y = np.array([v.get(int(a), np.nan) for v in V])
        if not np.all(np.isfinite(y)) or np.ptp(y) == 0: continue
        for label, yy in (('raw', y), ('unwrapped', np.unwrap(y)), ('unwrapped deg', np.degrees(np.unwrap(np.radians(y))))):
            coef = np.linalg.lstsq(X, yy, rcond=None)[0]
            r2 = 1 - np.sum((yy - X @ coef) ** 2) / np.sum((yy - yy.mean()) ** 2)
            rows.append((r2, coef[0], int(a), label, y[0], y[-1]))
    rows.sort(reverse=True)
    print(f'\n== {name}: best fits')
    for r2, k, a, label, y0, y1 in rows[:15]: print(f'  r2 {r2:.5f} slope {k:9.4f} 0x{a:X} ({label})  {y0:.4f} -> {y1:.4f}')
np.savez('follow.npz', T=np.array(T), Y=Y, P=P)
