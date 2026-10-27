# Picks live copies of the camera's (yaw, pitch) pair (Dead Space: untyped objects with these vtables, pair at +0x30
# and +0x60) while the player turns the camera, and sets the first one as WriteWatch=<address> in the given ini, so
# Gyro4All's write watcher logs the code that writes it. Usage: watchyaw.py <tools dir> <Gyro4All.ini> [vtable rva ...]
import sys, time, struct, re; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import find_values, private_regions
INI = sys.argv[2]
VTS = [int(x, 16) for x in sys.argv[3:]] or [0x3FC4580, 0x45294D0]
p = Proc('Dead Space.exe')
objs = [a for a, v in find_values(p, [p.base + v for v in VTS], list(private_regions(p)))]
def pair(a): r = p.read(a + 0x30, 8); return struct.unpack('<2f', r) if r else None
first = {a: pair(a) for a in objs}
print(len(objs), 'objects; turn the camera now...', flush=True)
time.sleep(2.0)
live = [a for a in objs if first[a] and pair(a) and abs(pair(a)[0] - first[a][0]) > 1e-3 and abs(first[a][0]) < 10]
print(len(live), 'live copies:', ' '.join(f'0x{a + 0x30:X}' for a in live[:12]))
if not live: sys.exit('no live copy (was the camera turning?)')
target = live[0] + 0x30
text = open(INI, encoding='utf-8', errors='replace').read()
text = re.sub(r'(?m)^WriteWatch=.*$', f'WriteWatch=0x{target:X}', text)
open(INI, 'w', encoding='utf-8', newline='').write(text)
print(f'WriteWatch=0x{target:X} written to the ini')
