# Rotates the HybridRotationInputChimeraBehavior transform (+0x90 right, +0xA0 up, +0xB0 forward) about world Y by
# DEG degrees once, then reads it (and a reference camera matrix copy) back for 2 s.
import sys, time, struct, ctypes, winsound; sys.path.insert(0, sys.argv[1])
import numpy as np
from fbmem import Proc, k32
OBJ, REF, DEG = int(sys.argv[2], 16), int(sys.argv[3], 16), float(sys.argv[4])
p = Proc('Dead Space.exe')
hw = k32.OpenProcess(0x0020 | 0x0008 | 0x0010 | 0x0400, False, p.pid)  # VM_WRITE | VM_OPERATION | VM_READ | QUERY
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
def rows(a): return np.array(struct.unpack('<12f', p.read(a, 48)), dtype=np.float64).reshape(3, 4)
def yaw_of(m, fwd_row=2): f = m[fwd_row, :3]; return np.degrees(np.arctan2(f[0], f[2]))
before = rows(OBJ + 0x90); ref0 = rows(REF)
th = np.radians(DEG); c, s = np.cos(th), np.sin(th)
Ry = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
after = before.copy(); after[:, :3] = before[:, :3] @ Ry.T
buf = struct.pack('<12f', *after.flatten())
winsound.Beep(1000, 150)
ok = k32.WriteProcessMemory(hw, ctypes.c_void_p(OBJ + 0x90), buf, len(buf), None)
print('write', bool(ok), 'obj yaw', round(yaw_of(before), 2), '->', round(yaw_of(after), 2), ' ref yaw', round(yaw_of(ref0), 2))
for i in range(8):
    time.sleep(0.25)
    m, r = rows(OBJ + 0x90), rows(REF)
    print(f'{0.25*(i+1):4.2f}s obj yaw {yaw_of(m):8.2f}  ref yaw {yaw_of(r):8.2f}  ref pitch {np.degrees(np.arcsin(np.clip(r[2,1],-1,1))):6.2f}')
