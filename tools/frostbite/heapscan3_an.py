import numpy as np
d = np.load('heapscan3.npz'); pos, ks, S, P = d['pos'], d['ks'], d['S'].astype(float), d['P']
def ang(u, v, k):
    u, v = u[..., :k], v[..., :k]
    with np.errstate(all='ignore'):
        dd = np.sum(u * v, -1) / np.linalg.norm(u, axis=-1) / np.linalg.norm(v, axis=-1)
        if k == 4: dd = np.abs(dd)
        return np.degrees(np.arccos(np.clip(dd, -1, 1))) * (2 if k == 4 else 1)
blocks = []; i = 0
while i < len(P):
    j = i
    while j + 1 < len(P) and P[j + 1] == P[i]: j += 1
    blocks.append((P[i], i, j)); i = j + 1
rows = []
for n in range(len(pos)):
    k = ks[n]; x = S[:, n]
    if np.isnan(x[:, :k]).any(): continue
    if not np.all(np.abs(np.linalg.norm(x[:, :k], axis=1) - 1) < 3e-3): continue
    steps = ang(x[1:], x[:-1], k); ms = steps.max()
    still = max(ang(x[b0:b1+1], x[b0], k).max() for nm, b0, b1 in blocks if nm == 'still')
    r = [ang(x[b1], x[b0], k) for nm, b0, b1 in blocks if nm == 'right'][0]
    u = [ang(x[b1], x[b0], k) for nm, b0, b1 in blocks if nm == 'up'][0]
    moving = (steps > 0.05).sum()
    rows.append((ms > 15 or still > 0.3 * max(r, u) or r < 5 or u < 3, pos[n], k, r, u, still, ms, moving, x[0, :k], x[-1, :k]))
good = [r for r in rows if not r[0]]
print(len(rows), 'unit all along;', len(good), 'smooth, moved with right AND up, quiet when still')
for bad, a, k, r, u, st, ms, mv, v0, v1 in sorted(good, key=lambda r: r[1]):
    print(f'0x{a:X} k{k} right {r:6.1f} up {u:6.1f} still {st:5.2f} maxstep {ms:5.2f} moving {mv:3d}  {np.round(v0,3)} -> {np.round(v1,3)}')
print('\nclosest misses (sorted by maxstep, moved right > 5):')
for bad, a, k, r, u, st, ms, mv, v0, v1 in sorted([r for r in rows if r[3] > 5], key=lambda r: r[6])[:30]:
    print(f'0x{a:X} k{k} right {r:6.1f} up {u:6.1f} still {st:5.2f} maxstep {ms:5.2f} moving {mv:3d}  {np.round(v0,3)} -> {np.round(v1,3)}')
