"""Global Ctrl+Shift+F9 marker for benchmarks: appends the QPC tick of each press to a file.
Pairs of marks (start, stop) select the measured window in a PresentMon --qpc_time capture.
Usage: python bench_marker.py <marks file> <stop flag file>"""
import ctypes, os, sys, time

user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
VK_CONTROL, VK_SHIFT, VK_F9 = 0x11, 0x10, 0x78

def qpc():
    v = ctypes.c_longlong()
    kernel32.QueryPerformanceCounter(ctypes.byref(v))
    return v.value

def down(vk):
    return user32.GetAsyncKeyState(vk) & 0x8000 != 0

marks, stop_flag = sys.argv[1], sys.argv[2]
was = False
while not os.path.exists(stop_flag):
    now = down(VK_CONTROL) and down(VK_SHIFT) and down(VK_F9)
    if now and not was:
        with open(marks, "a") as f:
            f.write(f"{qpc()}\n")
        kernel32.Beep(1000 if sum(1 for _ in open(marks)) % 2 else 600, 120)  # high = start, low = stop
    was = now
    time.sleep(0.005)
