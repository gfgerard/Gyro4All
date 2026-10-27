# id Tech tools

Tools for adding an id Software id Tech game (DOOM 2016, id Tech 6, first) to Gyro4All: finding the player's view
angles and debugging the id Tech module (`src/engines/idTech/`). They are research tools: never shipped, and nothing
here writes into a game's files. Only `poke.py` writes into the running game's memory.

| Tool | What it does | Needs the game running |
|---|---|---|
| [`typeinfo.py`](typeinfo.py) | Lists the game's classes and their fields (with offsets) straight from its exe | No |
| [`dumpimage.py`](dumpimage.py) | Dumps the running exe (decrypted) so `typeinfo.py` / `disasm.py` can read it | Yes |
| [`livemem.py`](livemem.py) | RTTI vtables, heap instances, pointers to them; snapshots around a turn | Yes |
| [`turndiff.py`](turndiff.py) | Floats that changed with a `livemem.py turn` and then held | No |
| [`record.py`](record.py) | Memory spans at 4 Hz while you turn in a fixed order | Yes |
| [`analyse.py`](analyse.py) | Offsets in a recording that follow the turns, grouped by pattern (numpy) | No |
| [`series.py`](series.py) | Chosen floats of a recording, frame by frame | No |
| [`disasm.py`](disasm.py) | Disassembles the function around an exe offset, e.g. a WriteWatch writer (capstone) | No |
| [`poke.py`](poke.py) | Adds degrees to a candidate angle and shows whether it holds (writes memory) | Yes |

## Before you start

- Single-player only: DOOM's multiplayer and SnapMap are online. Test in the offline campaign.
- Back up the saves first: `um backup create "<Saved Games>/id Software/DOOM"`.
- Python 3 for `typeinfo.py` (standard library only).

## 1. List the game's classes and fields: `typeinfo.py`

id Tech exes carry the type info that id's tools generate for the game code (classes, their base class, size and
every member with its type and offset), as static tables in the exe. `typeinfo.py` reads them from the exe on disk, in
the id Tech 6/7 layout (DOOM 2016, DOOM Eternal) or the id Tech 8 one (DOOM: The Dark Ages), whichever finds more
classes (printed on the first line).
Use the exe the game really runs: for DOOM with Vulkan that is `DOOMx64vk.exe` (`DOOMx64.exe` only relaunches it).

```bash
python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --class idPlayer --field "view|angle"
python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --class idPlayer --inherited     # with idActor, idEntity...
python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --find Camera                    # class names
python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --type playerPState_t            # fields of that type
python -I tools/idTech/typeinfo.py "<game>/DOOMx64vk.exe" --all types.txt                  # everything
```

Each field line is `offset size type name`, e.g. `0x002040 12 idAngles viewAngles` (an `idAngles` is three floats:
pitch, yaw, roll, in degrees). Embedded members add up: `idPlayer.physicsObj` (0x14e58) + `idPhysics_Player.viewAngles`
(0x2040) is the player's view angles. Templates (`idList<...>`, `idEntityList<...>`) have no entries of their own.

The Gyro4All module reads the same tables in the running game (`src/engines/idTech/idtech_types.cpp`), so it finds
fields by name and a patch that moves them doesn't break it. Record every finding in `MODLOG.md`.

## 2. Games whose type info has no player fields (Wolfenstein: The New Order, Wolfenstein II)

MachineGames' builds keep only helper structs in the type info, and the PC Game Pass / Xbox app exes can't be read
from disk. The route that found Wolfenstein II's view (MODLOG 2026-10-20), step by step. Keep every dump and recording
in a scratchpad, never in the repository, and back up the saves first.

1. **Read the type info from the running game** (any screen, the tables are static):
   ```bash
   python -I tools/idTech/dumpimage.py NewColossus_x64vk.exe <scratchpad>/image.bin
   python -I tools/idTech/typeinfo.py <scratchpad>/image.bin --find "Player|Physics"
   ```
   No `idPlayer` class (or no fields on it): continue with RTTI. `grep -a -o "\.?AV[A-Za-z_]*Player[A-Za-z_]*@@"` on
   the dump lists the RTTI class names (Wolfenstein II: `idHavokPhysics_Player`, not `idPhysics_Player`).
2. **Find the live objects** (on foot in a level):
   ```bash
   python -I tools/idTech/livemem.py NewColossus_x64vk.exe find idPlayer idHavokPhysics_Player idUsercmdGenLocal
   ```
   An embedded physics object shows as a heap hit inside the player (Wolfenstein II: player+0xeb68); the input
   generator (`idUsercmdGenLocal`) is a static object in the exe ("in the exe at exe+0x...").
3. **Record a guided turn** (the game in front: it pauses without focus; the pad still on the table):
   ```bash
   python -I tools/idTech/record.py NewColossus_x64vk.exe <scratchpad>/rec.pkl \
       --inner idHavokPhysics_Player@eb68 --span player:60000 --span exe+3db7980:4000
   uv run --with numpy python -I tools/idTech/analyse.py <scratchpad>/rec.pkl
   python -I tools/idTech/series.py <scratchpad>/rec.pkl exe+3db7980:10 exe+3db7980:14 player:fb60 player:fb64
   ```
   After the double beep: right, left, up, down, about 2 s each at full stick. Look for a group that falls while
   turning right, and copies a constant apart (view = input angles + delta). Values that move but sit next to
   positions or in a 4x4 block are transforms (bones, attachments), not the view.
4. **Trace or test the candidate.** WriteWatch in Gyro4All (`Gyro4All.ini`: `WriteWatch=on` at start, then
   `WriteWatch=player+<hex>` once a player exists; the module names the local player `player`) logs the code that
   writes it; `disasm.py` on the dump shows that code. Then a live write, with the game in front:
   ```bash
   uv run --with capstone python -I tools/idTech/disasm.py <scratchpad>/image.bin 126B004
   python -I tools/idTech/poke.py NewColossus_x64vk.exe exe+3db7990 1:30 0:15 \
       --watch idPlayer,idHavokPhysics_Player@eb68,eb68+ff8
   ```
   The source holds the write and the screen turns (+30 yaw: left; +15 pitch: down); a copy is overwritten within
   a few frames (Gyro4All logs `the game undid the gyro's camera writes` and starts the stick fallback).
5. Put the offsets in `idtech_engine.cpp` (a fixed layout with a check at start) and record them in `MODLOG.md`.
