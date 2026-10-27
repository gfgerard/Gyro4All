# Frostbite TypeInfo walk (Dead Space 2023 layout) + instance finder, external and read-only.
import sys, struct, re, pickle, os, time
import numpy as np
from fbmem import Proc, MBI, k32
import ctypes

def walk_types(p, seed_rva=0x565E460):
    T0 = p.base + seed_rva
    seen = {T0}; todo = [T0]; types = {}
    while todo:
        t = todo.pop()
        info = p.q(t); nm = p.cstr(p.q(info), 200) if info else None
        if not nm: continue
        raw = p.read(info, 0x70)
        flags, size = struct.unpack_from('<HH', raw, 0xC)
        ptrs = struct.unpack_from('<7Q', raw, 0x38)
        types[t] = dict(name=nm, kind=(flags >> 5) & 0x1F, size=size, parent=ptrs[0] if ((flags >> 5) & 0x1F) == 3 else 0)
        for o in (8, 0x10):
            n = p.q(t + o)
            if n and n not in seen: seen.add(n); todo.append(n)
    return types

def exec_regions(p):
    m = MBI(); a = p.base; end = p.base + p.size
    while a < end and k32.VirtualQueryEx(p.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == 0x1000 and (m.Protect & 0xF0) and not (m.Protect & 0x101):
            yield m.BaseAddress, min(m.BaseAddress + m.RegionSize, end)
        a = m.BaseAddress + m.RegionSize

def find_gettype(p, targets):
    """code addresses of `lea rax,[rip+T]; ret` for T in targets -> {T: [fn...]}"""
    out = {}; tset = set(targets)
    pat = re.compile(rb'\x48\x8D\x05(....)\xC3', re.S)
    for rb, re_ in exec_regions(p):
        for c in range(rb, re_, 64 << 20):
            b = p.read(c, min(64 << 20, re_ - c) + 7)
            if not b: continue
            for m in pat.finditer(b):
                tgt = c + m.start() + 7 + struct.unpack('<i', m.group(1))[0]
                if tgt in tset: out.setdefault(tgt, []).append(c + m.start())
    return out

def private_regions(p, min_size=0):
    m = MBI(); a = 0x10000
    while a < 0x7FFFFFFF0000 and k32.VirtualQueryEx(p.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == 0x1000 and m.Type == 0x20000 and m.Protect in (0x04, 0x40) and m.RegionSize >= min_size:
            yield m.BaseAddress, m.BaseAddress + m.RegionSize
        a = m.BaseAddress + m.RegionSize

def find_values(p, values, regions):
    """addresses (8-aligned) whose qword is in values"""
    vals = np.array(sorted(values), dtype=np.uint64); out = []
    for rb, re_ in regions:
        for c in range(rb, re_, 64 << 20):
            b = p.read(c, min(64 << 20, re_ - c))
            if not b: continue
            a = np.frombuffer(b[: len(b) // 8 * 8], dtype=np.uint64)
            idx = np.nonzero(np.isin(a, vals))[0]
            out.extend((c + int(i) * 8, int(a[i])) for i in idx)
    return out

def image_data_regions(p):
    m = MBI(); a = p.base; end = p.base + p.size
    while a < end and k32.VirtualQueryEx(p.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == 0x1000 and not (m.Protect & 0x101) and (m.Protect & 0xEE):
            yield m.BaseAddress, min(m.BaseAddress + m.RegionSize, end)
        a = m.BaseAddress + m.RegionSize
