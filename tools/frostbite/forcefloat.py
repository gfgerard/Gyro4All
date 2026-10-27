# Write test: keeps a float at an address forced to a value for some seconds (rewriting it as fast as possible, since
# the game rewrites it every tick), then lets go. Beeps: double low = get ready (5 s); one high beep = forcing starts;
# long low beep = released.
# Usage: forcefloat.py <tools dir> <address hex> <value> [seconds, default 2]
import sys, time, struct, ctypes, winsound; sys.path.insert(0, sys.argv[1])
from fbmem import Proc, k32
p = Proc('Dead Space.exe'); a = int(sys.argv[2], 16); v = float(sys.argv[3]); secs = float(sys.argv[4]) if len(sys.argv) > 4 else 2
hw = k32.OpenProcess(0x0038 | 0x0400, False, p.pid)
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
data = struct.pack('<f', v)
winsound.Beep(330, 250); time.sleep(0.1); winsound.Beep(330, 250); time.sleep(5)
print('before', struct.unpack('<f', p.read(a, 4))[0], flush=True)
winsound.Beep(1320, 200)
end, n = time.time() + secs, 0
while time.time() < end:
    k32.WriteProcessMemory(hw, a, data, 4, None); n += 1
winsound.Beep(300, 700)
print(f'{n} writes; after', struct.unpack('<f', p.read(a, 4))[0])
