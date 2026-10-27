import sys, pickle; sys.path.insert(0, sys.argv[1])
import numpy as np
types = pickle.load(open('types.pkl', 'rb')); objs, spans, S, R, P = pickle.load(open('corrwatch.pkl', 'rb'))
addr = int(sys.argv[2], 16); i = [k for k, (a, t) in enumerate(objs) if a == addr][0]
M = np.array([np.frombuffer(r, '<f4') for r in R], dtype=np.float64); fwd = M[:, 8:11]
yaw = np.degrees(np.arctan2(fwd[:, 0], fwd[:, 2])); pitch = np.degrees(np.arcsin(np.clip(fwd[:, 1], -1, 1)))
F = np.array([np.frombuffer(s[i][: spans[i] // 4 * 4], '<f4') for s in S], dtype=np.float64)
U = np.array([np.frombuffer(s[i][: spans[i] // 4 * 4], '<u4') for s in S])
ch = np.nonzero(np.any(U != U[0], 0))[0]
print('object', types[objs[i][1]]['name'], hex(addr), 'size', hex(spans[i]), 'changing offsets', [hex(o * 4) for o in ch])
ts = list(range(0, len(P), max(1, len(P) // 24)))
print('  t  phase  refYaw refPitch | ' + ' '.join(f'+{o*4:03X}'.rjust(9) for o in ch))
for t in ts:
    print(f'{t:3d} {P[t]:5s} {yaw[t]:7.1f} {pitch[t]:7.1f} | ' + ' '.join(f'{F[t, o]:9.4f}' for o in ch))
