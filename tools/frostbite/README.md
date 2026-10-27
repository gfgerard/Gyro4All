# Frostbite camera-finding tools

External Python scripts for finding the player camera of a Frostbite game while it runs (first used on Dead Space
2023, see MODLOG 2026-10-17/18). They attach to the running game with `ReadProcessMemory` (and, for the write tests
only, `WriteProcessMemory`); nothing is injected and no game file is touched. Single-player, offline games without
anti-cheat only.

Requirements: Python 3.13 via `uv`, numpy (`uv run --with numpy ...`). The game must run as the same user (not elevated).
The audio cues use `winsound`, so the player hears when to move the camera.

## Conventions

- Run the scripts from a **work folder outside the repository** (they write `.pkl`/`.npz` result files to the
  current folder), and pass this tools folder as the first argument so they can import `fbmem.py`/`fbtypes.py`:

  ```
  uv run --with numpy python -I <this folder>\find_instances.py <this folder> "<class regex>"
  ```

- The scripts name Dead Space's exe; set `FB_EXE=<exe name>` (e.g. `FB_EXE=bf4.exe`) to run them on another game
  (`fbmem.Proc` reads it).
- `fbtypes.walk_types` starts from the TypeInfo of `DataContainer`; its RVA is the Dead Space default (`0x565E460`).
  For another game, take it from `Gyro4All.log` (`fb: DataContainer TypeInfo at <exe>+0x...`).
- Audio protocol used by the sampling scripts: **1 low beep** = release the stick and stay still; **2 beeps** = right
  stick right; **3 high beeps** = right stick up; **4 beeps** (deltascan) = right stick left; **long low beep** = end.
  The player must actually move the camera: a run with a still camera finds nothing.

## Scripts

| Script | What it does |
|---|---|
| `fbmem.py` | Process access: read/write helpers, region walks, byte/qword search. |
| `fbtypes.py` | TypeInfo walk (Dead Space layout), `getType()` finder (`lea rax,[rip+T]; ret`), qword search over heap/image. |
| `find_instances.py` | Live instances of classes matching a regex: getType -> vtable start -> heap objects. Writes `types.pkl`. |
| `corrwatch.py` + `corrfind.py` | Samples every typed object (~4 Hz) with a reference camera matrix through the beep protocol, then lists fields that follow the camera's yaw/pitch (linear fit, r2 > 0.98). |
| `objseries.py` | Time series of one sampled object's changing fields next to the reference yaw/pitch. |
| `heapscan3.py` + `heapscan3_an.py` | Whole-heap unit 3-vectors and quaternions at rest/after a turn/after a look up, then a 15 Hz follow of the candidates: finds camera matrices (LinearTransform rows). |
| `deltascan.py` | Whole-memory delta scan (Cheat Engine style): floats whose change equals the camera's yaw/pitch change (rad or deg). |
| `follow.py` | Follows the delta-scan candidates during free camera movement and ranks them by fit. |
| `owner.py`, `dumpobj.py`, `objptrs.py` | Which object holds an address (vtable before it), raw dumps, pointers with their classes. |
| `findpair.py` | Every copy of an 8-byte value (e.g. a yaw/pitch pair) in the heap, with its holder. |
| `disasm.py` | Disassembles the running game's code at an RVA (capstone; `uv run --with numpy --with capstone`), resolving rip-relative floats and call targets. |
| `exefile.py` | **Offline**, no game running: reads an exe whose code is plain on disk (Dead Space's Steam build, MODLOG 2026-10-18) and gives function bounds from `.pdata` (`func`), disassembly (`dis`), direct call sites (`calls`), rip-relative references (`refs`), qwords holding an address (`holders`), byte-string search (`find`) and call chains between two functions (`path`). `uv run --with numpy --with capstone python -I exefile.py "<exe>" <command> <rva hex>`. Addresses differ from the EA app build by a few KB: find the same bytes there at run time. |
| `watchyaw.py` | Picks a live copy of the yaw while the camera turns and writes it as `WriteWatch=<address>` into `Gyro4All.ini`, for Gyro4All's in-process write watcher (`WriteWatch=on` must be set when the game starts; results in `Gyro4All.log`, `writewatch:` lines). Any address can be watched the same way by editing `WriteWatch=` while the game runs. |
| `hop.sh` | One WriteWatch hop from Git Bash: beep, set `WriteWatch=<address>`, wait while the player turns, long beep, print the new `writewatch:` log lines. The value can carry a mode and a symbolic address: `x:<address>` (execute), `r:<address>` (reads and writes), `r:di+8` (the right stick's X in the buffer the game passes to DirectInput's `GetDeviceState`; its call site is logged as `dinputpad: the game reads controller ...`). |
| `fieldtest.py` | Write test on one float of an object (only if its value is in a given range), followed for 3 s. |
| `byvtable.py` | Heap objects with a given vtable (exe RVA) and the value at an offset of each. |
| `stickscan.py` + `stickfollow.py` | Whole-memory scan for the right stick's X axis (released / right / left), then a 20 Hz follow of the candidates through the same phases (samples saved to `stickfollow.npz`). |
| `yawtest.py`, `alltest.py` | **Write tests**: rotate a LinearTransform in place / add to every copy of the yaw, then watch whether the camera follows. They verify the value right before writing; don't write to addresses found minutes earlier (heap memory is reused). |
