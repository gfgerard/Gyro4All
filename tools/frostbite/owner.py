import sys, struct, pickle; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import exec_regions
p = Proc('Dead Space.exe'); types = pickle.load(open('types.pkl', 'rb'))
codes = list(exec_regions(p)); is_code = lambda v: v and any(rb <= v < re_ for rb, re_ in codes)
def cls_of_vt(vt):
    # vtable start: slots that are code; find a getType-like fn: lea rax,[rip+T]; ret with T a known type
    for s in range(0, 400):
        fn = p.q(vt + 8 * s)
        if not is_code(fn): break
        b = p.read(fn, 8)
        if b and b[:3] == b'\x48\x8d\x05' and b[7] == 0xC3:
            t = fn + 7 + struct.unpack('<i', b[3:7])[0]
            if t in types and types[t]['kind'] == 3: return types[t]['name'], s
    return None, None
for a in [int(x, 16) for x in sys.argv[2:]]:
    found = None
    for back in range(0, 0x800, 8):
        base = (a & ~7) - back
        vt = p.q(base)
        if vt and p.base <= vt < p.base + p.size and is_code(p.q(vt)):
            nm, slot = cls_of_vt(vt)
            found = (base, nm, slot, p.off(vt)); break
    if found:
        base, nm, slot, vto = found
        print(f'0x{a:X}: object 0x{base:X} +0x{a - base:X}  class {nm} (getType slot {slot}), vtable {vto}')
    else: print(f'0x{a:X}: no vtable within 0x800 bytes before')
