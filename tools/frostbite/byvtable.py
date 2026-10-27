# Heap objects whose vtable is exe+<rva> (read-only), with the value at an offset of each.
# Usage: byvtable.py <tools dir> <vtable rva hex> [offset hex]
import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import find_values, private_regions
p = Proc('Dead Space.exe'); vt = p.base + int(sys.argv[2], 16); off = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
for a, v in find_values(p, [vt], list(private_regions(p))):
    r = p.read(a + off, 16)
    print(f'0x{a:X}  +0x{off:X} = 0x{a + off:X}  ' + (' '.join(f'{x:9.4f}' for x in struct.unpack('<4f', r)) if r else '?'))
