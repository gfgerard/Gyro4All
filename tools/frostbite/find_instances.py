import sys, time, pickle, os; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import *
pat = sys.argv[2]  # regex on class names
p = Proc('Dead Space.exe'); t0 = time.time()
cache = 'types.pkl'  # in the work folder, never in the repository
types = pickle.load(open(cache, 'rb')) if os.path.exists(cache) else walk_types(p)
pickle.dump(types, open(cache, 'wb'))
targets = {t: v for t, v in types.items() if v['kind'] == 3 and re.search(pat, v['name']) and not v['name'].endswith('[]')}
print(f'{len(types)} types, {len(targets)} targets ({time.time()-t0:.1f}s)')
gt = find_gettype(p, targets); print(f'getType fns for {len(gt)} classes ({time.time()-t0:.1f}s)')
fns = {f: t for t, fs in gt.items() for f in fs}
slots = find_values(p, fns.keys(), list(image_data_regions(p)))
print(f'{len(slots)} vtable slots ({time.time()-t0:.1f}s)')
# vtable start: assume getType is slot 0..2; keep candidates
codes = list(exec_regions(p))
is_code = lambda v: v and any(rb <= v < re_ for rb, re_ in codes)
vt = {}
for addr, fn in slots:
    s = 0
    while s < 200 and is_code(p.q(addr - 8 * (s + 1))): s += 1
    vt[addr - 8 * s] = (fns[fn], s)
objs = find_values(p, vt.keys(), list(private_regions(p)))
print(f'{len(objs)} heap refs ({time.time()-t0:.1f}s)')
from collections import Counter
cnt = Counter((vt[v][0], vt[v][1]) for a, v in objs)
res = {}
for a, v in objs: res.setdefault(vt[v][0], []).append((a, v, vt[v][1]))
for (t, s), n in sorted(cnt.items(), key=lambda x: types[x[0][0]]['name']):
    print(f'  {types[t]["name"]:70s} slot{s} x{n}  e.g. {[hex(a) for a, v, ss in res[t] if ss == s][:3]}')
pickle.dump(res, open('instances.pkl', 'wb'))
