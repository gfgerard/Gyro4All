import sys, time, struct, ctypes, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc, k32
from fbtypes import find_values, private_regions
REF, DELTA = int(sys.argv[2], 16), float(sys.argv[3]); VTS = [int(x, 16) for x in sys.argv[4:]]
p = Proc('Dead Space.exe'); hw = k32.OpenProcess(0x0038 | 0x0400, False, p.pid)
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
objs = [a for a, v in find_values(p, [p.base + v for v in VTS], list(private_regions(p)))]
print(len(objs), 'objects')
def ref():
    m = np.array(struct.unpack('<12f', p.read(REF, 48))).reshape(3, 4); fw = m[2, :3]
    return np.degrees(np.arctan2(fw[0], fw[2])), np.degrees(np.arcsin(np.clip(fw[1], -1, 1)))
pairs = {a: struct.unpack('<2f2f', p.read(a + 0x30, 8) + p.read(a + 0x60, 8)) for a in objs}
for a, v in pairs.items(): print(f'  0x{a:X} +30 ({v[0]:.4f},{v[1]:.4f}) +60 ({v[2]:.4f},{v[3]:.4f})')
yaws = [v[0] for v in pairs.values()] + [v[2] for v in pairs.values()]
common = max(set(np.round(yaws, 4)), key=lambda y: np.sum(np.isclose(yaws, y, atol=1e-4)))
targets = [(a + o, v[i]) for a, v in pairs.items() for o, i in ((0x30, 0), (0x60, 2)) if abs(v[i] - common) < 1e-4]
print(f'writing {len(targets)} copies of yaw {common:.4f} -> {common + DELTA:.4f}')
r0 = ref(); winsound.Beep(1000, 150)
for addr, v in targets:
    now = struct.unpack('<f', p.read(addr, 4))[0]
    if abs(now - v) < 1e-4: k32.WriteProcessMemory(hw, ctypes.c_void_p(addr), struct.pack('<f', v + DELTA), 4, None)
print('ref before', [round(x, 2) for x in r0])
for i in range(8):
    time.sleep(0.25); r = ref()
    print(f'{0.25*(i+1):4.2f}s ' + ' '.join(f'{struct.unpack("<f", p.read(a, 4))[0]:.4f}' for a, v in targets[:6]) + f'   ref yaw {r[0]:8.2f} pitch {r[1]:7.2f}')
