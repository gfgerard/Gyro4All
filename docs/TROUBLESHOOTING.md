# Gyro4All: problems?

| Problem | What to do |
|---|---|
| Nothing happens and there is no `Gyro4All\Gyro4All.log` | Gyro4All isn't loading. Check it's next to the `-Win64-Shipping.exe` ([installation](../README.md#install), step 2), then try renaming `winmm.dll` to `version.dll`. |
| The camera doesn't move, but the log exists | Close DS4Windows/DSX/reWASD, plug the controller in by USB, check Steam Input. If a box at the top of the screen says **the game ignores the gyro**, the game needs a profile: see [Finding the camera of a new game](ADVANCED.md#finding-the-camera-of-a-new-game). |
| The gyro works but the game ignores the controller's buttons and sticks | The game only supports Xbox controllers. Look for `xinput:` lines in `Gyro4All.log`: they say why Gyro4All didn't show the controller as an Xbox one. If the reason is wrong, set `XInput=2` in `Gyro4All\Gyro4All.ini` ([details](../README.md#controllers-the-game-doesnt-support)). |
| Every button acts twice, or the button icons keep switching | The game reads the controller itself and also gets Gyro4All's Xbox controller. Set `XInput=0` in the game's profile or in `Gyro4All\Gyro4All.ini` (keeps the game's own DualSense support), or `XInput=1` if it is `2`, and report the game. |
| The camera slowly drifts on its own | Put the controller on a table for a few seconds in a menu, or use **"Recalibrate gyro"** in the panel. |
| The camera moves twice as much as it should | Steam Input (or another tool) is also turning the gyro into mouse movement. Turn that off. |
| `F10` does nothing | Try it from the pause menu. DirectX 11, 10 and 9 games: the panel is new there; check that `Gyro4All\gyro4all.dll` is installed and look for `panel:` lines in `Gyro4All.log`. Meanwhile, edit `Gyro4All\gyrolib.ini` instead. |
| The `F10` panel shows but ignores the mouse and keyboard | ReShade's overlay is open: while it is, ReShade blocks the mouse and keyboard for the whole game, Gyro4All's panel included. Use the controller in the panel (it doesn't go through ReShade), or close ReShade's overlay. |
| A box says **gyro disabled for this session** (old Visual C++ runtime) | The game ships its own old `MSVCP140*.dll` that would crash GyroLib, so Gyro4All left the gyro off and the game runs normally. Rename the files the box names (they're in the game's `Binaries\Win64` folder, e.g. add `.bak`) and restart. The `deps:` lines in `Gyro4All.log` give the details. |
| The game crashes | Uninstall Gyro4All and report it (below). |

**Reporting a problem:** open an issue with the game name and version, your controller and how it's connected
(USB / Bluetooth, Steam Input on or off), and attach `Gyro4All\Gyro4All.log` from the session where it went wrong.

For the meaning of the lines in `Gyro4All.log`, see the [log reference](ADVANCED.md#log-reference).
