import sys, struct, pickle; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import *
p = Proc('Dead Space.exe'); types = pickle.load(open('types.pkl', 'rb'))
OBJ, N = int(sys.argv[2], 16), int(sys.argv[3], 16)
# vtable -> class (via getType in vtable): resolve by scanning vtable slots for lea rax,[TypeInfo];ret
codes = list(exec_regions(p))
def cls_of(obj):
    vt = p.q(obj)
    if not vt or not (p.base <= vt < p.base + p.size): return None
    for s in range(0, 260):
        fn = p.q(vt + 8 * s)
        if not fn or not any(rb <= fn < re_ for rb, re_ in codes): break
        b = p.read(fn, 8)
        if b and b[:3] == b'\x48\x8d\x05' and b[7] == 0xC3:
            t = fn + 7 + struct.unpack('<i', b[3:7])[0]
            if t in types: return types[t]['name']
    return '?vt ' + p.off(vt)
raw = p.read(OBJ, N)
for o in range(0, N, 8):
    q = struct.unpack_from('<Q', raw, o)[0]; f = struct.unpack_from('<2f', raw, o)
    note = ''
    if p.base <= q < p.base + p.size: note = p.off(q)
    elif 0x10000 < q < 0x7fffffffffff and q % 8 == 0:
        c = cls_of(q); note = f'-> {q:X} {c or ""}'
    print(f'+{o:03X} {q:016X}  {f[0]:12.4f} {f[1]:12.4f}  {note}')
