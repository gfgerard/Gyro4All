# Offline reading of statefollow.npz: fields (float32, int16, int8) that change while the stick is right / left / up
# and stay still while it is released, whatever their value (absolute angles too), with the direction of the change.
# Usage: stateanalyse.py [statefollow.npz]
import sys
import numpy as np
d = np.load(sys.argv[1] if len(sys.argv) > 1 else 'statefollow.npz')
raw, P = d['raw'], d['P']
views = {'f32': raw.view('<f4'), 'i16': raw.view('<i2'), 'i8': raw.view('i1')}
for kind, V in views.items():
    V = V.astype(np.float64)
    with np.errstate(all='ignore'):
        dv = np.diff(V, axis=0)
    ph = P[1:]
    for i in range(V.shape[1]):
        col = V[:, i]
        if not np.all(np.isfinite(col)) or np.max(np.abs(col)) > 1e6: continue
        d_ = dv[:, i]
        tot = {n: np.sum(d_[ph == n]) for n in ('right', 'left', 'up', 'rel')}
        still_rel = np.mean(np.abs(d_[ph == 'rel']) > 1e-6) < 0.15
        mov = {n: np.mean(np.abs(d_[ph == n]) > 1e-6) for n in ('right', 'left', 'up')}
        yaw = still_rel and mov['right'] > 0.6 and mov['left'] > 0.6 and np.sign(tot['right']) == -np.sign(tot['left']) != 0
        pitch = still_rel and mov['up'] > 0.6 and mov['right'] < 0.3
        if yaw or pitch:
            size = {'f32': 4, 'i16': 2, 'i8': 1}[kind]
            print(f'{kind} +0x{i * size:03X} {"YAW" if yaw else "PITCH"}  start {col[0]:.4f}  change right {tot["right"]:+.4f} '
                  f'left {tot["left"]:+.4f} up {tot["up"]:+.4f}  moving R {mov["right"]:.2f} L {mov["left"]:.2f} U {mov["up"]:.2f}')
