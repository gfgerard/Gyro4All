import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
p = Proc('Dead Space.exe'); N = int(sys.argv[2], 16)
for a in [int(x, 16) for x in sys.argv[3:]]:
    raw = p.read(a, N); vt = struct.unpack_from('<Q', raw, 0)[0]
    print(f'== 0x{a:X} vtable {p.off(vt)}')
    for o in range(0, N, 16):
        q = struct.unpack_from('<2Q', raw, o); f = struct.unpack_from('<4f', raw, o)
        fs = ' '.join(f'{x:10.4f}' if abs(x) < 1e6 and (abs(x) > 1e-6 or x == 0) else '         ~' for x in f)
        notes = ' '.join(p.off(x) if p.base <= x < p.base + p.size else (f'ptr {x:X}' if 0x10000 < x < 0x7fffffffffff and x % 8 == 0 else '') for x in q)
        print(f'  +{o:03X} {fs}   {notes}')
