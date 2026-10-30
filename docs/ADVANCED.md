# Gyro4All: advanced

This page is for the curious, for adding support for new games, and for developers. You don't need it to
play.

## Technical features

- **Drop-in install:** one proxy `winmm.dll` (or `version.dll`) plus a `Gyro4All` folder next to the game executable. No loader
  (UE4SS etc.) required.
- **Engine modules:** an engine-neutral core plus one module per engine, picked at startup (`engine:` in the log,
  `Engine=Unreal` / `Engine=RE` in `Gyro4All.ini` to force one). Unreal Engine 4/5 and, since 1.0, Capcom's RE Engine.
- **Signature-free engine access:** Unreal: GUObjectArray, FNamePool and the UStruct/FProperty layout are found at
  runtime by shape and verified against known engine classes, so no per-game byte patterns are needed.
  Works on monolithic and modular (one DLL per engine module) builds. RE Engine: the type database (TDB) is found
  by its header in the exe, and every class and field is looked up in it by name at runtime.
- **Generic camera path:** adds the gyro delta to `PlayerController.ControlRotation` in real degrees (float
  rotators on UE4, double rotators on UE5).
- **Per-game profiles** for games with custom cameras: vehicle views, camera components, or non-reflected
  C++ camera state reached by raw offset.
- **Xbox controller translation:** a controller the game can't read (DualSense, Switch Pro, DirectInput pads,
  without Steam Input) is shown to it as an XInput controller, from the same SDL device GyroLib reads. Automatic
  only when the game doesn't read pads itself (see [How it works](#how-it-works)).
- **Stick fallback:** when the game undoes the gyro's camera writes frame after frame (a scripted scene that rebuilds
  the camera from its own aim, such as RE3's restricted aim, or a camera Gyro4All doesn't know), the gyro is given to
  the game as right-stick input instead, through the controller paths Gyro4All already handles (the game's
  DirectInput controllers, the controller it shows as XInput or a real XInput one, the game's own PlayStation
  reports). Like Steam Input's gyro to joystick camera, each frame it gives the deflection that turns the camera at
  the gyro's speed, from the gyro alone: a still gyro is a centred stick, so the camera can't drift. To know which
  deflection that is, it learns the game's own stick by itself while it runs: its dead zone (axial or radial), response
  curve, top speed and sense (inverted look) on each axis, and how many frames late the game reads it, by comparing
  the deflection it gives with what the camera does (leaving out your own stick, limits such as looking straight up,
  and jumps). Before that (a few seconds of turning) it uses a default model. The learnt model is saved to the game's
  profile (`StickFallbackLearned=`), so the next session starts with it. Turns slower than the game's smallest stick
  speed go out as short pulses, so slow aiming isn't lost; turns faster than the game's top stick speed are capped
  there. Your own
  right stick keeps working. It switches back as soon as the game keeps the writes again, or on another camera. It
  needs no setting; games that read the controller through Steam Input or their own SDL can't get it. To test it in a
  game whose writes last, `StickFallbackTest=1` (`Gyro4All.ini` or the game's profile) forces it on in the first minute
  of play and every other minute after, with a notice at each switch.
- **Stick camera** (`StickCamera=1`, off by default): for games whose right-stick turn is too slow even at full
  deflection. Gyro4All turns the camera with the right stick itself, written in degrees like the gyro, so there is no
  speed cap, and the game reads a centred right stick. No mouse is emulated, so the game keeps its controller prompts
  and UI; the stick click (R3) still works. See [Stick camera](#stick-camera).
- **Sensor-time pacing** (`SensorPacing=1`, on by default): the controller's reports reach the PC at uneven intervals
  and are read once per frame, so a frame can get 8 or 12 ms of motion in a 10 ms frame: the camera turns unevenly at a
  steady hand, where the stick is smooth. Gyro4All plays the motion back in the controller's own time instead, so each
  frame turns by the motion of its own length, also with VRR and a changing frame rate. By default it plays the motion
  as far behind the frame as the last report usually is (`SensorPacingDelayUs=-1`, about 2 ms with a DualSense over
  USB), so the average latency is the same as without it, and carries the last rate forward when a report is late. It
  never turns against your motion: when it carried a turn too far, the difference is taken back while you keep turning.
  `SensorPacingDelayUs=0` plays up to the frame time itself (the last rate carried forward every frame), `SensorPacing=0`
  turns it off. Both apply when `Gyro4All.ini` or the game's profile is saved.
- **Gameplay gating:** the gyro stops in menus, pause, main menu, while the mouse cursor is shown, and when
  the game window loses focus.
- **Automatic calibration** in menus (GyroLib's "menus only" mode), including games whose gamepad menus
  show no cursor.
- **In-game settings panel** (GyroLib's DX12 overlay, `F10` by default): sensitivity, activation button or
  trigger, invert axes, smoothing and more, saved to `gyrolib.ini`. Edits to `gyrolib.ini` are also reloaded
  live while the game runs. Gyro4All declares `GL_HOST_MENU_STATE` to GyroLib, plus native right-stick
  suppression while it translates the controller to XInput or while the game reads a PlayStation controller itself
  through a checked report layout (`NativeFlickStick`), and long-press blocking (`GL_HOST_LONG_PRESS_BLOCKING`)
  wherever it can change the buttons the game reads (the controller shown as XInput, a real XInput controller, or
  the game's own PlayStation controller reports once their buttons were checked); no touchpad suppression (see
  [Current scope](../README.md#current-scope)). The recenter step callback (`gl_set_recenter_step_callback`) is
  registered from the start (so the main menu's panel shows it) and kept while the active view's rotation can be
  read (any view: the generic `ControlRotation` or a profile's `Property=`/`Offsets=`), and withdrawn in a view
  with `Recenter=0` or whose rotation can't be read. It moves the pitch to the view's `RecenterPitch=` (default 0) through the same
  writes as the gyro (all offsets, `WorldRotation`, `OnGyro=`). Custom cameras are best effort: their level may
  not be 0. The log says `recenter: available in view N` / `unavailable in view N (...)`.
- **Coexists with ReShade, RenoDX (HDR) and the Steam overlay.** Panel rendering runs on Gyro4All's own
  thread, so it keeps working when the engine moves `Present`/`ResizeBuffers` between threads.
- **Diagnostics for new games:** a status line every 30 s in `Gyro4All.log`, plus an optional probe (`F7`
  snapshot / `F8` diff) to find which rotation a game's camera actually uses.

### Stick camera

Set in `Gyro4All.ini` or in a game's profile (`[Gyro4All]` section, overrides `Gyro4All.ini`):

| Setting | Default | Meaning |
|---|---|---|
| `StickCamera` | 0 | 1 = the right stick turns the camera through Gyro4All (restart the game after changing it) |
| `StickSpeed` | 360 | Yaw at full deflection, degrees/s |
| `StickSpeedPitch` | 200 | Pitch at full deflection, degrees/s |
| `StickDeadzone` | 10 | Radial deadzone, % |
| `StickCurve` | 2.0 | Response exponent: 1 = linear, higher = finer near the centre |
| `StickEdge` | 90 | Deflection (%) past which the speed ramps up |
| `StickEdgeSpeed` | 720 | Yaw after the ramp, degrees/s (pitch scaled alike) |
| `StickEdgeRamp` | 400 | Time to reach `StickEdgeSpeed` while held past `StickEdge`, ms |
| `StickInvertY` | 0 | 1 = stick up turns the camera down |
| `StickCameraPassButtons` | (none) | Comma-separated buttons (same names as the sensitivity profiles: `RB`, `LT`, `R1`...). Held over 200 ms, they give the right stick back to the game until released, e.g. `RB` for a weapon wheel picked with the right stick |

Changes to the `Stick*` values apply when the file is saved, without a restart. Diagonals turn no faster than the axes.

When it turns:
- Only in gameplay. Menus, the mouse cursor, pause, the F10 panel and an unfocused window leave the stick to the game.
- Not while GyroLib's flick stick uses the right stick: flick stick keeps priority.
- Not while the stick fallback runs.
- Only where Gyro4All can hide the stick from the game:
  - the controller shown as XInput;
  - the game's own PlayStation support, once Gyro4All has checked the report layout (move the right stick once in
    game).

  It can't hide the stick with Steam Input or a real Xbox controller, so the stick camera stays off there.

Limits:
- The game no longer sees the right stick, so its aim assist, aim slowdown and invert-Y setting don't apply to it.
- A weapon wheel on the right stick that shows no cursor would turn the camera: name its button in
  `StickCameraPassButtons`, or set `StickCamera=0` in that game's profile.

## Compatibility details

| Game | Engine | Profile needed| Notes |
|---|---|---|---|
| Pacific Drive | UE 4.27 | yes | On foot uses the generic path. In the car the gyro drives the car camera and overrides the auto-recentering while driving. |
| Returnal | UE 4.25 (modular build) | no | Generic path working OOB. A good setup is gyro on while L2/R2 is held |
| Cronos: The New Dawn | UE 5.4 | yes | The camera ignores `ControlRotation`; the profile writes the look rotation inside the pawn's `View` component. Panel verified with HDR + RenoDX. |
| Gears of War: E-Day | UE 5.6 (Xbox / Game Pass build) | no | Generic path, campaign tested *WITH EAC DISABLED*. Install in `Binaries\WinGDK` renamed to `version.dll` (with RivaTuner running, the game loads the system `winmm.dll` first). Protected exe: Gyro4All switches to inline hooks and finds the engine objects with a heap scan, so the gyro becomes available a few seconds after the main menu appears. The game ships EasyAntiCheat: don't use Gyro4All in online modes. |
| Life is Strange Remastered | UE 4 (pre-4.25 property system), DX11 | yes (shipped) | Found with the camera probe. The camera splits pitch and yaw across two raw rotators, and the walking camera blends back to an automatic follow camera unless it gets look input; the profile writes `OnGyro=` to keep it manual. No settings panel (DX11): configure via `gyrolib.ini`. |
| Luto | UE 5.5.4 | no | Generic path working OOB. Only reads XInput: Gyro4All shows the DualSense / Switch Pro as an Xbox controller (Epic, no Steam Input). |
| The Outer Worlds: Spacer's Choice Edition | UE 4 | no | Generic path. Native DualSense (delay-loaded `libScePad.dll`): XInput translation stays off. |
| Mortal Shell | UE 4.25+, DX11 | yes (shipped) | Generic camera path. The game links an old Sony pad library (`WinDualShock`) that knows only the DualShock 4: the profile sets `DualSenseAsDualShock4=1` (PlayStation icons, rumble, light bar). Other controllers (Switch Pro, DirectInput) are shown as Xbox ones; the Epic Online Services overlay rewrites the game's XInput import after Gyro4All, which takes it back. Tested with a DualSense over USB and Bluetooth (buttons, sticks, rumble, light bar, native flick stick). No settings panel (DX11). |
| Warhammer 40,000: Space Marine 2 | Swarm Engine, DX12 | no | Swarm module: the third-person camera controller's pitch/yaw, found by a heap scan. Tested solo with EAC disabled (DualSense over USB, the game's own DualSense support). Rename the game's old `msvcp140.dll`. |
| Hogwarts Legacy | UE 4 | yes | Third person detached camera, new objects detected. Native DualSense (delay-loaded `libScePad.dll`): XInput translation stays off. |
| Visage | UE 4  | no | First person camera XInput translation/Steam input needed. |
| Valor Mortis | UE 5.7 | no |  |
| PAYDAY 3 | UE 5 | yes (shipped) | Generic path in normal play; during some actions the camera is turned by a camera modifier (`SBZClampCameraRotationModifier`, raw +0x68), which the profile adds as a `Match=Camera` view: Gyro4All switches to it and back by itself. Case-preserving names build (`WITH_CASE_PRESERVING_NAME`): Gyro4All detects the different name pool layout by itself. Native DualSense (HID): XInput translation stays off. Always-online co-op game: use at your own risk, private or solo lobbies only. Tested with GyroUE 0.9.1 (DualSense), same engine code; not yet with Gyro4All. |
| Resident Evil 7 biohazard | RE Engine (TDB 70, the 2022+ DX12 build) | yes (shipped) | The gyro turns the free-look camera controller (`app.PlayerCamera`, `MaximumOperatable`); scripted cameras, animations and pause turn it off. The game only reads XInput (it takes a real DualShock 4 found through DirectInput, but not a DualSense, nor a DualSense shown as a DualShock 4): the profile sets `XInput=2`. Tested with a DualSense, Steam Input off. |

Tested with a DualSense over USB/BT and Nintendo Switch Pro Controller. Requirements and limits:

- Windows 10/11 x64, DirectX 12 for the settings panel. Without a DX12 swapchain the gyro still works; only
  the panel is unavailable.
- **Single-player / offline games without anti-cheat only.** Do not use Gyro4All with EasyAntiCheat,
  BattlEye or any online-protected game.
- UE 4 and UE 5. Both property systems are detected at runtime: FField (UE 4.25+, UE 5) and the older UProperty
  objects (tested with Life is Strange Remastered).
- RE Engine: type database versions 69 and 70 (Resident Evil 7's current build; RE2/RE3's ray-tracing builds and
  Village use the same versions but are untested). The camera part knows Resident Evil 7's player camera only.
- Gyro4All ships as `winmm.dll`, which more games import than `version.dll`. If the game already has a `winmm.dll`
  proxy from another mod, or Gyro4All doesn't load (no log), rename it to `version.dll`: the same DLL works under both
  names. The installer refuses to overwrite a foreign proxy.
- Close other pad remappers (DS4Windows, DSX, reWASD) while playing. If Steam Input is enabled for the game,
  check that Steam isn't also converting the gyro to mouse, or the motion will be doubled.

## Log reference

All diagnostics go to `Gyro4All\Gyro4All.log` (probe results, if enabled, to `Gyro4All\Gyro4All-probe.log`).

| Symptom | Check |
|---|---|
| No log file | `winmm.dll` is not next to the game exe (it must be in `Binaries\Win64` or `Binaries\WinGDK`), another mod's `winmm.dll` replaced it, or the system `winmm.dll` was loaded first (e.g. by RivaTuner's hooks, seen in Gears of War: E-Day): rename it to `version.dll`. |
| "Entry point `timeGetDevCaps` not found" | A GyroUE older than 0.4.7 named `winmm.dll`. Update: 0.4.7+ exports winmm too. |
| `motion=0` in the status line | The controller isn't visible to SDL: close remappers, try USB, check Steam Input. |
| `view=none` in gameplay | The pawn is missing or a placeholder. The game may need a profile. |
| `settings: GyroUE folder found (former name): ...` | First start after updating from GyroUE: its `GyroUE.ini` (as `Gyro4All.ini`), `gyrolib.ini` and profiles were copied into `Gyro4All\` (never over existing files) and the old folder renamed to `GyroUE.migrated`. |
| `engine: none detected, using Unreal` | The game's exe and folder don't look like an Unreal game (renamed exe outside `Binaries\Win64`). Unreal is used anyway; `Engine=Unreal` in the `[Gyro4All]` section of `Gyro4All\Gyro4All.ini` (or the game's profile) sets it explicitly. |
| `applied yaw` changes but the camera doesn't move | Custom camera: see [Finding the camera](#finding-the-camera-of-a-new-game). |
| `camera: '<view>' is rewritten by the game` (or `is ignored by the camera`), `trying '<next>' (N degrees carried over)` / `the camera follows view '<view>' now (Match=Camera)` | The game switched between rotations of the same view target and Gyro4All followed it (see [Several cameras for the same view target](#several-cameras-for-the-same-view-target-matchcamera)). |
| `probe: started during play (...)` / `probe: camera learned: ...` / `probe: stopped (...)` | A probe started from the probe key or by AutoProbe, and what became of it (see [Learning a camera during play](#learning-a-camera-during-play-no-restart)). |
| `update rotation: <class> UpdateRotation is vtable[N] at <module>+0x... (reads RotationInput +0x... first): hooked, the gyro goes into RotationInput` | A `Property=RotationInput` view: the gyro is added to the stick's look input inside the controller's `UpdateRotation` (see [The stick's look input](#the-sticks-look-input-propertyrotationinput)). `N functions of its vtable read RotationInput first ... not found` / `not unique`: the controller's `UpdateRotation` couldn't be told apart, so that view doesn't turn (report it with the log). |
| `scene: the pawn is in a cutscene (IsInActiveCutscene): gyro paused` / `the pawn moves in MOVE_Flying (a scripted scene): gyro paused` / `scripted scene over` | Unreal: scripted moments count as a pause, so the gyro stops as the stick does: a pawn with a reflected `IsInActiveCutscene` set (Dead Island 2), or the pawn's `CharacterMovement` in flying mode (games move the character along a scripted path with it). |
| `profile: the profile file changed, views read again` | The profile was edited, or the probe added a view: its views apply at once. |
| The gyro stops mid-game, `gyro on` still logged | Look at the lines just before: `focus: lost to <program> (window class ...)` (another window is in front), `gyro: GyroLib suspends the gyro` (activation mode, focus, pause, menus or calibration), `gyro: motion data lost`, `gyro: GyroLib's sample count restarted` (the controller was reopened) or `gyro: the controller reads as lying still` while you hold it (no real gyro data). If `applied yaw` keeps changing, `camera: stopped following the gyro (...) - target ..., camera manager ..., CameraStyle ...` names the camera that took over. |
| Camera drifts slowly | Rest the pad in a menu for a few seconds (`bias=` in the status line should become non-zero) or use "Recalibrate gyro". |
| Panel doesn't open | Look for `overlay:` lines (DX11/DX10/DX9 games: also `panel:` lines, e.g. `panel: captured D3D11 swapchain ...`, `panel: D3D11 renderer ready ...`, `panel: no frame presented for N ms: panel closed`). Try from the game's pause menu. As a fallback, edit `gyrolib.ini` directly. |
| Double motion | Steam Input or another tool is also converting the gyro to mouse. |
| `ue: core classes not found` (and a notice after 60 s) | Gyro4All can't read this game's engine data, so nothing can turn the camera and the probe can't help. Report the game with the log: the `ue: core classes missing:` line says what was found instead. |
| `xinput: ... shown as XInput controller N` | The controller is translated to an Xbox controller in slot N. |
| `xinput: ... not shown: <reason>` | Why it isn't: an Xbox/Steam Input/remapper controller is connected, or the game reads pads itself (Sony's pad library, its own SDL, Raw Input for gamepads, the controller's HID device, a DirectInput game controller it reads). `XInput=2` overrides the last group. |
| `nativepad: the game reads 054C:0CE6 (...): N bytes, first byte 0x31, report 0x31 (DualSense, Bluetooth)` | The game reads the controller itself (Sony's pad library). `unknown layout, left alone` means a report Gyro4All doesn't know: no native flick stick for it. |
| `nativepad: ... matches SDL` / `native right-stick suppression available` | The right stick in the game's reports agreed with SDL while you moved it: GyroLib's flick options are offered. `doesn't match SDL` or `CRC isn't the expected one`: Gyro4All leaves the reports alone (report it with the log). |
| `dualsense: the game's own PlayStation support closed the DualSense unread ...` | The game's Sony library only knows the DualShock 4. `DualSenseAsDualShock4=1` (and `DualSenseAsDualShock4Learned=1`, which says Gyro4All wrote it) was written to its profile (`Gyro4All\profiles\<exe>.ini`, created if needed); the DualSense is an Xbox controller until the game restarts. Not done when the profile already has a `DualSenseAsDualShock4` value. |
| `dualsense: the game rejected the DualSense shown as a DualShock 4 too ...` | The DualShock 4 identity Gyro4All had learned didn't work either (the game opened the controller and closed it unread): the game doesn't use PlayStation controllers this way (Resident Evil 7 only takes a real DualShock 4 it finds through DirectInput). The DualSense is an Xbox controller from then on, and `DualSenseAsDualShock4=0` is written to the profile so it isn't learned again. |
| `nativepad: DualSense 054C:0CE6 (version ...) shown to the game as a DualShock 4` | `DualSenseAsDualShock4=1` in the game's profile: the game asked who the controller is and got a DualShock 4. Then `feature report 0x.. asked by the game: answered with ...` lists what it checked, and `output report 0x05 -> DualSense report 0x02/0x31` its rumble and light bar. `opened ... Bluetooth, shown to it as a USB DualShock 4` and `Bluetooth, ... reads, converted` for a Bluetooth DualSense. If the game opens the controller and closes it for good right after the identity line, it rejected the emulation: when Gyro4All had learned it, it takes it back by itself (`dualsense: the game rejected ...`); when you or a shipped profile set it, report it with the log. |
| `nativepad: the game's controller reports made idle (GyroLib's panel has the controller)` | While the settings panel is open (and until you release the button that closed it), the game reads an idle controller, so navigating the panel doesn't also move you in the game. `passed through again` when that ends. The controller shown as XInput, and real XInput controllers, read idle too. |
| `long-press blocking available (...)` | GyroLib's *Block long press* works for your controller: with Hold or Hold to disable on a single button, a short press of that button still does its game action (when you release it), a hold only controls the gyro. For a controller the game reads itself, it becomes available after `nativepad: ... the buttons in the game's reports match SDL`, i.e. after you've pressed a few buttons in game. `unavailable` when the game reads the controller in a way Gyro4All can't change (Raw Input, DirectInput). |
| `longpress: button N tapped` / `held` | Long-press blocking at work (first few logged): `tapped` = a short press passed to the game, `held` = a hold whose game action was blocked. N is SDL's button number (0 south/Cross, 1 east/Circle, 2 west/Square, 3 north/Triangle...). |
| `nativepad: right stick centred in the game's reports` | Flick stick is active: the game sees a centred right stick (`passed through` when it stops, e.g. in menus). |
| `xinput: XInput=2: the game's access to HID device 054C:0CE6 refused` | With `XInput=2` the game tried to open the controller itself (its own DualSense support) and was refused, so it only gets the Xbox controller. `Raw Input registration ... dropped` is the same for Raw Input. |
| `hooks: the import of ... was written back by something else (now <module>)` | Another module (an overlay such as Epic Online Services' `EOSOVH-Win64-Shipping.dll`) rewrote a game import Gyro4All had patched. XInput imports are `patched again` so the translated controller keeps reaching the game; others are `left to it`. |
| `gyro: Steam Input reader started (the game's steam_api64.dll)` | The game uses Steam: GyroLib also reads the controller's gyro through the game's own Steam Input (it borrows the game's Steam service and changes nothing in it). This is how the gyro works when Steam Input hides the controller and the game reads Steam Input itself (Monster Hunter Wilds). `Steam Input reader: unavailable` is normal while Steam Input has no controller. |
| `stick fallback: the game undid the gyro's camera writes 8 frames in a row: gyro as right-stick input; game's stick: ...` (or `forced on (StickFallbackTest)`) / `stick fallback off: <why>` | The game kept putting the camera back after each gyro write (a scripted aim, or a camera rebuilt from other state), so the gyro went to the game as right-stick input until `<why>`: the game keeps the writes again, another camera, or no readable view. `the stick turns nothing` means the game doesn't read the controller through a path Gyro4All can change (Steam Input, its own SDL): no fallback for that camera. `game's stick: radial dead zone 0.20, curve 1.50, yaw 160 and pitch 110 deg/s at full stick, 2 frames late (default, to learn)` is the model of the game's stick it starts with (`(saved)`: from `StickFallbackLearned=`, `(learnt)`: this session). `stick fallback: the game's stick, learnt: ... (the model explains N% of the camera's motion)`: the model learnt from the camera (again when it changes); `(inverted)` after an axis the game turns the other way. `stick fallback: StickFallbackLearned=... saved to <profile>`: kept for the next session. While it runs, a `status: stick fallback: stick (x, y), game's stick: ... (learnt, explains N% / saved for this game, learning / default, learning)` line follows the status line. With `MotionTrace=1`, `K,x,y,yaw,pitch,used` lines in `motion-trace.csv` give each frame's deflection, the camera's turn and which axes the learner took (with `StickFallbackTest=1` the trace starts with the fallback). `stick fallback test: forced on / detection only for a minute`: `StickFallbackTest=1`. |
| `settings: SensorPacing=1 (...): motion played back in sensor time, delay automatic` (or `delay N ms`, `SensorPacing=0`) | Sensor-time pacing's settings, at start and when `Gyro4All.ini` or the profile is saved with a change. Every 30 s while the gyro turns: `status: sensor pacing: delay D ms (automatic), N frames paced, P% past the last report (extrapolated), B left to GyroLib's burst delivery, S stalls`: D is how far behind each frame the motion is played; P the frames whose time came after the last report read (its rate carried forward); B frames turned as GyroLib gave them while it spreads a Bluetooth pad's bursts itself; S game stalls (a frame over 100 ms), whose turn goes out at once. In `motion-trace.csv` (`MotionTrace=1`) the F lines' raw yaw/pitch are GyroLib's, the last two the paced turn. |
| `settings: StickCamera=1 (...): yaw ... deg/s, pitch ...` | The stick camera's settings, at start and each time `Gyro4All.ini` or the profile is saved. |
| `stick camera: on (the controller shown as XInput)` / `on (the game's own controller reports)` | The right stick turns the camera through Gyro4All; the game reads it centred. |
| `stick camera: off while GyroLib's flick stick uses the right stick` / `off while the stick fallback turns the camera` | Flick stick (priority) or the stick fallback has the right stick. |
| `stick camera: unavailable: Gyro4All can't hide the right stick from the game here (...)` | Steam Input or a real Xbox controller, or the game's own controller reports aren't checked yet (move the right stick). The game keeps its own stick. A `status: stick camera <state>` line follows the status line every 30 s. |
| `dinputpad: stick fallback on the game's right stick (N-byte state, Z ..., Rz ...)` | The first time the fallback moves the right stick of a DirectInput controller the game reads, with the axis ranges the game set. |
| `dinputpad: the game opens a DirectInput game controller (VID:PID, type ...)` | The game created a DirectInput device for a controller. It only counts as the game reading controllers itself once it reads it (`xinput: the game reads controllers itself: it reads a DirectInput game controller ...`). |
| `xinput: the game imports the HID library but hasn't opened a PlayStation controller in 30 s` | The game imports `hid.dll` (usually a sign of Sony's pad library) but never opened the controller: it is translated. If the game opens a PlayStation controller later, the translation stops (`its own PlayStation support is back`). |
| `xinput trace: ...` | `XInputTrace=1` (diagnostics): every module that calls XInput, and whether the game loads Windows.Gaming.Input, GameInput or DirectInput. |
| `writewatch: armed on 0x... in N threads`, `writewatch: writer <module>+0x... (thread ...)` | Diagnostics for finding a camera's source, off by default: with `WriteWatch=on` in `Gyro4All.ini` when the game starts, Gyro4All re-reads `WriteWatch` every 500 ms; set it to a hex address (e.g. a camera value found with an external memory tool) and a hardware write breakpoint watches those 4 bytes for `WriteWatchSeconds` (10). Each instruction that writes there is logged once with its registers, its callers (stack unwind, `caller #N`, with their non-volatile registers) and code bytes, then a summary with how often each wrote. `WriteWatch=x:<address>` watches a function's entry instead (an execute breakpoint) and logs the objects it ran on (`rcx`) and, every 200 ms, the floats at `WriteWatchFields=<hex offset>,...` from that object. `WriteWatch=r:<address>` logs the instructions that read or write it (x86 has no read-only breakpoint), to follow an input value forward into the game's code. Instead of a hex address, `di+<hex offset>` is the buffer of the game's last DirectInput `GetDeviceState` (`r:di+8`: the right stick's X of a PlayStation controller). Engine modules add names of their own that follow the object when the game replaces it (Frostbite: `cam`, the active camera's rotation interface; `writewatch: cam+2C4 moved to 0x...`). The game slows down while armed and the watch disarms itself after 100,000 hits (`too many hits`); remove the setting for playing. |
| `dinputpad: the game reads controller VID:PID with GetDeviceState (N bytes, ...) into 0x... (...), called from <module>+0x... (thread ...)` | Each place the game reads a DirectInput controller from, once: the call site in the game's code, the buffer (static, heap or `on the caller's stack`) and its data format. The last `GetDeviceState` buffer is WriteWatch's `di` address. |
| `xinput: WMI: the game reads controller VID:PID's DeviceID: shown as an XInput one` | The game checks which DirectInput controllers are XInput ones through WMI (`IsXInputDevice`); that controller now passes, and the game should poll it (`the game polls XInputGetState`). |
| `xinput: XInputGetState hook registered, no game module imports it yet` | The game hasn't loaded its XInput code yet, or uses another input API (then there is nothing to translate). |
| `engine: RE (detected)` | `re_chunk_000.pak` next to the exe: the RE Engine module is used. |
| `re: TDB 70 ready`, `re: via.Application ...`, `re: gyro frame hooked before LateUpdateBehavior` | RE Engine start-up: the type database, the engine's application object and its per-frame steps (all listed once, `name(priority)`) were found, and the gyro runs once per frame before that step. `re: ... the gyro can't run in this game` says which part failed. |
| `re: camera <controller> (type N[, animation][, paused])` | RE Engine: the player camera's current controller. `MaximumOperatable (type 0)` is free look, where the gyro turns the camera; anything else (`FullAnimation`, `ElasticRotation`...) is a scripted camera and the gyro stays off. `re: camera no player` in menus and loading. |
| `deps:` lines | Before loading GyroLib, Gyro4All checks which copy of each DLL GyroLib shares with the game (Visual C++ runtime, SDL3): one already loaded by the game, or one next to its exe, is the one GyroLib gets. `older than 14.40, GyroLib ... would crash the game` turns the gyro off for the session (the game still starts) and shows the fix: rename the game's copies so Windows' newer ones are used. `older than GyroLib's build tools` and `the game's own SDL3` are warnings only. |

## How it works

```text
DualSense --HID--> SDL3 (inside gyrolib.dll)
          --> GyroLib: calibration, fusion, gyro space, smoothing, curves, activation, views
          --> gl_update() -> yaw/pitch delta in degrees
          --> Gyro4All, on the game thread once per frame:
                scene   <- GEngine -> GameViewport -> World -> LocalPlayer -> PlayerController,
                           Pawn, PlayerCameraManager view target, WorldSettings pauser
                gating  <- focus, pause, cursor, "no gameplay view" (main menu DefaultPawn)
                apply   -> profile view rotation += delta  (default: ControlRotation)
```

- `winmm.dll` is a proxy for the system DLL of the same name. It also exports all of `version.dll`, so it can be
  renamed to `version.dll`; either way every export is forwarded to the System32 DLL with its own file name, so the
  game behaves normally. 
- The per-frame hook is an import-table patch of `PeekMessageW`. Unreal pumps window messages on the game
  thread once per frame, which is a safe place to read and write game objects. The game's own code is never
  patched. Exception: protected exes whose import table is only a stub filled in later by the protector
  (e.g. Gears of War: E-Day). There Gyro4All hooks `user32!PeekMessageW` inline with
  [MinHook](https://github.com/TsudaKageyu/minhook), and finds the swapchain through the vtable of the real
  System32 DXGI factory (so it doesn't fight Streamline, ReShade or RivaTuner over the `dxgi.dll` exports). The log says
  which mode is used (`hooks: ... import-table hooks` / `inline hooks on exports`); `GYRO4ALL_INLINE=1` in the
  environment forces inline mode.
- Xbox controller translation: the game's imports of `XInputGetState`, `XInputSetState`, `XInputGetCapabilities`,
  `XInputEnable` (by name or ordinal, including delay-loaded ones: UE 5 delay-loads `XINPUT1_4.dll`) and
  `XInputGetStateEx` are patched in the game modules only, so SDL inside GyroLib never sees the virtual controller.
  Once per frame Gyro4All copies the SDL state of GyroLib's selected controller into the slot it answers for (the
  first one with no real controller), and turns `XInputSetState` into SDL rumble. In auto mode it waits 2 s after
  the controller appears, and stays off if a real XInput controller is connected or the game shows signs of reading
  pads itself. Sony's pad library (`libScePad*.dll` or `hid.dll` imported, also delay-load, or `libScePad` loaded)
  only counts for a PlayStation controller: other controllers are still translated there. `hid.dll` imported alone
  counts for 30 s after the controller appears, and after that only once the game has opened a PlayStation HID device
  (Monster Hunter Wilds imports it and never does). For any controller: an SDL of its own, `RegisterRawInputDevices`
  for joysticks/gamepads, the game reading a DirectInput game controller (`GetDeviceState`/`GetDeviceData` on a
  joystick/gamepad device; creating DirectInput or its devices doesn't count: RE Engine games do that only to identify
  controllers), or `CreateFileW` on the controller's HID device (only without a Sony library, whose enumeration opens
  every HID device). When the controller appears or goes, it
  posts `WM_DEVICECHANGE` to the game's `UnrealWindow`s, because Unreal only checks empty XInput slots after that
  message. With `XInput=2` the game's `CreateFileW` on PlayStation/Nintendo HID devices (and the selected
  controller's) fails with access denied, also inside a separate `libScePad.dll` (whose import table is patched for
  that hook only), and its Raw Input registrations for joysticks/gamepads are dropped: Sony's pad library and
  direct HID code then see no controller, as when another program holds it exclusively. Not available on
  protected exes (inline-hook mode).
- Games that make XInput devices only of the DirectInput controllers WMI calls XInput (Microsoft's `IsXInputDevice`
  sample: Battlefield 4, 007 First Light) would never poll the translated controller: once the game creates a WMI
  locator, Gyro4All hooks WMI's in-process property reader (`IWbemClassObject::Get`) and shows the `DeviceID` of every HID game
  controller (joystick, gamepad, multi-axis) as an XInput one (`HID\VID_xxxx&PID_xxxx&IG_00\...`), so the game's own
  check takes it as an Xbox controller, whatever the engine. PlayStation controllers are left out when the game reads
  them itself (auto mode). A controller listed so reads as connected to `XInputGetCapabilities` in the slot it will
  get until it is shown (some games pick the slot once).
- Flick stick with the game's own PlayStation controller support (`NativeFlickStick=1`, `nativepad.cpp`): Sony's pad
  library (as `libScePad.dll` or linked into the exe) opens the controller's HID device with `CreateFileW` and reads
  input reports with `ReadFile` / `GetOverlappedResult(Ex)`. Gyro4All follows the Sony handles the game opens for
  reading (`CloseHandle` ends that) and, when a read completes, notes the right stick in every report it returned
  (one read can hold several, of the device's `InputReportByteLength`): report `0x01` (USB, bytes
  3-4), `0x31` (DualSense Bluetooth, bytes 4-5) or `0x11` (DualShock 4 Bluetooth, bytes 5-6); Bluetooth reports
  must carry the expected CRC32 (0xA1 + report). Once that stick agrees with SDL's while it is held away from the
  centre, Gyro4All declares native stick suppression to GyroLib; while GyroLib then asks for it, the stick bytes in the
  game's copy are set to 128 (and the CRC recomputed). The controller and GyroLib's own SDL reading are untouched.
- DualSense as a DualShock 4 (`nativepad.cpp`), for games whose Sony library predates the DualSense. Detection: the
  game's Sony library opens every controller it finds; one it supports stays open and is read at once, one it doesn't
  is closed unread. A DualSense opened, closed unread and not open 5 s later (with a Sony library among the game's
  imports) makes Gyro4All write `[Gyro4All] DualSenseAsDualShock4=1` into the game's profile and translate the DualSense to
  XInput for that session: the library only looks for controllers when the game starts (not on `WM_DEVICECHANGE`, tested
  in Mortal Shell), so the new identity can only work from the next start. It also writes `DualSenseAsDualShock4Learned=1`,
  and nothing when the profile already has a `DualSenseAsDualShock4` value. If on that next start the game opens the
  DualSense shown as a DualShock 4 and closes it unread too (5 s, none open), the learned identity is taken back: the
  DualSense is translated to XInput at once, whatever else the game reads controllers with (Resident Evil 7 also creates
  DirectInput), and the profile gets `DualSenseAsDualShock4=0` (the `Learned` key removed). A value the user or a
  shipped profile set is never changed. A profile can also ship the key. With it,
  on the game's own handles to a DualSense: on the game's own handles to a DualSense, `HidD_GetAttributes` reports a USB DualShock 4 (054C:09CC,
  version 0x0100), `HidP_GetCaps` / `HidP_GetValueCaps` its report sizes and feature reports, and `HidD_GetFeature`
  answers the DualShock 4's calibration (from the DualSense's), address and firmware reports; the game's feature
  writes are not passed on. Input reports are rewritten as DualShock 4 USB reports (over Bluetooth Gyro4All reads the
  longer reports itself and hands the game converted ones), and the game's DualShock 4 output report (rumble, light
  bar) is sent as the DualSense's (with its CRC over Bluetooth).
- RE Engine (`src/engines/reEngine/`): the main thread only waits for window messages there, so there is no
  `PeekMessageW` frame. The module finds the type database (TDB) in the exe, `via.Application` through its runtime
  type, and that object's table of per-frame steps (module entries); it swaps one entry's function pointer (a data
  write, no code patched) so the gyro runs just before that step, on the game's update thread. `FrameEntry=` in
  `Gyro4All.ini` picks the step (default `LateUpdateBehavior`, after the game has applied the stick). The scene:
  `app.ObjectManager` (found among the exe's static singleton slots) -> `PlayerObj` -> its `app.PlayerCamera`
  component; the gyro rotates the free-look controller's `<rotation>` quaternion (yaw about the world's up axis,
  pitch about the camera's own horizontal axis, within the controller's `XRotateLowerLimit`/`XRotateUpperLimit`), which
  the game turns into the camera every frame. Field offsets come from the TDB by name. Layouts follow
  [REFramework](https://github.com/praydog/REFramework)'s SDK (MIT). Games with Capcom's anti-tamper (Resident Evil
  Requiem, Pragmata) also need REFramework installed, to switch the anti-tamper off; RE7 doesn't.
  `tools/reEngine/tdbdump.py` lists a game's types and field offsets from its exe, offline; `tools/reEngine/README.md`
  explains how to find a new game's camera.
- Swarm Engine (`src/engines/swarm/`, Saber Interactive: Warhammer 40,000: Space Marine 2): the game window is pumped
  by a thread of its own (the startup thread only pumps the splash screen), which becomes the game thread. The view's
  angles are the third-person camera controller's `float pitch, yaw` in degrees; the module finds the controller's
  vtable through the code of its constructor and the controller itself with a heap scan on a thread of its own (again
  after loading screens). The pitch stops at the on-foot camera's limits (70 up, 75 down), or wherever the game's own
  stick took it further (aiming). The camera rig, render camera and view matrices are copies rebuilt from those
  angles every frame. The game opens the DualSense through both `CreateFileW` and `CreateFileA`.
- Every memory read through reflection is guarded, so a missing object or property disables the feature
  instead of crashing the game.
- The settings panel hooks DXGI factory and swapchain vtables (`Present`, `ResizeBuffers`) and subclasses
  the game window to forward input. These hooks are only installed if the panel is enabled in
  `gyrolib.ini`.
- DirectX 11, 10 and 9 games (GyroLib's DX12 panel can't draw there) get GyroLib's static ImGui panel, the same widgets
  and settings, from `Gyro4All\gyro4all.dll`. `winmm.dll` loads it only once such a game has made its swapchain (D3D10/
  D3D11) or device (D3D9): DX12 games never load it. The panel's frame is built on the game thread after `gl_update`
  (GyroLib's context has one owner thread) and drawn just before the game's `Present`: D3D11 inside a device context
  state of its own (the game's whole pipeline state is swapped out and back), D3D10 with a state block, D3D9 with a state
  block plus its render targets. On an HDR swapchain (scRGB or HDR10, D3D11) the panel is composed in linear light with
  the UI white at 203 nits, as GyroLib's DX12 panel does. Under wrappers that hand the game proxy objects (ReShade as
  `dxgi.dll`, `d3d11.dll` or `d3d9.dll`) the panel draws on the real swapchain or device inside them, after their effects
  and overlay (`overlay: swapchain ... not used: the panel draws on the real swapchain inside it`). Its own ImGui exports
  nothing, writes no `imgui.ini` and never changes the mouse cursor. It navigates with GyroLib's selected controller,
  like the DX12 panel; Escape closes it. Not available there yet: the Steam Input mouse option, which needs GyroLib's DX12
  window bridge. Set `ui.menu_key=` (empty) and `ui.gamepad_menu_shortcut=0` to run with no graphics hooks
  at all.
- The panel finds the game's swapchain through the game's own `dxgi.dll` factory imports (protected exes: the real
  System32 factory). Some protectors resolve the graphics imports themselves, so those imports are never called; for
  games that ship NVIDIA Streamline (DLSS), Gyro4All also hooks Streamline's proxy factory (`sl.interposer.dll`: its
  `CreateDXGIFactory*` exports and `slUpgradeInterface`) as soon as it loads, and uses it when the game makes its
  factory there without calling its own import (`overlay: the game makes its DXGI factory through Streamline ...`).
  The panel then sits above frame generation, as with the game's own factory; games whose import is called first stay
  on it. When Streamline's queue and swapchain report different devices (e.g. ReShade under Streamline), the panel
  renders with the queue under Streamline's proxy (`overlay: queue and swapchain devices differ: ...`), and it never
  keeps the game's swapchain alive: when the game releases it (e.g. toggling frame generation), the panel lets go at
  the same moment (`overlay: the game released its swapchain: released swapchain ...`) and attaches to the new one.
  Verified in RE Requiem with DLSS frame generation on and off and toggled both ways, with ReShade and REFramework.

## Game profiles

The generic path needs no profile. When a game's camera ignores `ControlRotation`, add
`Gyro4All\profiles\<exe name without .exe>.ini`. Profiles shipped in this repo live in [`profiles/`](../profiles).
Profiles and settings files written for GyroUE (the former name) still work: keys in their `[GyroUE]` section are
still read, and the same key in a `[Gyro4All]` section wins.

```ini
[Gyro4All]
; Extra views, in priority order (the default view always exists).
Views=InCar
; Diagnostics: F7 = snapshot, F8 = log what changed (see below).
Probe=0

[View.Default]
Label=Exploration
; Optional: another object and rotation for the default view.
;   Object=Root.Sub...   Root = PC | Pawn | Camera | ViewTarget; each Sub is an object property name,
;                        the name of a subobject (e.g. a component) of the previous object, or @Class:
;                        its first subobject of that class (for names that change every session)
;   Property=Name        a reflected FRotator property on that object; Property=RotationInput (PC): the stick's
;                        look input, see "The stick's look input (Property=RotationInput)"
;   Offsets=0x2A0,0x420  raw FRotator offsets instead (non-reflected C++ state); all get the delta
;   RotatorSize=24       24 = double (UE5), 12 = float (UE4)
;   PitchView=1          this view turns yaw only; its pitch goes into view 1's rotation (a camera that keeps
;                        yaw and pitch in two objects; the probe writes it for a camera that only followed yaw)
;   WorldRotation=1      with Property=RelativeRotation of a scene component that uses absolute rotation: also
;                        writes the component's world rotation (see "Components with absolute rotation")
;   RecenterPitch=0      level pitch for the recenter button, in this view's rotator (degrees, default 0);
;                        set it when "level" isn't 0 in a custom camera
;   Recenter=0           hide the recenter button in this view (it doesn't level this camera properly)

[View.InCar]
Id=2
Label=In car
; Active while the camera's view target is (or derives from) this class.
ViewTargetClass=BP_PlayerCarNew_C
; Rotator property on the view target (or use Object/Offsets as above).
Property=CameraControllerRotation_Target
; Settings inherit from view 1 until changed for this view in the panel.
Parent=1
; Scalar writes on the view object whenever the gyro moves it (e.g. to cancel auto-recentering).
; Name:value is on the view object; Root.Sub.Name:value on another object (same path syntax as Object=),
; e.g. Life is Strange: OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1
OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0
; Diagnostics: log properties on change / dump all properties once.
;Watch=IsAutoControlling
;Dump=1

[View.Auto3]
Id=3
Label=SBZClampCameraRotationModifier
; Active while the camera follows this rotation, instead of the view for the same view target (see below).
; Optional ViewTargetClass= limits it to that view target; without it, it applies to every one.
Match=Camera
Parent=1
Object=Camera.@SBZClampCameraRotationModifier
Offsets=0x68
RotatorSize=24
```

In Unreal games the profile's views are read again whenever the file changes, so edits (and views the probe adds)
apply without a restart. Its `[Gyro4All]` settings still apply from the next start.

### The stick's look input (`Property=RotationInput`)

Some games rebuild `ControlRotation` from other state every frame, so a turn written into it is lost. Dead Island 2
takes the view's yaw from the character's capsule, and the right stick turns the whole character instead. For those,
`Property=RotationInput` (on the PlayerController, the default view's object) sends the gyro down the right stick's own
path. The engine zeroes `RotationInput` at the start of each player tick, adds the stick to it, and turns the camera
from it in the controller's `UpdateRotation`. So Gyro4All doesn't write it from its frame hook: it hooks
`UpdateRotation` and adds the gyro there, just before the game reads it. The game's limits, its turn of the character
and whatever follows from it then apply to the gyro as they do to the stick.

`UpdateRotation` is found by its shape, not by address. Gyro4All goes through the controller class's virtual functions
and picks the one that reads `RotationInput` before any call, jump or return, which is the engine's first statement
there. `RotationInput` isn't reflected in UE4: its offset is the one just before the reflected `InputYawScale`, which
follows it in `APlayerController` (`InputYawScale_DEPRECATED` in UE5). The log says which one (`update rotation: ... UpdateRotation is vtable[N] at
<module>+0x...: hooked`). If none matches, or more than one does, the view isn't turned and the log says so.

```ini
[View.Default]
Label=Exploration
Property=RotationInput
```

### Several cameras for the same view target (`Match=Camera`)

Unreal games only. Some games turn the camera through different rotations while the view target stays the same. PAYDAY 3 uses
`ControlRotation` in normal play, but during some actions a camera modifier (`SBZClampCameraRotationModifier`) clamps
the view and rewrites `ControlRotation` from its own rotation. `ViewTargetClass` can't tell these apart, so such a view
uses `Match=Camera` instead:

- The base view (the `ViewTargetClass` view for the view target, or the default view) and the `Match=Camera` views
  for that view target form a group. The gyro turns the group's current view.
- The current view is dropped for the next one as soon as the game shows it isn't the camera's input:
  - **fast:** the game puts its rotation back after each gyro write (it rewrites it from the real input), 3 frames
    in a row: `camera: '<view>' is rewritten by the game, trying '<next>'`;
  - **slow:** the camera ignores the gyro written into it (at least 2 degrees over at least 0.2 s, no right stick or
    mouse), for a rotation the game neither rewrites nor reads: `... is ignored by the camera, trying ...`.
  The one the camera follows stays: `camera: the camera follows view '<view>' now (Match=Camera)`.
- Both checks judge yaw only: at the pitch limit (looking straight up or down) every camera ignores pitch, which
  isn't a camera change.
- The gyro the dropped view lost is written into the next one at once (`(N degrees carried over)`, at most 45 of yaw
  and 10 of pitch), so a camera change delays the turn a moment instead of eating it.
- If no view of the group moves the camera (a cutscene, or a camera held at a yaw limit), the view the camera followed
  last is used and the group waits 2 s before trying again: `camera: no view of this camera follows the gyro`.
- A view whose object doesn't exist right now (the modifier only exists during the action) is skipped at once.
- Each `Match=Camera` view has its own tab in the settings panel, starting with the settings of view 1. ADS, ALT_ADS
  and OTHER also apply to the `Match=Camera` views of the default view.

### Unreal Engine 3 games (`[Unreal3]`)

UE3 games need no view sections: the gyro turns the player controller's `Rotation`. By default the gyro stays off while
the world is a menu level (`WorldInfo.bIsMenuLevel`). Some games keep that flag on during play (Dishonored): their
profile turns the check off:

```ini
[Unreal3]
MenuLevel=0
```

## Finding the camera of a new game

> [!IMPORTANT] 
> Gyro4All guides you with on-screen messages but the game can't be using exclusive fullscreen or you won't see them.


Most UE games turn the camera from `PlayerController.ControlRotation`, and Gyro4All works with no profile.
Some games keep their own look rotation somewhere else: in a vehicle actor, in a camera component, or in
C++ memory that Unreal's reflection doesn't describe. The **probe** finds that rotation for you. It records
the camera-related state, you turn the camera with the right stick, and it logs what changed. The result
goes into the game's profile.

The steps below use the real Cronos: The New Dawn session as the example.

### 1. Check whether the game needs a profile

Install Gyro4All, play for a few seconds moving the controller to check if it moves the camera, quit, and open `Gyro4All\Gyro4All.log`. Gyro4All compares
how much it turned the view with how much the camera actually moved, and says it in one line:

```text
camera: confirmed, the camera follows the gyro (ControlRotation, view 'Camera')
camera: the gyro turned ControlRotation (view 'Camera') by 412 degrees but the camera moved only 0: the game ignores it.
        To find the camera's real rotation, set Probe=1 in ...Gyro4All\Gyro4All.ini, restart and turn the camera
        with the right stick.
```

The check only runs on the player's own camera (view target = pawn), so scripted or fixed cameras don't trigger it.
The first verdict needs three 2 s windows with at least 10° of gyro each and no stick or mouse. After it, the check
keeps watching and logs `camera: stopped following the gyro` after three such windows in a row (about 6 s of gyro motion),
`follows the gyro again` when it does; AutoProbe starts from the first one (see
[Learning a camera during play](#learning-a-camera-during-play-no-restart)). The warning also appears **on screen**, in a small box at the top of the game
window, with what to do next; so do the probe's progress messages below (capture ready, capture done, turn
discarded, suggested profile written). The box is drawn by Windows, not by the game, so it works with DX11 and DX12;
it doesn't show in exclusive fullscreen. `Notices=0` in `Gyro4All\Gyro4All.ini` keeps the messages in the log only.
The `status:` lines written every 30 seconds give more detail:

```text
status: view=Camera pc=SHPlayerControllerPlay_BP_C pawn=CronosCharacterPlay_BP_C target=CronosCharacterPlay_BP_C
        paused=0 cursor=0 | gyro enabled=1 motion=1 ... | applied yaw=-220.1 pitch=14.5
```

| What you see | Meaning |
|---|---|
| `view=Camera`, `applied yaw/pitch` changing, **camera moves** | The generic path works. No profile needed. |
| `view=Camera`, `applied yaw/pitch` changing, **camera doesn't move** | Gyro4All writes `ControlRotation` but the game ignores it. Use the probe. |
| `view=none` during gameplay | No pawn is possessed, or it is a placeholder `DefaultPawn` (normal in main menus). |
| `motion=0` | Controller problem, not a camera problem. See [Problems?](TROUBLESHOOTING.md). |

### 2. Enable the probe

Open `Gyro4All\Gyro4All.ini` (Gyro4All creates it with the defaults on the first run; releases don't ship it, so updates
keep your values) and set:

```ini
[Gyro4All]
Probe=1
```

`Gyro4All.ini` holds the settings for every game in that folder. A game's profile
(`Gyro4All\profiles\<game exe>.ini`) can override any of them in its own `[Gyro4All]` section.

`Gyro4All.log` now says `probe: results in ...Gyro4All-probe.log` and `profile: probe enabled (automatic capture on stick or mouse turns, F7/F8 manual)`. The
probe writes its results to that separate file, `Gyro4All\Gyro4All-probe.log`, which starts with a short reminder of
these steps; `Gyro4All.log` only gets one line per snapshot and diff. `F7` and `F8` are polled only while
the game window has focus. If the game uses those keys for something, press them where that doesn't matter
(e.g. standing still).

### 3. Capture camera turns

**Automatic (recommended).** With `Probe=1` Gyro4All finds the camera by itself and tells you on screen what to do.
The gyro output is **paused** while it runs, so the controller can stay in your hands.

1. **Captures.** Load into gameplay and stand still for a second (`Gyro4All.log`: `probe: automatic capture ready ...`).
   Turn the camera **with the right stick**, more than 30° sideways (and a bit up or down), then stop. Repeat until
   the notice says 3 captures are done. Camera turns made without the right stick (cutscenes, scripted cameras) are
   ignored. Every rotation that followed the camera in the captures becomes a candidate: one that now holds the
   camera's pitch and yaw, or one that changed by exactly the camera's turn (a copy kept with an offset or a small
   lag; Hogwarts Legacy keeps its look rotation 3° above the final pitch).
2. **Live test. Don't touch the controller.** Gyro4All writes a 3° turn into each candidate, exactly as a profile would,
   and watches the camera: it must turn by that amount and come back when the turn is undone, first in yaw and then
   in pitch. The camera twitches a little. A candidate the game rewrites every frame (an output), or that keeps the
   turn without moving the camera, fails. For a third-person camera the probe also checks the camera's location:
   the stick orbits it around the character (the captures measure the radius), so a candidate that only turns the
   view on the spot (a free camera) fails too. The ones that pass are tested again in a second round. Touching the stick,
   pausing or leaving gameplay pauses the test (it retries).
3. **Result.** A rotation that passed both rounds is **verified live**. When several pass, the probe takes one that
   turns pitch too, and among those one that **also turns the character** (`it also turns the character` in the log):
   that one is the look input that both the camera and the character read. A rotation that turns only the camera can
   be a camera stage the game rebuilds from it in other states (Silent Hill 2: the spring arm turns the camera
   standing still, but the game rewrites it while walking, and while aiming it moves only the crosshair).
   - **The game has no profile:** Gyro4All writes it, `Gyro4All\profiles\<game exe>.ini`. Restart the game to use it.
   - **The game already has a profile:** it is left untouched; the result goes to `<game exe>.suggested.ini` next to
     it. To use it, rename it to `<game exe>.ini` (replacing the old one) and restart.

   If nothing passes, Gyro4All asks for more captures (up to 8) and tests the new candidates. After that it writes only
   an unverified guess to `<game exe>.suggested.ini`; **an unverified guess never becomes the game's profile.**

Every test and its result is in `Gyro4All-probe.log` (`probe test (round N): <rotation>: MOVES THE CAMERA` /
`overwritten (output)` / `kept, camera didn't move` / ...), and the profile file lists every other rotation with
what the live test said about it. For Hogwarts Legacy, for example, the probe should verify:

```ini
; VERIFIED LIVE: the probe wrote small turns into this rotation and the camera followed them, twice.
[View.Default]
Label=Exploration
Object=ViewTarget.BehaviorStack
Offsets=0x68,0x80
RotatorSize=12
```

With a verified result Gyro4All sets `Probe=0` in `Gyro4All\Gyro4All.ini` by itself (and in the game's profile, if that one
set `Probe=1`), and the notice asks you to restart the game: the probe keeps the gyro paused while it runs, so the
profile only takes effect on the next start. Even a verified profile was only tested
on the camera of the captures: a game with other cameras (vehicles, aiming) may need more views (see below).

**Manual.** `F7` takes a snapshot, turn the camera with the right stick only (well over 30° to one side **and**
some degrees up or down, without walking), `F8` logs the diff to `Gyro4All-probe.log`. Diffs with a large enough turn
feed an unverified guess in `<game exe>.suggested.ini` (no live test in manual mode). Pressing `F7`/`F8` turns the
automatic probe off for the session.

### 4. Read the probe output

Open `Gyro4All\Gyro4All-probe.log` (it is recreated every game session). The diff has three parts. Every line starts with the object's **profile path**: the root (`PC`, `Pawn`,
`Camera` = PlayerCameraManager, or `ViewTarget`), followed by subobject names, then the object's class in
parentheses. Abridged output from Cronos:

```text
probe: roots pc=SHPlayerControllerPlay_BP_C_2147478565 pawn=CronosCharacterPlay_BP_C_2147478555 camera=SHPlayerCameraManagerPlay_2147478563 ...
probe: snapshot of 159 objects, 3179 values

  rot Camera(SHPlayerCameraManagerPlay).CameraCachePrivate.POV.Rotation (P 0.00 Y 150.00 R -0.00) -> (P 55.15 Y -62.26 R -0.00)
  rot Camera.TransformComponent0(SceneComponent).RelativeRotation (P 0.00 Y 150.00 R 0.00) -> (P 55.15 Y -62.26 R -0.00)
  rot Pawn.Animation(SHCharacterPlayAnimComponent).LastAimRotation (P 0.00 Y 150.00 R -0.00) -> (P 55.15 Y -62.26 R -0.00)
probe: diff done (6 rotators, 18 numbers changed)

probe: raw diff, POV pitch 0.00 -> 55.15 (d 55.15), yaw 150.00 -> -62.26 (d 147.74)
  raw Camera(SHPlayerCameraManagerPlay)+0x388 double 0.0000 -> 55.1492  = POV pitch
  raw Camera(SHPlayerCameraManagerPlay)+0x390 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.View(SHCharacterPlayViewComponent)+0x2A0 double 0.0000 -> 55.1492  = POV pitch
  raw Pawn.View(SHCharacterPlayViewComponent)+0x2A8 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.View(SHCharacterPlayViewComponent)+0x420 double 0.0000 -> 55.1492  = POV pitch
  raw Pawn.View(SHCharacterPlayViewComponent)+0x428 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.SpringArm(SHCharacterPlaySpringArmComponent)+0x2D8 double 0.0000 -> 55.1492  = POV pitch
probe: raw diff done (39 matches)
```

| Line | What it is |
|---|---|
| `rot <path>.<Property> (before) -> (after)` | A **reflected** `FRotator` that changed (nested structs are followed up to 3 levels, e.g. `CameraCachePrivate.POV.Rotation`). |
| `num <path>.<Property> before -> after` | A reflected float/double that changed. Useful for side effects (arm length, aim offsets, auto-recentering flags) and for `OnGyro=` / `Watch=`. |
| `raw <path>+0xOFFSET double/float before -> after = ...` | **Non-reflected** memory inside the object that now holds the camera's yaw or pitch (in degrees or radians), or changed by the same amount as the camera. |

The camera's final view (`PlayerCameraManager...POV.Rotation`) always changes, but it is an **output**: the
engine recomputes it every frame, so writing to it does nothing. You are looking for the **input**, the value
the stick writes and the camera reads. How to choose:

- **Ignore the camera manager and transforms**: `Camera(...)`, `Camera.TransformComponent0`, and component
  world transforms (`+0x100`/`+0x200` in scene components, the spring arm's world rotation). These are copies of
  the result.
- **Ignore animation consumers**: anim instances, aim offsets, `LastAimRotation`, head rotation. They read the
  camera; they don't drive it.
- **Prefer a reflected property** (`rot` line) on the PlayerController, the pawn, a vehicle, or a
  camera/"control"/"view" component, with a name like `...Rotation_Target`, `ControlRotation`, `LookRotation`.
- **Otherwise look for a raw rotator**: three consecutive values at `+N` (pitch), `+N+8` (yaw), `+N+16` (roll)
  for doubles, or `+N`, `+N+4`, `+N+8` for floats, in a component whose name suggests control or view. Here
  that is `Pawn.View(SHCharacterPlayViewComponent)`, a rotator at **+0x2A0** and another at **+0x420**,
  matching in both diffs.
- If there are two candidates that always hold the same value (current/target), list both.

### 5. Write the view in the profile

Turn the chosen line into profile keys:

| Probe line | Profile key |
|---|---|
| Path before `(` | `Object=` (e.g. `Pawn.View`). Omit it for `PC` + `ControlRotation`. |
| `rot ... .PropertyName` | `Property=PropertyName` |
| `raw ...+0x2A0` | `Offsets=0x2A0` (the offset of the **pitch** value, i.e. the start of the rotator) |
| `rot ... .Struct.Rotation` (nested in a struct) | `Property=Struct.Rotation` (dotted path through struct properties, e.g. Jedi: Fallen Order's `Property=ModeOutputs.Rotation` on `Object=Camera`) |
| `double` / `float` | `RotatorSize=24` (UE5) / `RotatorSize=12` (UE4) |
| `WorldRotation=1` in the suggestion | keep it (a component with absolute rotation, Example 3) |

**Example 1: non-reflected rotator in a pawn component (Cronos, UE5).** The camera is always the
character's, so the default view is redirected:

```ini
; profiles/Cronos-Win64-Shipping.ini
[View.Default]
Label=Exploration
Object=Pawn.View
Offsets=0x2A0,0x420
RotatorSize=24
```

**Example 2: reflected property on a vehicle (Pacific Drive, UE4).** On foot the generic path works. In the car
the camera's view target becomes the car actor, and the right stick writes its reflected rotator
`CameraControllerRotation_Target` (found with an earlier, script-based version of the same snapshot/diff method). An
extra view is added that is active only while the view target is the car:

```ini
; profiles/PenDriverPro-Win64-Shipping.ini
[Gyro4All]
Views=InCar

[View.Default]
Label=On foot

[View.InCar]
Id=2
Label=In car
ViewTargetClass=BP_PlayerCarNew_C
Property=CameraControllerRotation_Target
Parent=1
; From the num lines: the car re-centres its camera unless these are cleared while the player looks.
OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0,Camera_AutoControlAmount_current:0
```

**Example 3: a camera component with absolute rotation (Severed Steel, UE4).** The character turns its
`FirstPersonCamera` component directly from the component's current world rotation (look input, then
`SetWorldRotation`), and copies the result to `ControlRotation` every frame, so `ControlRotation` is only an output.
A component with absolute rotation keeps its rotation twice: in `RelativeRotation` (reflected) and in its world
transform `ComponentToWorld` (not reflected), which is what the game reads back. Writing `RelativeRotation` alone
is undone the same frame. `WorldRotation=1` writes both:

```ini
[View.Default]
Label=Exploration
Object=Pawn.FPCamera
Property=RelativeRotation
WorldRotation=1
```

Gyro4All finds `ComponentToWorld` by value the first time (its quaternion equals the `RelativeRotation` one; log:
`ue: CameraComponent ComponentToWorld rotation at +0x1C0`) and only writes while the two still agree. If they
don't (the component isn't absolute, or the game is between its writes), it skips that frame and logs
`profile: view N WorldRotation not applied to ...` with the reason. The probe suggests this form by itself for
attached components whose world rotation matches their `RelativeRotation`, a camera component first.

Use the **default view** with `Object=` when the game always uses that camera. Use an **extra view** with
`ViewTargetClass=` when a different camera takes over in some situations (vehicles, turrets). Extra views use
the view target as their object unless `Object=` is given. Each view gets its own tab in the settings panel.

### 6. Verify and clean up

1. Start the game and move the controller. With raw offsets, the log adds a line per status:
   `status: view object View +0x2A0=(P -13.34 Y 167.93) +0x420=(P -13.34 Y 167.93)`. If it says `(not found)`,
   the `Object=` path is wrong.
2. Check that the camera turns smoothly, that up is up, that aiming follows the camera, and that the right
   stick still works. If the camera jumps back or fights the gyro, you picked an output: try another
   candidate.
3. Set `Probe=0` (or remove the line) once the profile works. The automatic probe does this by itself after a
   verified result; with a hand-written profile it is up to you (while `Probe=1`, the gyro stays paused).

Raw offsets depend on the exact game build and may break when the game is updated. Prefer a reflected property
when one exists, and note the game version in the profile.

### What the probe covers

- **Objects:** the PlayerController, the pawn, the PlayerCameraManager, the camera's view target, and every
  object whose Outer chain leads to one of them (components and other subobjects, up to 8 levels).
- **Reflected values:** every `float`, `double` and `FRotator` property of those objects, following nested
  structs up to 3 levels (vectors, quaternions, transforms and colours are skipped). Rotators are logged when
  any component changes by more than 0.5°, numbers when they change by more than 0.001 (at most 150 rotator and 250 number
  lines per diff).
- **Raw memory:** each object's bytes up to its class size, compared as aligned floats and doubles. A value is
  reported if it now equals the camera's yaw or pitch (±0.6°, degrees or radians) or changed by the camera's
  delta. Turns smaller than 2° are ignored (at most 300 lines).
- **Not covered:** objects that aren't subobjects of those roots (e.g. a separate camera actor that isn't the view
  target, world subsystems) and plain C++ heap data that isn't stored inside a UObject. For those, a debugger or
  memory scanner is needed. The probe only reads memory and never writes.

### Learning a camera during play (no restart)

The probe (Unreal games) can also start in the middle of a game, for the camera of that moment, without `Probe=1` or
a restart:

- **Probe key** (`ProbeKey=Ctrl+F7` in `Gyro4All.ini`; F1..F24 with optional `Ctrl+`, `Shift+`, `Alt+`; empty = none):
  starts the probe where you are. The same key cancels any probe, also one started with `Probe=1`.
- **Automatically** (`AutoProbe=1`, the default): when a camera that followed the gyro stops following it
  (`camera: stopped following the gyro`), or the game's camera ignores the gyro from the start, the probe starts by
  itself and says so on screen. The gyro stays on meanwhile (the camera ignores it anyway); if the camera follows the
  gyro again, or no turn with the stick or mouse is captured in 45 s (a cutscene), it gives up. At most 3 automatic
  starts per session, 30 s apart.

Then turn the camera with the **right stick or the mouse** (more than 30°), stop, 3 times, and leave the controller
alone during the short live test. The verified rotation is **added to the game's profile** as a new
`[View.Auto<id>]` with `Match=Camera` (the file is created if needed; its other views and settings are kept, and a
rotation the profile already has isn't added twice), and Gyro4All uses it at once. Log:
`probe: camera learned: <rotation> added to ... as view <id> '<label>' (Match=Camera)`.

A probe started with `Probe=1` in a game that already has a profile adds its camera the same way. Only in a game with
no profile does it still write the profile's default view (a restart applies it).

## Building from source

Requirements:

- Visual Studio 2022 or newer with the C++ desktop workload (MSVC x64, CMake 3.24+). C++20.
- Git Bash (for the helper scripts).
- The GyroLib **1.3.1** Windows x64 SDK, from the
  [GyroLib releases](https://github.com/lud-berthe/GyroLib/releases). Verify it against `SHA256SUMS` and
  extract it to `third_party/GyroLib-1.3.1/sdk/GyroLib-1.3.1-sdk-windows-x64/`. The pinned version is in
  `tools/gyrolib-sdk.sh`.

```bash
bash tools/build-gyro4all.sh      # -> build/Gyro4All/Release/winmm.dll
bash tools/test-gyro4all.sh       # build + unit tests (tests/profile_tests.cpp, no game needed)
bash tools/package-gyro4all.sh    # build + build/release/Gyro4All-v<version>.zip (installable release)
```

Gyro4All links the static MSVC runtime. `gyrolib.dll` is delay-loaded by full path from the `Gyro4All` subfolder.

Repository layout:

| Path | Contents |
|---|---|
| `src/core/` | The engine-neutral mod: proxy, hooks, GyroLib wiring and per-frame host logic (`host.cpp`), settings, XInput, panel (`overlay.cpp`), the `Engine` interface (`engine.hpp`) |
| `src/engines/unrealEngine/` | The Unreal Engine module: UE reflection access (`ue.cpp`), scene and views (`ue_engine.cpp`), the profile's views (`ue_profile.cpp`), camera probe (`ue_probe.cpp`) |
| `tests/` | Unit tests of the parts that need no game (`profile_tests.cpp`) |
| `profiles/` | Per-game profiles |
| `games/<Game>/` | Per-game notes and scripts |
| `tools/` | Build, deploy and benchmark scripts; `gyroprobe` sensor test tool; per-engine debug and camera-finding tools in `tools/<engine>/` (each with a README) |
| `bench/` | Performance measurements |
| `docs/` | User documentation: this file, `TROUBLESHOOTING.md`, `CREDITS.md` |
| `MODLOG.md` | Development journal: findings, offsets, dead ends |

From a source checkout, `tools/deploy-gyro4all.sh` installs into a game (`remove` as a second argument
uninstalls; an existing `gyrolib.ini` is never overwritten):

```bash
bash tools/deploy-gyro4all.sh "D:/Games/MyGame/MyGame/Binaries/Win64"
```

## Performance

Gyro4All's per-frame work is roughly 50–100 µs on the game thread, plus 15–40 µs in the `Present` hook while the
panel is closed. Benchmarks in Returnal showed no measurable FPS or frametime change. See
[`bench/BENCHMARKS.md`](../bench/BENCHMARKS.md).
