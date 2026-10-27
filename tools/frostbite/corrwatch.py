# Samples every typed object (~4 Hz) plus a reference camera matrix copy through the protocol, for correlating fields
# with the camera's yaw/pitch.
import sys, time, pickle, winsound, struct; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc
from fbtypes import *
REF = int(sys.argv[2], 16)  # address of a right-vector row of a camera LinearTransform copy
p = Proc('Dead Space.exe'); types = pickle.load(open('types.pkl', 'rb'))
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
print(len(objs), 'objects', flush=True)
def beep(f, k):
    for _ in range(k): winsound.Beep(f, 120); time.sleep(0.05)
S, R, P = [], [], []
for name, secs, f, k in [('still', 4, 440, 1), ('right', 4, 880, 2), ('still', 4, 440, 1), ('up', 3, 1320, 3), ('still', 4, 440, 1)]:
    beep(f, k); end = time.time() + secs
    while time.time() < end:
        R.append(p.read(REF, 0x40)); S.append([p.read(a, n) or b'' for (a, t), n in zip(objs, spans)]); P.append(name)
winsound.Beep(300, 600)
pickle.dump((objs, spans, S, R, P), open('corrwatch.pkl', 'wb'))
print(len(S), 'samples')
