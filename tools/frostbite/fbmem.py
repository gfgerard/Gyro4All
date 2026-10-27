# Read-only inspection of a running process's memory (ReadProcessMemory). Usage: fbmem.py <exe name> <command>
# Every tool names its game (e.g. Proc('Dead Space.exe')); FB_EXE=<exe name> in the environment overrides it (bf4.exe).
import ctypes, ctypes.wintypes as W, sys, struct, re
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x10, 0x400
class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_ulonglong), ('AllocationBase', ctypes.c_ulonglong), ('AllocationProtect', W.DWORD),
                ('PartitionId', W.WORD), ('RegionSize', ctypes.c_ulonglong), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]
k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.OpenProcess.restype = W.HANDLE

def find_pid(name):
    import subprocess
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH', '/FI', f'IMAGENAME eq {name}'], capture_output=True, text=True).stdout
    m = re.search(r'"[^"]+","(\d+)"', out)
    return int(m.group(1)) if m else None

class Proc:
    def __init__(self, name):
        import os
        name = os.environ.get('FB_EXE', name)
        self.pid = find_pid(name)
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, self.pid)
        mods = (W.HMODULE * 1024)(); need = W.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        self.base = mods[0]
        class MI(ctypes.Structure): _fields_ = [('base', ctypes.c_void_p), ('size', W.DWORD), ('entry', ctypes.c_void_p)]
        mi = MI(); psapi.GetModuleInformation(self.h, W.HMODULE(self.base), ctypes.byref(mi), ctypes.sizeof(mi))
        self.size = mi.size
    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)): return None
        return buf.raw[:got.value]
    def q(self, addr):
        b = self.read(addr, 8); return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None
    def cstr(self, addr, n=128):
        b = self.read(addr, n) if addr else None
        if not b: return None
        z = b.find(b'\0')
        return b[:z].decode('latin1') if z > 0 else None
    def regions(self, begin, end):
        a = begin; m = MBI()
        while a < end and k32.VirtualQueryEx(self.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
            rb, re_ = m.BaseAddress, min(m.BaseAddress + m.RegionSize, end)
            if m.State == 0x1000 and not (m.Protect & 0x101) and (m.Protect & 0xEE): yield rb, re_
            a = m.BaseAddress + m.RegionSize
    def chunks(self, begin, end, size=16 << 20):
        for rb, re_ in self.regions(begin, end):
            for p in range(rb, re_, size):
                b = self.read(p, min(size, re_ - p))
                if b: yield p, b
    def find_bytes(self, pat, begin=None, end=None):
        begin = begin or self.base; end = end or self.base + self.size
        out = []
        for p, b in self.chunks(begin, end):
            for m in re.finditer(re.escape(pat), b): out.append(p + m.start())
        return out
    def find_qwords(self, values, begin=None, end=None):
        begin = begin or self.base; end = end or self.base + self.size
        vals = set(values); out = []
        pats = [struct.pack('<Q', v) for v in vals]
        for p, b in self.chunks(begin, end):
            for pat in pats:
                i = b.find(pat)
                while i >= 0:
                    if i % 8 == 0: out.append(p + i)
                    i = b.find(pat, i + 1)
        return out
    def off(self, a):
        return f'exe+0x{a - self.base:X}' if self.base <= a < self.base + self.size else f'0x{a:X}'
