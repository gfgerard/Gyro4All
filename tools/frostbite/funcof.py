# The function (start..end, from the exe's x64 unwind table, .pdata, read from the running game) that contains each
# given code address, and the places in the exe image that hold its start as a qword (vtable slots or function tables),
# with the slot's index when the qwords before it are code too (a vtable). Read-only.
# Usage: funcof.py <tools dir> <rva hex> ...   (needs: uv run --with numpy)
import sys, struct; sys.path.insert(0, sys.argv[1])
from fbmem import Proc
from fbtypes import find_values, image_data_regions, exec_regions
p = Proc('Dead Space.exe')
dos = p.read(p.base, 0x40); e_lfanew = struct.unpack_from('<I', dos, 0x3C)[0]
nt = p.read(p.base + e_lfanew, 0x108)
exc_rva, exc_size = struct.unpack_from('<II', nt, 0x18 + 0x70 + 3 * 8)  # data directory 3: exception
table = p.read(p.base + exc_rva, exc_size)
funcs = [struct.unpack_from('<III', table, i) for i in range(0, len(table) - 11, 12)]
print(f'{len(funcs)} functions in .pdata')
codes = list(exec_regions(p)); is_code = lambda v: v and any(rb <= v < re_ for rb, re_ in codes)
starts = {}
for a in sys.argv[2:]:
    rva = int(a, 16)
    lo, hi = 0, len(funcs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e, u = funcs[mid]
        if rva < b: hi = mid - 1
        elif rva >= e: lo = mid + 1
        else: break
    b, e, u = funcs[mid]
    if not (b <= rva < e): print(f'exe+0x{rva:X}: no function entry'); continue
    # chained unwind info: walk back to the primary function's start
    info = p.read(p.base + u, 4)
    flags = info[0] >> 3
    start = b
    while flags & 4:  # UNW_FLAG_CHAININFO: the primary RUNTIME_FUNCTION follows the unwind codes
        n = info[2]
        chain = p.read(p.base + u + 4 + ((n + 1) & ~1) * 2, 12)
        start, _, u = struct.unpack('<III', chain)
        info = p.read(p.base + u, 4); flags = info[0] >> 3
    print(f'exe+0x{rva:X}: in function exe+0x{start:X} (this chunk exe+0x{b:X}..0x{e:X})')
    starts[p.base + start] = a
for addr, fn in find_values(p, set(starts), list(image_data_regions(p))):
    s = 0
    while s < 600 and is_code(p.q(addr - 8 * (s + 1))): s += 1
    print(f'  exe+0x{fn - p.base:X} (from {starts[fn]}) held at {p.off(addr)}: slot {s} of a table starting at {p.off(addr - 8 * s)}')
