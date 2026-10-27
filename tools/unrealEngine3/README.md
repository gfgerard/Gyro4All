# Unreal Engine 3 tools

How to look inside a running Unreal Engine 3 game (64-bit) for Gyro4All's `Unreal3` module (`src/engines/unrealEngine3/`):
its objects, names and properties, the player's camera rotation, and which fields change when a menu opens. Made with
Mass Effect Legendary Edition 1 (MODLOG 2026-10-22), meant for ME2/ME3 and other UE3 games. Research tools: never shipped,
nothing here writes into a game's files.

| Command | What it does | Writes game memory |
|---|---|---|
| `find` | Finds and prints the reflection layout: GObjects, the UObject fields, the name table, the property fields | No |
| `objects` | Lists live objects (address, class, full name), filtered by regex | No |
| `props <Class>` | A class's own properties: offset, size, type, bool mask | No |
| `scene` | GameEngine → LocalPlayer → controller, pawn, camera, WorldInfo; the rotations and the pause/menu bools | No |
| `snap <out.pkl>` / `diff <a> <b>` | Snapshot of every object and the scene objects' bytes; what differs between two | No |
| `write-test` | Holds the controller's pitch up then down, then turns its yaw, so you see whether the camera follows | **Yes** |

Nothing is fixed per game. `find` locates, in the running game: GObjects (a TArray of objects that hold their own index),
UObject's Outer/Name/Class (Class is the field where the class `Class` points to itself), the name table (BioWare's SFXName
chunks, as in Mass Effect LE, or UE3's `GNames`, whichever reads "Class" for that object), UProperty's Offset, ElementSize
and ArrayDim (from `Core.Object.Matrix`'s planes), UBoolProperty's bitmask and UStruct's SuperField. The result is
cached per process in `%TEMP%\ue3tool-<pid>.json`; `--refresh` searches again.

## Before you start

- The game running and in a level (not the main menu) for `scene`, `snap` and `write-test`.
- Python 3 on Windows (standard library only). From Git Bash:
  `python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe find`.
- Back up the saves first (`um backup create`), especially before `write-test`.
- Single-player or offline games without anti-cheat only.
- Memory dumps, snapshots and other output stay outside the repository.

## 1. Find the layout: `find`

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe find
```

The first run takes a few seconds (it walks every object). On Mass Effect LE 1 it prints GObjects at `exe+0x1770670`,
`sfx` names at `exe+0x16A2090`, UObject `Outer +0x40, Name +0x48, Class +0x50`, UProperty `Offset +0x94`, the bool mask
at `+0xD0`, and the offsets the module needs (`Engine.GamePlayers`, `Player.Actor`, `Actor.Rotation`,
`Controller.Pawn`, `Actor.WorldInfo`, `WorldInfo.Pauser`). Anything "not found" there is what the module would miss too.

## 2. Look around: `objects`, `props`

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe objects --class "PlayerController|Camera$"
```

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe props SFXPlayerCamera
```

`props` lists only the class's own properties (inherited ones are under the parent's name, e.g. `props Actor`).

## 3. The camera: `scene`, then `write-test`

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe scene
```

Prints the scene objects, the controller's and the pawn's `Rotation` (degrees and raw int32 units, 65536 per turn) and
WorldInfo's `bPlayersOnly`, `bIsMenuLevel` and `Pauser`. In UE3 the camera normally follows the controller's
Rotation (the counterpart of UE4's ControlRotation). To check it on screen:

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe write-test --wait 15 --hold 3
```

It waits 15 s (put the game in front), holds pitch at +20° then -20° for 3 s each, puts it back, then turns yaw by
+30°, printing the controller's rotation after each step. If the camera tilts and turns with it, that rotation is the
view: the module writes it. If the value snaps back or the camera ignores it, look for the copy that drives the camera
(`props` on the camera class, e.g. `CameraDesiredPosition`, `CameraCache`). The game limits pitch itself (-60° in ME1).

## 4. Pause, menus, cutscenes: `snap` and `diff`

Take one snapshot in plain gameplay and one in the state to detect (the pause menu held open, a conversation, a
cutscene), then compare:

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe snap "%TEMP%/play.pkl"
```

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe snap "%TEMP%/menu.pkl"
```

```bash
python -I tools/unrealEngine3/ue3tool.py MassEffect1.exe diff "%TEMP%/play.pkl" "%TEMP%/menu.pkl"
```

`diff` lists the classes with more or fewer objects (a menu adds its `BioSFPanel` / `GFxMovie` objects in ME1), the
new objects, and for each scene object the properties of its class chain that changed (bools by name with old → new).
Switching to the terminal takes focus from the game: take both snapshots the same way, so focus isn't the difference.
Confirm a candidate on a third snapshot, back in gameplay, before using it. ME1's pause menu: `WorldInfo.bPlayersOnly`
0 → 1 (`Pauser` stays empty and game time keeps running); flags that flip every frame (`Actor.bTicked`) are noise.

Record every finding in `MODLOG.md` (exact class, property and offset names), dead ends too.
