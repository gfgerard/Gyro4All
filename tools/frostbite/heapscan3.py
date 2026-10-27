# Round 1: unit 3- and 4-vectors (float32) everywhere at A/B/C/C2 -> candidates that change with BOTH the right turn
# and the look up, and are stable C->C2. Round 2: follow candidates at ~15 Hz through the same protocol.
import sys, time, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
from fbtypes import private_regions
p = Proc('Dead Space.exe'); regions = list(private_regions(p)); CH = 64 << 20
def chunks():
    for b, e in regions:
        for c in range(b, e, CH): yield c, min(CH, e - c)
def scan():
    out = {3: ([], []), 4: ([], [])}
    for c, n in chunks():
        raw = p.read(c, n)
        if not raw or len(raw) < 32: continue
        a = np.frombuffer(raw[: len(raw) // 4 * 4], dtype='<f4')
        with np.errstate(all='ignore'):
            sq = a * a
            for k in (3, 4):
                s = sum(sq[j:len(sq) - k + 1 + j] for j in range(k))
                cols = [a[j:len(a) - k + 1 + j] for j in range(k)]
                mx = np.max(np.abs(np.stack(cols)), 0)
                m = (np.abs(s - 1) < 2e-3) & (mx < 0.995) & (mx > 0.05)
                if k == 4: m &= np.abs(cols[3]) > 1e-3  # a real w
                idx = np.nonzero(m)[0]
                out[k][0].append(c + idx.astype(np.int64) * 4); out[k][1].append(np.stack([x[idx] for x in cols], 1))
    return {k: (np.concatenate(v[0]), np.concatenate(v[1])) for k, v in out.items()}
def reread(addrs, k):
    out = np.full((len(addrs), k), np.nan, np.float32); order = np.argsort(addrs); sa = addrs[order]
    for c, n in chunks():
        lo, hi = np.searchsorted(sa, c), np.searchsorted(sa, c + n - 4 * k)
        if lo == hi: continue
        raw = p.read(c, n)
        if not raw: continue
        a = np.frombuffer(raw[: len(raw) // 4 * 4], dtype='<f4'); off = ((sa[lo:hi] - c) // 4).astype(np.int64)
        out[order[lo:hi]] = np.stack([a[off + j] for j in range(k)], 1)
    return out
def ang(u, v):  # works for 3-vectors and (sign-free) quaternions
    with np.errstate(all='ignore'):
        d = np.abs(np.sum(u * v, -1)) if u.shape[-1] == 4 else np.sum(u * v, -1)
        d = d / np.linalg.norm(u, axis=-1) / np.linalg.norm(v, axis=-1)
        return np.degrees(np.arccos(np.clip(d, -1, 1))) * (2 if u.shape[-1] == 4 else 1)
def beep(f, k):
    for _ in range(k): winsound.Beep(f, 120); time.sleep(0.05)
# ---- round 1
beep(440, 1); time.sleep(2); t = time.time(); A = scan(); print('A', {k: len(v[0]) for k, v in A.items()}, f'{time.time()-t:.0f}s', flush=True)
beep(880, 2); time.sleep(4); beep(440, 1); time.sleep(2); B = {k: reread(A[k][0], k) for k in A}
beep(1320, 3); time.sleep(3); beep(440, 1); time.sleep(2); C = {k: reread(A[k][0], k) for k in A}; time.sleep(1); C2 = {k: reread(A[k][0], k) for k in A}
cands = []
for k in (3, 4):
    ad, a0 = A[k]; b, c, c2 = B[k], C[k], C2[k]
    unit = lambda v: np.abs(np.linalg.norm(v, axis=1) - 1) < 3e-3
    m = unit(b) & unit(c) & unit(c2) & (ang(a0, b) > 8) & (ang(b, c) > 5) & (ang(c, c2) < 0.5)
    cands += [(int(x), k) for x in ad[m]]
print('round 1 candidates:', len(cands), flush=True)
winsound.Beep(300, 400); time.sleep(1.5)
# ---- round 2
pos = np.array([a for a, k in cands], np.int64); ks = np.array([k for a, k in cands])
def sample():
    out = np.full((len(cands), 4), np.nan, np.float32)
    for i, (a, k) in enumerate(cands):
        r = p.read(a, 4 * k)
        if r and len(r) == 4 * k: out[i, :k] = np.frombuffer(r, '<f4')
    return out
S, P = [], []
for name, secs, f, k in [('still', 4, 440, 1), ('right', 4, 880, 2), ('still', 4, 440, 1), ('up', 3, 1320, 3), ('still', 4, 440, 1)]:
    beep(f, k); end = time.time() + secs
    while time.time() < end: S.append(sample()); P.append(name); time.sleep(0.05)
winsound.Beep(300, 600)
np.savez('heapscan3.npz', pos=pos, ks=ks, S=np.array(S), P=np.array(P))
print(len(S), 'samples')
