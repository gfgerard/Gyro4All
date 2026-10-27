# Whole-memory delta scan for the camera's yaw/pitch state, against a reference camera matrix copy.
import sys, time, struct, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
from fbtypes import private_regions
REF = int(sys.argv[2], 16)
p = Proc('Dead Space.exe'); regions = list(private_regions(p)); CH = 32 << 20
chunks = [(c, min(CH, e - c)) for b, e in regions for c in range(b, e, CH)]
def ref_angles():
    m = np.array(struct.unpack('<12f', p.read(REF, 48)), dtype=np.float64).reshape(3, 4); f = m[2, :3]
    return np.arctan2(f[0], f[2]), np.arcsin(np.clip(f[1], -1, 1))
def full():
    out = []
    for c, n in chunks:
        r = p.read(c, n)
        out.append(np.frombuffer(r[: len(r) // 4 * 4], '<f4').copy() if r else None)
    return out
def beep(f, k):
    for _ in range(k): winsound.Beep(f, 110); time.sleep(0.05)
def wrap(a): return (a + np.pi) % (2 * np.pi) - np.pi
def match(base, new, d):
    """addresses whose change equals k*d, k in +-1 (rad), +-57.29578 (deg), modulo a full turn"""
    hits = []
    for (c, n), a, b in zip(chunks, base, new):
        if a is None or b is None or len(a) != len(b): continue
        with np.errstate(all='ignore'):
            dv = b.astype(np.float64) - a
            m = np.zeros(len(a), bool)
            for k in (1.0, -1.0, 57.29578, -57.29578):
                full_turn = 2 * np.pi * abs(k)
                r = (dv - k * d + full_turn / 2) % full_turn - full_turn / 2
                m |= np.abs(r) < 0.02 * abs(k * d) + 1e-4 * abs(k)
            m &= np.isfinite(dv) & (np.abs(dv) > 1e-4) & (np.abs(b) < 1e4)
        idx = np.nonzero(m)[0]
        hits.append(c + idx.astype(np.int64) * 4)
    return np.concatenate(hits) if hits else np.zeros(0, np.int64)
def read_at(addrs):
    out = np.full(len(addrs), np.nan)
    for i, a in enumerate(addrs):
        r = p.read(int(a), 4)
        if r: out[i] = struct.unpack('<f', r)[0]
    return out
log = []
beep(440, 1); time.sleep(1.5); y0, p0 = ref_angles(); A = full(); print(f'A yaw {np.degrees(y0):.1f} pitch {np.degrees(p0):.1f}', flush=True)
beep(880, 2); time.sleep(2.5)
beep(440, 1); time.sleep(2.0); y1, p1 = ref_angles(); B = full(); dy = wrap(y1 - y0)
yaw_c = match(A, B, dy); print(f'B yaw {np.degrees(y1):.1f} (d {np.degrees(dy):.1f}) pitch {np.degrees(p1):.1f}: {len(yaw_c)} yaw candidates', flush=True)
del A
beep(1320, 3); time.sleep(2.0)
beep(440, 1); time.sleep(2.0); y2, p2 = ref_angles(); C = full(); dp = p2 - p1
pitch_c = match(B, C, dp); print(f'C yaw {np.degrees(y2):.1f} pitch {np.degrees(p2):.1f} (d {np.degrees(dp):.1f}): {len(pitch_c)} pitch candidates', flush=True)
del B
# yaw candidates must not move with pitch (much)
yB = read_at(yaw_c) if len(yaw_c) < 2_000_000 else None
beep(660, 4); time.sleep(2.5)
beep(440, 1); time.sleep(2.0); y3, p3 = ref_angles(); D = full(); dy2 = wrap(y3 - y2)
yaw_c2 = np.intersect1d(yaw_c, match(C, D, dy2)); print(f'D yaw {np.degrees(y3):.1f} (d {np.degrees(dy2):.1f}): {len(yaw_c2)} yaw candidates confirmed', flush=True)
winsound.Beep(300, 600)
np.savez('deltascan.npz', yaw=yaw_c2, pitch=pitch_c, angles=np.array([[y0, p0], [y1, p1], [y2, p2], [y3, p3]]))
for a in yaw_c2[:40]: print(f'yaw   0x{a:X} = {read_at([a])[0]:.4f}')
for a in pitch_c[:40]: print(f'pitch 0x{a:X} = {read_at([a])[0]:.4f}')
