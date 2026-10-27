# Finds the fields that turn with the right stick while aiming (Dead Space: Isaac's own orientation next to the
# camera's). Samples every typed object (~4 Hz) through the beep protocol and fits each float field to references built
# from the protocol itself (yaw: rises while the stick is right, falls while it is left; pitch: rises while it is up),
# so no camera address is needed. Lists the fields with r2 > 0.95 and their classes.
# Protocol (after the object scan: a double low ready beep, then 5 s), L2 held from the first beep to the long low beep:
#   1 beep = L2 + stick right (4 s), 2 beeps = stick up (3 s), 4 beeps = stick left (4 s), long low beep = done.
# Turn Gyro4All's gyro off (F10 panel) first.
# Usage: aimcorr.py <tools dir> [class regex to print]   (writes types.pkl and aimcorr.pkl in the current folder)
import sys, os, time, pickle, winsound, re; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
from fbtypes import *
show = re.compile(sys.argv[2] if len(sys.argv) > 2 else '.')
p = Proc('Dead Space.exe')
types = pickle.load(open('types.pkl', 'rb')) if os.path.exists('types.pkl') else walk_types(p)
pickle.dump(types, open('types.pkl', 'wb'))
classes = {t: v for t, v in types.items() if v['kind'] == 3}
gt = find_gettype(p, classes); fns = {f: t for t, fs in gt.items() for f in fs}
slots = find_values(p, fns.keys(), list(image_data_regions(p)))
codes = list(exec_regions(p)); is_code = lambda v: v and any(rb <= v < re_ for rb, re_ in codes)
vt = {}
for addr, fn in slots:
    s = 0
    while s < 600 and is_code(p.q(addr - 8 * (s + 1))): s += 1
    vt[addr - 8 * s] = fns[fn]
objs = find_values(p, vt.keys(), list(private_regions(p)))
skip = re.compile(r'^Dx12|^Rc2|RenderTarget|Rime|Texture|Mesh|Shader|Sound|Audio|Bytecode|CullTag')
objs = [(a, vt[v]) for a, v in objs if not skip.search(types[vt[v]]['name'])]
spans = [min(types[t]['size'], 0x400) if types[t]['size'] >= 0x10 else 0x200 for a, t in objs]
print(len(objs), 'objects; ready beep, starting in 5 s', flush=True)


def beep(f, k):
    for _ in range(k): winsound.Beep(f, 120); time.sleep(0.05)


winsound.Beep(330, 250); time.sleep(0.1); winsound.Beep(330, 250)  # ready
time.sleep(5)
S, P = [], []
for name, secs, f, k in [('right', 4, 880, 1), ('up', 3, 1100, 2), ('left', 4, 660, 4)]:
    beep(f, k); end = time.time() + secs
    while time.time() < end:
        S.append([p.read(a, n) or b'' for (a, t), n in zip(objs, spans)]); P.append(name)
winsound.Beep(300, 700)
pickle.dump((objs, spans, S, P), open('aimcorr.pkl', 'wb'))
P = np.array(P)
print(len(S), 'samples', {n: int((P == n).sum()) for n in ('right', 'up', 'left')}, flush=True)

refs = {'yaw': np.cumsum(P == 'right') - np.cumsum(P == 'left'), 'pitch': np.cumsum(P == 'up')}
out = []
for ref_name, ref in refs.items():
    X = np.stack([ref.astype(float), np.ones(len(ref))], 1)
    for i, ((a, t), n) in enumerate(zip(objs, spans)):
        rows = [s[i] for s in S]
        if any(len(r) < n for r in rows): continue
        with np.errstate(all='ignore'):
            F = np.array([np.frombuffer(r[: n // 4 * 4], '<f4') for r in rows], dtype=np.float64)
            ok = np.all(np.isfinite(F), 0) & (np.ptp(F, 0) > 1e-3) & (np.max(np.abs(F), 0) < 1e5)
        for o in np.nonzero(ok)[0]:
            y = F[:, o]
            coef, *_ = np.linalg.lstsq(X, y, rcond=None)
            r2 = 1 - np.sum((y - X @ coef) ** 2) / np.sum((y - y.mean()) ** 2)
            if r2 > 0.95: out.append((ref_name, r2, coef[0], types[t]['name'], a, o * 4, y[0], y[-1]))
from collections import Counter
print('classes with fields that follow:', Counter((r[0], r[3]) for r in out).most_common(30))
for ref_name, r2, slope, nm, a, off, y0, y1 in sorted(out, key=lambda r: (r[0], r[3], -r[1])):
    if show.search(nm):
        print(f'{ref_name:5s} r2={r2:.4f} slope {slope:+.5f}/sample {nm:50s} 0x{a:X} +0x{off:03X}  {y0:.4f} -> {y1:.4f}')
print(len(out), 'fields follow the stick')
