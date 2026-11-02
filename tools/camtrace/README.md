# camtrace: find a game's camera by following its data flow

An engine-neutral way to find where a native 64-bit game keeps the view it turns with the mouse or stick. Start from any
copy of the view at a fixed address, write-test it, and if the game undoes the write, put a hardware write breakpoint on
it. The code that hits shows where the copy comes from. Repeat until a write sticks and the view follows. Made with Metro
Exodus Enhanced Edition (MODLOG 2026-10-25), where it found the view angles in three breakpoints after whole-memory value
searches had failed for hours: the angles were fine, but every searchable copy was a temporary on a thread stack.

**When to use it:** as soon as the engine's own first steps don't find the camera (no reflection, no known global, a
value search that turns up only copies the game rewrites). Research tool: never shipped, it writes nothing into the
game's files.

**Start with the guided search** (`guided.py`, below): a minute of sound-cued moves, nothing saved to disk, no debugger
and no writes, so it also works on Denuvo and Themida games. It finds the view's copies and any stored source in one go.
Then write-test what it lists, and only use `bp` when every candidate is a copy the game rewrites. **On games with
anti-debugging (Denuvo, Themida) use Gyro4All's own write watcher instead of `bp`** (below).

## Guided search (start here)

`uv run --with numpy python -I tools/camtrace/guided.py <exe> [--ref ADDR] [--eye ADDR] [--poses 6] [--exe-only] [--out FILE]`

The user stands still in gameplay (solo, nothing moving nearby helps) with the game in front. Three rising beeps: the
first snapshot is taken. Then at each **high beep** the user turns left or right (30-170 degrees) **and** looks up or
down (15-60 degrees), alternating directions, for 2.5 s; at **two low beeps** they stop and stay still until the next
high beep (after the second move the comparison takes up to a minute). Three falling beeps: done.

- Poses 0-2 are full snapshots held in memory (exe static data + heap, not thread stacks: about twice the game's working
  set of RAM); later poses re-read only the candidates, so they take a fraction of a second.
- The view at each pose comes from a forward-vector copy: `--ref`, or by itself the biggest group of identical unit
  vectors that turned in both of the first two moves (it prints the groups; check the reference it picks).
- What it keeps, through every move: angles (float32/64, int16/32) in degrees, radians, turn fractions, grads, 3600,
  4096, 16384, 65536 or 2^32 per turn, either sign; values proportional to the turn at any scale (accumulated mouse
  counts, fixed point), with the ratio per degree; unit directions in rows or columns of a matrix; quaternions of the
  camera or of a yaw-only body; non-unit directions; with `--eye` (an eye position, often right after a view copy),
  points whose direction from the eye follows the view (aim or look-at targets). Yaw and pitch separately.
- The first two moves must each change yaw and pitch by 5 degrees or more, otherwise those tests are dropped (it says so).
- `--exe-only` searches only the exe's static data: much faster, but misses heap objects.

Read the result: the directions and quaternions are mostly copies (expected). Write-test every listed value
(`write-test <addr> --yaw 20` for a direction, `--add 5` or `--add 0.1` for an angle in degrees or radians, or the ratio
times some degrees for "any unit"). The one that keeps the write and moves the view is the source. If nothing keeps it,
follow a copy with `bp` (step 3 below, not on anti-debugging games). Zombie Army 4 (MODLOG 2026-11-02): only copies,
no stored source; the view is rebuilt every frame.

## Without a debugger: Gyro4All's WriteWatch (Denuvo, Themida)

The same breakpoints from inside the game: Gyro4All sets the hardware breakpoint on the game's threads itself and
catches it with its own exception handler, so no debugger is attached (it found Dead Space's camera source, with Denuvo,
without a reaction). Install Gyro4All in the game and put `WriteWatch=on` in `Gyro4All\Gyro4All.ini` before starting
the game; then, while it runs, set `WriteWatch=` to an address (re-read every 500 ms) and turn the camera for
`WriteWatchSeconds` (10). Every instruction that writes there is logged once in `Gyro4All.log` (`writewatch:` lines)
with its registers, callers and code bytes, then how often each wrote: the same as `bp`, without the debugger.

- Addresses: hex, `exe+<hex offset>` (as `guided.py` and `vectors` print them) or an engine module's name.
- `WriteWatch=r:<address>`: reads and writes; `WriteWatch=x:<address>`: a function's entry (`WriteWatchFields`).
- **Forward from the input**, when following copies back leads nowhere: the game's own input buffers, plus a hex offset.
  `r:ri+24` is the X movement of the first mouse report the game reads with Raw Input (`+28` the Y), `r:xi+c` the right
  stick's X in its last `XInputGetState`, `r:di+8` a DirectInput pad's. The code that reads it is the game's input
  layer; watch where it stores the value (`r:` on that), and so on, until the code that adds it to the view angles:
  that store is the source, or the place to hook. A buffer on the caller's stack (the log says so) is reused by other
  code: there, keep only the reads right after the call.
- Remove `WriteWatch` for playing: each hit stops a game thread for an exception.

## Commands

| Command | What it does | Writes game memory |
|---|---|---|
| `snap <out> [--heap]` | Snapshot of the exe's static data (and the heap, not thread stacks) | No |
| `vectors <a> <b> [--turn D]` | Unit 3-vectors that turned between two snapshots: copies of the view direction | No |
| `find-vec x,y,z` | Every copy of a direction in memory (stride 1, 2, 4 floats); thread stacks marked | No |
| `ptrs <addr> [--range R] [--outer R]` | Pointer paths to an object from the exe's static data: `[exe+X]+off` and through one heap object, `[[exe+X]+a]+off` (`--range`: how far before addr the object may start; `--outer`: the same for the holding object). Check the candidates survive a level change | No |
| `dump <addr>` / `watch <addr>` | Memory around an address; values while you move | No |
| `write-test <addr> --add R` or `--yaw D` | Waits for the game in front, writes once, shows whether the game kept it | **Yes** |
| `bp <addr>` | One-hit hardware write breakpoint through cdb (`-pd`), prints who wrote it, detaches | No (debugger) |
| `bp <addr> --exec [--cond EXPR]` | The same when the instruction at addr runs (registers there, e.g. right after a getter returns); `--cond` lets hits go until a cdb expression holds (`"poi(@rcx+0x70) == 0x..."`) | No (debugger) |
| `bp <addr> --read [--skip BASE:SIZE]` | The same on reads too (cdb `ba r`): who uses a value, e.g. a controller's stick in its device object; `--skip` its writer | No (debugger) |
| `bp ... --timeout SECONDS` | Without a hit by then (default 120 s), breaks in, clears the breakpoint and detaches. **Never kill cdb (or wrap `bp` in `timeout`)**: the hardware breakpoint stays in the game's debug registers and the next access crashes the game | No (debugger) |
| `refs <lo> <hi>` / `sig <pattern>` | Offline, the exe file: code referring to an address range; a signature's matches | No |

Addresses are absolute hex, exe-relative (`exe+0x30021e0`) or pointer paths (`[[exe+0x1696ce0]+0x30]+0x9e0`).

## Before you start

- Single-player or offline games without anti-cheat **or anti-debugging** (Themida, Denuvo and similar may detect the
  debugger): don't use `bp` on those; use Gyro4All's WriteWatch (above).
- Back up the saves (`um backup create`) and have the user save in game: a debugger session ended once with the game
  gone (no crash report), and an attach without `-pd` kills the game when the debugger quits.
- Python 3 with numpy (`pefile` and `capstone` for `refs`/`sig`), from the repository root:
  `uv run --with numpy python -I tools/camtrace/camtrace.py MetroExodus.exe snap a.pkl`
  (`uv run --with pefile --with capstone --with numpy python -I tools/camtrace/camtrace.py "<path to exe>" refs ...`).
- cdb: WinDbg from the Microsoft Store (`cdbX64.exe` in `%LOCALAPPDATA%\Microsoft\WindowsApps`) or the Windows SDK
  debuggers, or set `CDB`.
- Many games pause when not in front: reads then show still values (fine for snapshots), and a write or a breakpoint
  only fires once the user switches back. `write-test` waits for the game to be in front; `bp` fires on the next write.
- Keep snapshots and outputs out of the repository (they are game memory).

## Steps

1. **A copy at a fixed address.** The user faces a diagonal (not along a wall: axis-aligned directions have thousands
   of false matches) and looks some degrees up or down. `snap a.pkl`, the user turns about 90 degrees in place, `snap
   b.pkl`, then `vectors a.pkl b.pkl --turn 90`: directions in the exe's static data that turned. The best targets are
   exe-relative (`exe+...`): their address never changes. If none, `snap --heap` the same way, or `find-vec` with a
   direction read from one of them (`find-vec exe+...` reads it right before the search). Run `find-vec` while the game
   is paused (in the background): in gameplay the view sways a little between the read and the search, and only the
   freshest copies (on stacks) still match. Never use a thread-stack copy (`find-vec` marks them): it's a temporary, and a
   breakpoint there catches the stack probe, not the camera.
2. **Write-test it**: `write-test exe+... --yaw 20` (a direction) or `--add 0.35` (a float). Undone within a frame:
   it's a copy, go on. Kept, and the user saw the view jump: that's the source, done. Kept but the view didn't move: a
   dead copy, pick another.
3. **Breakpoint it**: `bp exe+...` (the address of the first float written). The user switches to the game; one hit,
   then the debugger detaches. It prints the writing code, the registers (exe-relative where they point into the exe)
   and the call stack; the full output (disassembly before the write, the stack) is in the file it names.
4. **Read where the written values come from** in that disassembly: the registers the stores read from (a source
   struct passed in, a pointer loaded from `[rip+...]`, the caller's arguments). `dump` the source live, check it holds
   the same view, and go back to step 2 with it. A routine that only copies (`movups` from `[r8]` to `[rcx+...]`):
   look at the caller (the first return address in the stack) with `refs`/a disassembler, or break on the source.
5. **Done when a write sticks and the view follows.** Look around the source for float angles too (`dump`): Metro's
   player held `pitch, yaw, roll` right next to its camera pointer. Then make it findable on every run: a code
   signature for the global (`refs` on the global's RVA lists the code that loads it; `sig` checks a pattern resolves to
   one target), plus the pointer path and offsets, as the engine module does (`data_from_code` in the 4A module).

## What it found in Metro Exodus (for reference)

The static view matrix `exe+0x30021e0` (in a camera object) ← the camera block `exe+0x30049b0` ← the engine's camera
globals `exe+0x17033d0` (written in double precision from the camera object `[player+0x9d0]`) ← the player's `float
pitch, yaw, roll` at `[[exe+0x1696ce0]+0x30]+0x9e0`, which the gyro now writes. The 4 MB blocks full of view matrices were
worker-thread stacks.
