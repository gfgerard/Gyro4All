# Genome tools

Tools for the Genome module (`src/engines/genome/`: Piranha Bytes' ELEX, ELEX II, Risen 3, Risen): checking a new build of
one of these games against the module, finding a value the module needs (a camera field, a menu or aim state), and
testing the module with synthetic input. They are research tools: never shipped, and none of them writes into a game's
files or memory. The engine-neutral camera search (`guided.py`, write tests, breakpoints) is in
[`tools/camtrace/`](../camtrace/README.md); these tools add what is particular to Genome.

| Tool | What it does | Needs the game running |
|---|---|---|
| [`shapes.py`](shapes.py) | Checks the module's code shapes (its byte patterns, read from its source) against a game build | No (Risen 3: a dump) |
| [`pe.py`](pe.py) | Exports, disassembly with export/import names, code references, RTTI vtables, strings; `dump` of a running game | `dump` only |
| [`heapstates.py`](heapstates.py) | Sound-cued search for a state (aiming, a menu) in the camera admin and the player's property sets | Yes |
| [`phases.py`](phases.py) | Runs synthetic-input phases on a test build and prints the module's per-phase stats | Yes |
| [`test-switches.patch`](test-switches.patch) | The module's test-only switches (synthetic gyro and stick, forced fallback): apply, build, test, revert | |

Python 3 with `pefile`, `capstone` and `numpy` (e.g. `uv run --with pefile --with capstone --with numpy python -I ...`).
`heapstates.py` and `phases.py` use camtrace's process access (Windows, 64-bit games).

## How the module finds things

Two routes (the top comments of `genome_engine.cpp` and `genome_risen1.cpp` describe them in full):

- **ELEX, ELEX II, Risen 3** (the engine linked into the exe, `<root>\system\<game>.exe`): no exports, MSVC RTTI kept. The
  module finds its functions by **code shapes** (byte patterns, one list per finder with an entry per game) and classes by
  their RTTI vtables, and reads its offsets from the matched instructions.
- **Risen** (2023 x64, `<root>\bin\Risen.exe` with `Game.dll`, `Engine.dll`, `SharedBase.dll`...): `Game.dll` and
  `Engine.dll` export their C++ names (10,858 and 14,836), so the module finds its functions by name and reads the few
  offsets from those functions' code.

## 1. A new build (a patch, another store): `shapes.py`

When a game update stops the gyro (the log says `genome: no camera hook for this build` and the gyro goes to the game as
right stick), check which shapes still match:

```bash
python -I tools/genome/shapes.py "<game>/system/ELEX2.exe" --source engine
python -I tools/genome/shapes.py "<game>/bin/Game.dll" --source risen1
```

Each pattern is listed with its line in the module and how often it matches. The whole-exe shapes (`ok` / `NOT FOUND` /
`N matches`: the frame builder, turn function and angle call of each game, Risen 3's game mode setter, ELEX II's aim mode)
must match exactly once for the game they were written for; the others (`local`) are looked for inside one function. A
shape that no longer matches: find the function again (it usually moved or got other registers) with `pe.py` and add an
entry for the new build next to the old one, as the per-game entries do.

Risen 3's exe carries Steam's DRM (a `.bind` section: the code is encrypted on disk). Dump the running game first:

```bash
python -I tools/genome/pe.py Risen3.exe dump risen3-dump.exe     # the game running, in gameplay
python -I tools/genome/shapes.py risen3-dump.exe --source engine
```

## 2. Reading the code: `pe.py`

```bash
python -I tools/genome/pe.py "<game>/bin/Game.dll" exports --grep "gCCameraAdmin@@"     # Risen's functions by name
python -I tools/genome/pe.py "<game>/bin/Game.dll" dis 0x802b40                         # a function, calls named
python -I tools/genome/pe.py "<game>/system/ELEX2.exe" vtable gCCombatSystem2_PS        # vtables and slots
python -I tools/genome/pe.py "<game>/system/ELEX2.exe" strings "^IsInAimMode$" --refs   # a name and who uses it
python -I tools/genome/pe.py "<game>/system/ELEX2.exe" refs 0x1aa6200                   # code that reads an address
```

Addresses are RVAs (offsets from the image base, as the module's log prints them after `exe+`). The game's own names
are the shortest way in: Genome registers its script functions by name (`IsInAimMode` led to ELEX II's aim mode), its
camera modifiers are named by condition (`CamModTargetDistance_Cond_RangedAim`), and its reflected properties by name.

## 3. A state the module needs: `heapstates.py`

camtrace's `states.py` finds a state's flag in the exe's static data; Genome keeps most state in heap objects: the
camera admin and the player entity's property sets (`gCCharacterMovement2_PS`, `gCCombatSystem2_PS`...). `heapstates.py`
snapshots those (and what they point to) while the player goes into the state and back, three times, and lists:

- dwords steady within each round that differ in the state (a mode, a flag): the best candidates;
- floats that separate the state cleanly (e.g. a camera distance that eases in when aiming).

ELEX II, aiming (the offsets come from the module's log: `layout from code ... target +0x80`, `player entity ...:
property set tables +0x30 / +0x38`, the camera admin's address from `camera admin ELEX2.exe+0x...`):

```bash
python -I tools/genome/heapstates.py ELEX2.exe --root exe+0x1AC2500 --entity "[[exe+0x1AC2500+0x80]+8]" \
    --tables 0x30,0x38 --state aim --out aim.npz
```

At the high beep go into the state and stay (hold aim; keep the controller still), at the low beep come back. A round
spoiled by an early press or a state that flickers (ELEX II's RT aims only while shooting) still leaves the others; with
no candidate in two rounds the one-round ones are shown. Then read the code around a candidate (`pe.py refs` / `dis`) and
prefer what the game itself checks (a getter or a script function) over a value that merely follows the state: the module
reads its offsets from that code.

## 4. Testing the module: `test-switches.patch` and `phases.py`

The patch adds test-only switches to the module, read from `Gyro4All.ini` whenever it is saved: `GenomeTestYaw` /
`GenomeTestPitch` (synthetic gyro, millidegrees a frame), `GenomeTestStickX` (a synthetic right stick, through the
controller Gyro4All shows as XInput), `GenomeForceNoCamera=1` (the unknown-build fallback: gyro as right stick), and for
ELEX, ELEX II and Risen 3 `GenomeTestNoise` (random tremor) and `GenomeSpuriousFix=0` (the spurious-turn filter off). Each
change logs the previous phase's stats (heading turn per frame: mean, spread, min/max; Risen also the elevation).

```bash
git apply tools/genome/test-switches.patch && bash tools/build-gyro4all.sh
# install that build into the game, start it, load a save, stand still in gameplay, then:
python -I tools/genome/phases.py ELEX2.exe "<game>/system"
git apply -R tools/genome/test-switches.patch && bash tools/build-gyro4all.sh     # never commit or ship the switches
```

`phases.py` runs a standard set (slow and fast yaw both ways, pitch both ways, the stick alone and with the gyro) or the
phases given (`"label:seconds:Key=value,Key=value"`). What to look for: the heading's mean per frame equals what was
sent (0.050 for `GenomeTestYaw=50`) in both directions and at slow rates; the stick plus the gyro add up; with no input
the heading stays put (mean 0: no creep). Risen's numbers from these tests are in MODLOG (2026-11-25).

If the patch no longer applies after module changes, re-create it: apply what still applies (`git apply --reject`), fix
the rest by hand (every line marked `// TEST-ONLY`), and write it again with `git diff` (the module's files and
`src/core/xinput.*`).
