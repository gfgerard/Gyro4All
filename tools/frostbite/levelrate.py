# Dead Space: turn speed per unit of the EIAYaw action level. Forces the level (state+0x52C) to each value for some
# seconds (rewriting it as fast as possible), sampling the character's camera-derived yaw (state+0x60, radians) and
# the action's hold time (state+0x7AC), then prints the turn speed in degrees per second over the forcing, in thirds
# (a growing speed = a hold-time ramp). Beeps: double low = get ready (5 s), one high beep per level, long low = done.
# Usage: levelrate.py <tools dir> <state hex> [--level 52C] [--angle 60] [--secs 3] [level ...] (default 0.25 0.5)
#   --level / --angle: offsets in the state of the forced level and of the angle (EIAPitch: --level 520 --angle 64; its
#   range is limited, so use short --secs and alternate signs). Stick released, gyro off.
import sys, time, struct, ctypes, math, winsound; sys.path.insert(0, sys.argv[1])
from fbmem import Proc, k32
p = Proc('Dead Space.exe'); state = int(sys.argv[2], 16)
args = sys.argv[3:]; opt = {'--level': '52C', '--angle': '60', '--secs': '3'}
while args and args[0] in opt: opt[args[0]] = args[1]; args = args[2:]
levels = [float(x) for x in args] or [0.25, 0.5]
hw = k32.OpenProcess(0x0038 | 0x0400, False, p.pid)
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
lvl = int(opt['--level'], 16)
LEVEL, YAW, HOLD, SECS = state + lvl, state + int(opt['--angle'], 16), state + lvl + 0x280, float(opt['--secs'])
winsound.Beep(330, 250); time.sleep(0.1); winsound.Beep(330, 250); time.sleep(5)
for level in levels:
    data = struct.pack('<f', level)
    winsound.Beep(1320, 200)
    t0 = time.perf_counter(); samples = []; last = 0
    while (t := time.perf_counter() - t0) < SECS:
        k32.WriteProcessMemory(hw, LEVEL, data, 4, None)
        if t - last > 0.01:
            last = t
            b = p.read(YAW, 4); h = p.read(HOLD, 4)
            samples.append((t, struct.unpack('<f', b)[0], struct.unpack('<f', h)[0]))
    # unwrap the yaw (radians, any wrap) and fit the speed in thirds
    ys, prev, off = [], None, 0.0
    for t, y, h in samples:
        if prev is not None and abs(y - prev) > math.pi: off += -2 * math.pi if y > prev else 2 * math.pi
        prev = y; ys.append(y + off)
    n = len(samples); parts = []
    for k in range(3):
        a, b_ = k * n // 3, (k + 1) * n // 3 - 1
        dt = samples[b_][0] - samples[a][0]
        parts.append(math.degrees(ys[b_] - ys[a]) / dt if dt > 0 else float('nan'))
    print(f'level {level:+.2f}: {n} samples, turn speed (deg/s) by thirds {parts[0]:+.1f} {parts[1]:+.1f} {parts[2]:+.1f}; '
          f'hold time {samples[0][2]:.2f} -> {samples[-1][2]:.2f}', flush=True)
    time.sleep(2 if SECS > 1.5 else 0.5)
winsound.Beep(300, 700)
