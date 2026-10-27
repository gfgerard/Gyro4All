import sys, pickle, struct; sys.path.insert(0, sys.argv[1])
import numpy as np
types = pickle.load(open('types.pkl', 'rb')); objs, spans, S, R, P = pickle.load(open('corrwatch.pkl', 'rb'))
M = np.array([np.frombuffer(r, '<f4') for r in R], dtype=np.float64)
right, fwd = M[:, 0:3], M[:, 8:11]
yaw = np.unwrap(np.arctan2(fwd[:, 0], fwd[:, 2])); pitch = np.arcsin(np.clip(fwd[:, 1], -1, 1))
print(f'reference: yaw {np.degrees(yaw[0]):.1f} -> {np.degrees(yaw[-1]):.1f} (range {np.degrees(np.ptp(yaw)):.1f}), pitch range {np.degrees(np.ptp(pitch)):.2f}')
out = []
for ref_name, ref in (('yaw', yaw), ('pitch', pitch)):
    if np.ptp(ref) < np.radians(5): print(f'{ref_name}: not enough motion'); continue
    X = np.stack([ref, np.ones_like(ref)], 1)
    for i, ((a, t), n) in enumerate(zip(objs, spans)):
        rows = [s[i] for s in S]
        if any(len(r) < n for r in rows): continue
        F = np.array([np.frombuffer(r[: n // 4 * 4], '<f4') for r in rows], dtype=np.float64)
        with np.errstate(all='ignore'):
            ok = np.all(np.isfinite(F), 0) & (np.ptp(F, 0) > 1e-3) & (np.max(np.abs(F), 0) < 1e5)
        for o in np.nonzero(ok)[0]:
            y = F[:, o]
            coef, res, *_ = np.linalg.lstsq(X, y, rcond=None)
            r2 = 1 - np.sum((y - X @ coef) ** 2) / np.sum((y - y.mean()) ** 2)
            if r2 > 0.98: out.append((ref_name, r2, coef[0], types[t]['name'], a, o * 4, y[0], y[-1]))
for ref_name, r2, slope, nm, a, off, y0, y1 in sorted(out, key=lambda r: (r[0], -r[1]))[:80]:
    unit = 'rad' if abs(abs(slope) - 1) < 0.05 else 'deg' if abs(abs(slope) - 57.2958) < 3 else f'x{slope:.3f}'
    print(f'{ref_name:5s} r2={r2:.4f} {unit:9s} {nm:50s} 0x{a:X} +0x{off:03X}  {y0:.4f} -> {y1:.4f}')
print(len(out), 'fields follow the camera')
