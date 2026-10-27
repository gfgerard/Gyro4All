# Adds DELTA to the float at OBJ+OFF once (only if its value is within [lo, hi]), then follows it for 3 s.
# Usage: fieldtest.py <tools dir> <obj hex> <off hex> <delta> [lo] [hi]
import sys, time, struct, ctypes, winsound; sys.path.insert(0, sys.argv[1])
from fbmem import Proc, k32
OBJ, OFF, DELTA = int(sys.argv[2], 16), int(sys.argv[3], 16), float(sys.argv[4])
lo, hi = (float(sys.argv[5]), float(sys.argv[6])) if len(sys.argv) > 6 else (-1e9, 1e9)
p = Proc('Dead Space.exe'); hw = k32.OpenProcess(0x0038 | 0x0400, False, p.pid)
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
f = lambda: struct.unpack('<f', p.read(OBJ + OFF, 4))[0]
v = f()
if not (lo <= v <= hi): sys.exit(f'value {v} outside [{lo}, {hi}]: not written')
winsound.Beep(1000, 150)
ok = k32.WriteProcessMemory(hw, ctypes.c_void_p(OBJ + OFF), struct.pack('<f', v + DELTA), 4, None)
print(f'0x{OBJ + OFF:X}: {v:.5f} -> {v + DELTA:.5f} written={bool(ok)}')
for i in range(12):
    time.sleep(0.25); print(f'  {0.25 * (i + 1):4.2f}s {f():.5f}')
