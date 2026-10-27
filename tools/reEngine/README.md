# RE Engine tools

Tools for adding a Capcom RE Engine game (Resident Evil 7 and later, Pragmata, ...) to Gyro4All: finding its player
camera and debugging the RE Engine module (`src/engines/reEngine/`). They are research tools: never shipped, and
nothing here writes into a game's files.

| Tool | What it does | Needs the game running |
|---|---|---|
| [`tdbdump.py`](tdbdump.py) | Lists the game's classes and their fields (with offsets) straight from its exe | No |
| [`camera_finder.lua`](camera_finder.lua) | Finds the rotation that turns the player's camera by itself (scan, captures with the stick, live test) and gives its path and pitch sign | Yes, with REFramework |
| [`methoddump.py`](methoddump.py) | Disassembles a class's method in the running game (e.g. a `get_RotationDegree` getter: which field holds the value), read-only | Yes (TDB 71+) |

How a camera gets supported: `camera_finder.lua` finds in game which rotation turns the camera and how to reach it,
`tdbdump.py` lists the classes and fields along that path, and that path becomes a camera model in `src/engines/reEngine/` (like
`requiem_camera.cpp`). Record every finding in `MODLOG.md`.

## Before you start

- **Capcom's anti-tamper.** Newer games (Resident Evil Requiem, Pragmata) crash when anything changes them unless
  [REFramework](https://github.com/praydog/REFramework) is installed, which switches the protection off. Use a
  recent REFramework build: an older one crashed Pragmata during loading. Resident Evil 7 doesn't need it.
- Back up the saves first: `um backup create`.
- Python 3 for `tdbdump.py` (standard library only).

## 1. List the game's camera classes: `tdbdump.py`

Reads the type database (TDB) in the game's exe on disk. Supports TDB 69/70 (RE7, RE2/RE3 RT, RE8), 81 (Monster
Hunter Wilds) and 83 (RE Requiem, Pragmata); it prints the version on the first line.

```bash
python -I tools/reEngine/tdbdump.py "<game folder>/<game>.exe" --find "Camera"
```

```bash
python -I tools/reEngine/tdbdump.py "<game folder>/<game>.exe" --type app.CameraSystem --type app.CameraBlender
```

| Option | Output |
|---|---|
| `--find "<regex>"` | Class names that match, with their parent class |
| `--type <class>` | That class's fields (offset, name, type), its parents' fields too unless `--no-inherited`; `--methods` adds methods |
| `--field "<regex>"` | Every field whose name matches, with the class that declares it |
| `--all <out.txt>` | Every class with its fields, into a file (large: search it afterwards) |

What to look for:
- `app.CameraSystem` (the camera singleton) with `_CameraBlenders`, and `app.CameraBlender` with `_Role` and the
  controller in use (`<BusyCameraController>k__BackingField` or `_BusyCameraController`). RE Requiem and Pragmata
  both have them.
- The player's camera controller classes (`app.Player...CameraController`) and the `via.Quaternion` fields on them
  or on the objects they hold: the rotation the gyro will turn is one of those.
- Pause: `app.PauseManager` (`_CurrentPauseTypeBits` or `_CurrentPauseTypes`).
- The game's own gyro, if any (Pragmata: `app.PlayerCameraController.CameraInputGyro`): the player must turn it off,
  or the motion doubles.

Offsets are from the start of the object's fields, and Gyro4All looks them up by name at run time, so a game update
doesn't break them. Only the names matter.

## 2. Find the rotation that turns the camera: `camera_finder.lua`

A [REFramework](https://github.com/praydog/REFramework) Lua script that does the whole search for you, like the Unreal
module's camera probe, in any game REFramework supports (RE2, RE3 and RE4 remakes, RE7, RE8, RE Requiem, Pragmata, ...):

1. **Scan**: it walks the game's objects (REFramework's managed singletons and the camera's own GameObject, through
   fields, arrays, lists and GameObject components) and keeps every rotation: `via.Quaternion` fields, float
   fields named like yaw, pitch or angle, and both components (`.x`, `.y`) of every `via.vec2` field, where some
   games keep the camera's pitch and yaw as a pair.
2. **Captures**: you turn the camera with the right stick 3 times. Only the rotations that turned exactly like the
   real camera are kept.
3. **Live test**: each of those is turned by 5 degrees, at the same point of the frame where Gyro4All writes
   (before `LateUpdateBehavior`), and put back. The real camera must follow it. A second test on pitch gives the
   `PitchSign` the camera model needs.

The result is the **path** to the rotation (for example
`singleton app.CameraSystem._CameraBlenders[0]._BusyCameraController.<CameraObjectRotation>k__BackingField`), the class
that holds it and its pitch sign: what a camera model in `src/engines/reEngine/` needs.

### Install

1. Install REFramework in the game's folder (next to the game's exe) and start the game once, so that it creates
   its `reframework\` folder. Games without Capcom's anti-tamper (RE7, RE8, RE2/RE3) don't need REFramework to run
   Gyro4All: install it only while you search, then remove it if you like.
2. Copy `tools/reEngine/camera_finder.lua` into the game's `reframework\autorun\` folder.
   If the game is already running: open REFramework's menu (`Insert`) -> **ScriptRunner** -> **Reset scripts**.
3. If the game has its own gyro option, turn it off. Gyro4All can stay installed: just keep the controller still
   during the search (no gyro motion).

### Use

1. Load into gameplay, with the camera free: on foot, not aiming, no cutscene, nothing in front of the camera.
2. Press `Insert`, open **Script Generated UI** -> **Gyro4All camera finder** and press **Find the camera**.
3. **Scan** (a few seconds; the game may stutter): wait until it asks you to keep the camera still.
4. **Captures**: when it says so, turn the camera **with the right stick**, more than 25 degrees sideways and a bit
   up or down, then let go of the stick. Do it 3 times; the menu shows how many rotations are left.
5. **Live test: don't touch the controller.** The camera twitches by 5 degrees a few times, once per rotation left.
6. Read the result:

   | Result | What to do |
   |---|---|
   | `CONFIRMED <path> ... PitchSign=1` (or `-1`) | That's the camera rotation. Several results: take the shortest path (the others are usually the same object reached another way) |
   | `PitchSign=?` | Pitch didn't move the camera (pitch locked here?): repeat looking at the horizon, or decide the sign on screen |
   | `yaw turned the other way` | Unusual: note it; the camera model then needs a yaw sign too |
   | `float ... degrees/radians, sign, yaw/pitch` | The game keeps yaw and pitch as two numbers, not a rotation: a camera model can write those |
   | `via.vec2 .x` / `.y ... degrees/radians, sign, yaw/pitch` | The same, kept as a pair in one `via.vec2` field: the path ends in `.x` or `.y`, and usually both components show up (one yaw, one pitch) |
   | `No rotation followed the camera` / `No rotation turned the camera` | See below |

The result is also written to `reframework\data\gyro4all_camera.txt` in the game's folder and to REFramework's log,
`re2_framework_log.txt`, with the prefix `[Gyro4All camera]`.

7. Check the result on screen before writing the camera model: the pitch sign is worked out from the camera's
   own rotation, assuming it looks down its -Z axis (true in RE Requiem and Pragmata). After the model is written,
   tilting the controller up must look up; if it looks down, flip `kPitchSign`.
8. Also note, for the camera model: which controller class the path goes through in free look, and what changes
   while aiming, in vehicles or in cutscenes (run the search again in those situations to see their rotation).
   Record everything in `MODLOG.md`.

If nothing is found:
- **`scan done: <few> objects visited, 0 rotations`**: the walk had almost nowhere to go. The log lines before it
  say why: `sdk.get_managed_singletons: ..., 0 singletons` (REFramework gave no list; the script then asks for
  singletons by name, `singletons by name: ...`) and `types visited: ...` (where the walk went). The player camera is
  often on the **player's** GameObject, not the camera's (RE7, RE8): add the singleton that holds the player to
  `KNOWN_SINGLETONS` at the top of the script (find it with `tdbdump.py --field "Player"`).
- **No rotation followed the camera**: the camera may be driven by something the scan doesn't reach. Raise
  `DEPTH_CAMERA_ROOTS` / `DEPTH_OTHER_ROOTS` / `MAX_OBJECTS` at the top of the script and try again, or use REFramework's
  **Object Explorer** by hand (open `app.CameraSystem` or the game's camera manager singleton and follow its fields).
- **No rotation turned the camera**: the rotations found are copies that the game rebuilds every frame from the
  real one. The real one may be a value the scan doesn't keep (a matrix, a vector, a float with another name): look
  near the copies' paths in the Object Explorer.

### Remove

Delete `camera_finder.lua` from `reframework\autorun\`. It changes nothing permanently: every test turn is put back.

## 2b. Read what a method does: `methoddump.py`

When the fields you can see are rebuilt every frame (Monster Hunter Wilds: `BaseCamParam.RotationDegree`), the game's
own getter shows where the value really lives. Some exes are encrypted on disk, so this reads the running game's
memory (read-only; nothing is written to the game). Needs Python 3 with `numpy`, and `capstone` for the disassembly
(raw bytes without it):

```
uv run --with numpy --with capstone python -I tools/reEngine/methoddump.py "<game folder>/<game>.exe" --type app.PlayerCameraController
uv run --with numpy --with capstone python -I tools/reEngine/methoddump.py "<game folder>/<game>.exe" --type app.PlayerCameraController --method get_RotationDegree
```

The first lists the type's methods and their addresses, the second disassembles one. In the code the first argument
(`rcx`) is the VM context, `rdx` the object, `r8`/`r9` the arguments; offsets are raw (the object's field base
included: 0x50 for behaviours, 0x10 for plain objects), so subtract the field base to get `tdbdump.py`'s offsets.

## 3. Debug Gyro4All in an RE Engine game

`Gyro4All\Gyro4All.log` (next to the game's exe) shows how far the RE Engine module got:

| Log line | Meaning |
|---|---|
| `re: TDB <n> ready: <count> types` | Type database read |
| `re: TDB <n> is not supported yet` | New TDB version: its layout must be added to `re_tdb.cpp` (and `tdbdump.py`) |
| `re: <count> application entries ...` | The game's per-frame steps, in run order (`FrameEntry=` picks one) |
| `re: gyro frame hooked before LateUpdateBehavior` | Frame hook installed |
| `re: first gyro frame ... on thread <id>` | The hook runs: the gyro is alive |
| `re: no known player camera in this game` | No camera model matches this game's classes: step 2 above, then a new model |
| `re: camera model: ...` / `re: camera <controller>` | Camera model in use / controller in use (changes are logged) |
| `nativepad: the game's controller reports made idle` | The `F10` panel took the controller from the game |

If the game crashes, look in the game's `CrashReport\` folder (Capcom's crash reporter: `CrashReport.txt` gives the
exception and address, `MiniDump.dmp` the stack). An `EXCEPTION_ILLEGAL_INSTRUCTION` at an address inside the game's
exe is the anti-tamper's penalty, not a Gyro4All bug: install or update REFramework. Read a dump with WinDbg's `cdb`:

```bash
cdbX64 -z "<game folder>/CrashReport/<date>/MiniDump.dmp" -c "~*k 8; q"
```
