import sys, struct, re; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import private_regions
p = Proc('Dead Space.exe')
pat = p.read(int(sys.argv[2], 16), 8)
print('pattern', struct.unpack('<2f', pat))
hits = []
for b, e in private_regions(p):
    for c in range(b, e, 32 << 20):
        r = p.read(c, min(32 << 20, e - c))
        if not r: continue
        for m in re.finditer(re.escape(pat), r): hits.append(c + m.start())
for h in hits:
    # owner vtable: nearest qword in exe image before (<= 0x200)
    own = None
    for back in range(0, 0x200, 8):
        q = p.q((h & ~7) - back)
        if q and p.base <= q < p.base + p.size: own = ((h & ~7) - back, p.off(q)); break
    print(f'0x{h:X}', f'object 0x{own[0]:X} +0x{h-own[0]:X} vt {own[1]}' if own else '')
print(len(hits), 'hits')
