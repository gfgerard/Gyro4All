# Unreal Engine tools

How to find the camera of an Unreal Engine (UE4/UE5) game and debug Gyro4All in one. Unlike the RE Engine tools,
everything here is built into Gyro4All itself (`src/engines/unrealEngine/`, the camera probe in `ue_probe.cpp`):
there is nothing extra to install, only settings to switch on. This folder holds the instructions, and any
Unreal-only scripts we add later.

The full reference, with a real Cronos session as the example, is
[Finding the camera of a new game](../../docs/ADVANCED.md#finding-the-camera-of-a-new-game) in `docs/ADVANCED.md`.

## Before you start

- Gyro4All installed in the game's `<Project>\Binaries\Win64` folder (or `Binaries\WinGDK` for Game Pass):
  `bash tools/deploy-gyro4all.sh "<game>/<Project>/Binaries/Win64"`.
- Single-player or offline games without anti-cheat only.
- Play windowed or borderless: the on-screen messages don't show in exclusive fullscreen.
- Back up the saves first: `um backup create`.

## 1. Does the game need a profile?

Most UE games turn the camera through `PlayerController.ControlRotation`, which needs no profile. Play a few seconds
moving the controller, quit, and open `Gyro4All\Gyro4All.log`:

| Log line | Meaning |
|---|---|
| `camera: confirmed, the camera follows the gyro` | Works as is. Done. |
| `camera: the gyro turned ... but the camera moved only 0: the game ignores it` | The game keeps its camera elsewhere: step 2 |
| `view=none` in the `status:` lines during gameplay | No pawn possessed (normal in menus); in gameplay the game may need a profile |
| `motion=0` in the `status:` lines | Controller problem, not camera: see `docs/TROUBLESHOOTING.md` |

## 2. Let the probe find the camera

**Without restarting** (default): when the camera ignores the gyro, the probe starts by itself (`AutoProbe=1`) and
says so on screen. You can also start it where you are with the probe key: add `ProbeKey=Ctrl+F7` to the
`[Gyro4All]` section of `Gyro4All\Gyro4All.ini`.

**From the start:** set `Probe=1` in the `[Gyro4All]` section of `Gyro4All\Gyro4All.ini` and restart the game.

Then, in gameplay:
1. Stand still for a second (`probe: automatic capture ready`).
2. Turn the camera **with the right stick or the mouse**, more than 30 degrees sideways and a bit up or down, then
   stop. Repeat until the on-screen notice says 3 captures are done.
3. **Don't touch the controller** during the live test that follows: Gyro4All turns each candidate rotation by 3
   degrees and keeps the one the camera follows.
4. The result is written to the game's profile, `Gyro4All\profiles\<game exe>.ini`, and used at once. The log says
   `probe: camera learned: <rotation> added to ... as view <id>` (during play) or names the profile it wrote.

`F7` / `F8` take manual snapshots/diffs instead (with `Probe=1`): see the full guide for reading
`Gyro4All\Gyro4All-probe.log` by hand.

## 3. Keep the result

- Copy the game's profile from `Gyro4All\profiles\` into this repository's `profiles/` folder, so it ships with the
  next release.
- Set `Probe=0` again (the automatic probe does it itself when it is done).
- Record the game, its UE version, the rotation found and how it was tested (controller, USB/Bluetooth, Steam Input
  on/off) in `MODLOG.md`, and add the game to `docs/GAMES.md` once it is verified in game.

## 4. Debug Gyro4All in an Unreal game

- `Gyro4All\Gyro4All.log`: the `status:` line every 10 s gives the view, player controller, pawn, view target,
  pause, cursor, gyro and applied turn. The [Log reference](../../docs/ADVANCED.md#log-reference) explains each line.
- `Gyro4All\Gyro4All-probe.log`: what the probe saw (snapshots, diffs, live test).
- Crashes: UE's crash reporter writes to `<Project>\Saved\Crashes\` (or the game's `%LOCALAPPDATA%` folder); read
  the `.dmp` with WinDbg's `cdb` (`cdbX64 -z <dump> -c "~*k 8; q"`) and look for `winmm` or `gyrolib` frames.
- Controller only (no game): `tools/gyroprobe` prints what SDL reads from the controller's sensors.
