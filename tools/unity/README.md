# Unity tools

How to find the camera of a Unity game (Mono or IL2CPP) and debug Gyro4All in one. As with Unreal Engine, everything is
built into Gyro4All (`src/engines/unity/`, the camera probe in `unity_probe.cpp`): there is nothing extra to install,
only settings and keys. This folder holds the instructions, and any Unity-only scripts we add later.

The user-facing reference is [Unity camera profiles](../../docs/ADVANCED.md#unity-camera-profiles) in `docs/ADVANCED.md`.

## Before you start

- Gyro4All installed next to the game's exe and `UnityPlayer.dll`: `bash tools/deploy-gyro4all.sh "<game folder>"`.
- Single-player or offline games without anti-cheat only: `um scan "<game>"` says.
- Play windowed or borderless: the on-screen messages don't show in exclusive fullscreen.
- Back up the saves first: `um backup create`.

## 1. Does the game need a profile?

Play and look at `Gyro4All\Gyro4All.log`:

| Log | What it means |
|---|---|
| `gyro on (target Camera.main (IL2CPP))` and the view turns right | Nothing to do. |
| `unity: ... the game rewrites its Transform`, or `stick fallback: the game undid the gyro's camera writes` | The game rebuilds the camera: go to step 2. The probe also starts by itself. |
| `gyro off: no gameplay view (target camera without mouse look (cursor not locked))` | The cursor is hidden but `Cursor.lockState` isn't Locked: the game has no mouse look here. If the camera should turn, step 2: profiles aren't subject to this rule. |
| `gyro off: no gameplay view (target fixed scene camera)` | The camera doesn't answer the stick or mouse. Nothing to find here, unless it is wrong: then step 2 says which camera turns. |
| The view turns, but around the wrong axis (the world seems tilted) | Yaw is written around the world's up axis, which is wrong under a rotated rig: step 2. |

A game with no `[Unity]` section looks for its own camera in gameplay, with no input (`AutoProbe=1`), once the player has
turned that camera with the stick or mouse or after 45 s on it (`probe: ...: gameplay, the camera may be searched by itself`;
splash and loading screens lock the cursor too): `probe: N of the floats
look like a view angle: nudging them`, then `probe: nudge ...` for each value that turned a camera, the usual `probe: test`
lines, and `probe: camera VERIFIED`. A profile written that way has `Source=auto`, and the first real stick or mouse turn
logs `the stick or mouse turns the yaw that was found: confirmed`, or takes it out (`it isn't the look state`, and `Rejected=`).

## 2. Run the probe

Press `Ctrl+F7` (or set `Probe=1` in `Gyro4All.ini`) in gameplay, then follow the notices: turn with the right stick or
mouse, more than 30 degrees and also up and down, 3 times; then don't touch anything while it tests.

## 3. Read `Gyro4All\Gyro4All-probe.log`

- `cam <hierarchy> | main=1 ortho=0 view=(...)`: every camera. `main=1` is `Camera.main`.
- `val <class> (camera rig): <field>=<value>`: script floats with look-like names, those on a camera's rig first.
- `yaw`/`pitch <class> <path> r=... per degree ...`: floats that followed the camera. `r` close to 1 or -1 and a per-degree
  of 1 or 0.01745 are real angles in degrees or radians.
- `probe: test yaw <class> <path>: asked 3.0, the camera moved 3.0 ..., value kept: turned`: the live test. `value rewritten by
  the game` is an output or a copy; `the camera moved 0.0` with `kept` is a value the camera doesn't read.
- `probe: PitchPath=...` and the other keys: what went into the profile.

`F7` alone adds a fresh `cam`/`val` dump at any moment, for looking at a camera the probe didn't get to.

## If the probe finds nothing

- **No camera turned:** a fixed scene camera, or look input the game doesn't take here. Try where the camera clearly moves.
- **Cameras turn, no value passes:** the game keeps its look state out of reach of the scan (a static field, two object
  references deep, a Transform with no `CharacterController` or `Rigidbody`, native code). The `val` lines may still name it: try
  it by hand in `[Unity]` (`YawParent=` for a body's Transform). A static state isn't supported by profiles yet. After that,
  `tools/camtrace/` as for any engine.
