# MODLOG: universal gyro aiming for any game (Gyro4All, formerly GyroUE)

Goal: DualSense gyro turns the real game camera in any single-player game (or multiplayer without anti cheat), without mouse emulation 
or gyro to stick.

## Findings

- 2026-11-10 **RAGE (2011, id Tech 5, Steam 9200, `Rage64.exe`): detection.** Gyro4All 1.11.0 loaded (winmm.dll, OpenGL pump thread, panel,
  DualSense samples) but took it for id Tech 6: RAGE has DOOM's `base\gameresources.resources` marker, not TNO's `master.index`/`chunk0.resources`.
  Type info 2856 classes, none of the player's fields; RTTI `idGameLocal` exe+0x1973AB8, `idPlayer` exe+0x19A4D38, `idPhysics_Player` exe+0x1C06E58;
  the exe has the `idUsercmdGenLocal` RTTI name. Change: `base\streamed.resources` + `virtualtextures\` mark id Tech 5 before DOOM's marker.
  Plan: `games/RAGE/MODDING_PLAN.md`.
- 2026-11-11 **RAGE: gyro works on foot** (user-confirmed, DualSense). `idUsercmdGenLocal` exe+0x290BE80, input angles +0x10 (a +30 yaw write
  turned the view). RAGE's own offsets (TNO's are wrong here): physics pointer idPlayer+0x340; the physics object holds repeated
  player-state blocks: command angles short[3] at physics+0x30a6 (SHORT2ANGLE), command time at +0x30c4 (new every tick: the pause gate),
  float view angles at +0x3120 (copy at +0x31b0); deltaViewAngles not located (0 on foot), the check takes view = command. The heap scan
  missed the player: RAGE's idPlayer is 8-aligned, `scan_region` only read 16-aligned qwords (now 8).
  - **Vehicles:** the player's view and command angles freeze while the vehicle camera keeps resetting the input angles to 0; the gyro
    there moved the vehicle camera. Gate: gyro off while the command stays >5 degrees from the input angles for 250 ms (log `idtech: RAGE:
    the player's command no longer follows the input angles`). Not yet tested in game.

- 2026-11-10 **RDR2: flick stick 180s in first person turned only 126-136 degrees and drifted; 90s and third person
  exact (user report; flick style 1, snap 1, 150 ms).** Not the Cyberpunk cause (RAGE has one view, no camera matching).
  Read-only sampler `games/RedDeadRedemption2/flick_sampler.py` (active camera at ~1 kHz; stored front `+0x7b0`, else
  the frame's forward row `+0x30`, as `read_view`): third person (`exe+0x339a6c0`) 90.0 and 180.0 every time; first
  person (`exe+0x3396310`, no stored front, the gyro turns its frame) 126-136 degrees on 20 flicks. In each, the game keeps
  the written row up to ~88-106 degrees, then each update moves it back to a heading that trails the turn (88, 91, 95,
  100, 106, 113 degrees from the start, the same in every flick): the first-person camera stays within ~88 degrees of
  the body, which follows at a limited, rising speed; the turn past that edge is lost. Fix (`rage_engine.cpp`): on the
  first-person camera, the yaw an update took back from the last write (at most that write's turn) goes again with the
  next frames' turns (and in `after_frame` on frames without a turn) until the body takes it; dropped by a turn the
  other way, another camera, or after 1.5 s. Log `rage: the first-person camera held back ... carried to the next
  frames` once. Builds, 1034 checks; **user-tested: first-person 180 flicks work** (DualSense shown as XInput, not under
Steam Input; the connection wasn't recorded). Also seen once: right after a first-person flick the view
  eased back ~80 degrees over 1.3 s by itself (not explained).
- 2026-11-10 **Cyberpunk 2077: GyroLib's flick stick (defaults, duration 150 ms; NativeFlickStick=1) turned only 20-50
  degrees instead of 90/180 (user report).** Sensor pacer ruled out (the real `sensor_pacer.cpp` over a simulated 90 degree
  flick delivers it all; here it runs in GyroLib's burst mode anyway) and REDengine's `limit_turn` doesn't limit on-foot yaw.
  Read-only sampler `games/Cyberpunk2077/flick_sampler.py` (body heading at ~1 kHz against the status lines' `applied yaw`):
  the body turned exactly what Gyro4All wrote (-8.5 vs +8.5 over 30 s, no steps back), so no writes lost; the turns were
  +35, -21, -50, +27 degrees in 17-79 ms, i.e. GyroLib's flick itself ended early. Cause, in the log: every flick logged
  `camera on foot -> cinematic` and back 1-2 ms later. The view is classified by matching the FPP camera's orientation
  with the active camera data, a copy a frame older; the matched camera's tolerance (`kContinueDot`, ~20 degrees, MODLOG
  2026-11-05) is less than a flick's turn between them, so one frame read as cinematic (view 0) and GyroLib cancelled the
  flick there. Fix (`red_engine.cpp`): the tolerance of the camera matched last frame is 20 degrees plus the turn Gyro4All
  wrote in the last 100 ms (`note_own_turn` in `apply`). Builds, 1034 checks; user-tested: flicks turn the full 90 and 180 degrees again (DualSense, native pad flick stick; the XInput route is not tested). Repeated left-right flicks drifting a few degrees is GyroLib's free pivot plus circular turning (`flick.snap=0`, `flick.style=0`), not a loss: `flick.snap` 1 or 2 with `flick.style=1` makes them return exactly.
- 2026-11-10 **Cyberpunk 2077: sitting on a chair (a workspot) turned the view in little steps and drifted (gyro Always
  On).** Log: `can't write the first-person view (neither on foot nor seated): gyro as right stick`, then the blind stick
  fallback (default dead zone 0.20 / curve 1.50: pulses to get past the dead zone, and any leftover motion integrated by
  the game; GyroLib's bias read 0 for a while after two `gyrolib.ini` reloads from the panel, not investigated).
  Read-only sampler (`games/Cyberpunk2077/chair_sampler.py`, ReadProcessMemory every 50 ms; follows the FPP camera's
  address in the log, since the game replaces the player's camera at some transitions). On the chair: moveComponent
  `+0x158` 0, FPP `+0x400` 0 (so neither on foot nor in a vehicle seat), FPP `+0x418` 1 (0 on foot, 1 seated), FPP
  `+0x454`/`+0x460` 7/50. The look node turns the head as in a vehicle seat: yaw `+0x1AC` and pitch `+0x1B0`, read back at
  FPP `+0x42C`/`+0x430`, clamped to the FPP limits `+0x3E0`/`+0x3E4` (-47.3/58.6) and `yawMaxLeft/Right` `+0x3E8`/`+0x3EC`
  (62.3/-57.7; yaw grows to the left; the look yaw stopped at 62.3 and -57.6). Held 11.2 degrees for 6 s with the stick
  released: no recentring. Sitting down (~3 s) and getting up (~3 s) the limits ramp (+-80/+-180 <-> the chair's) and FPP
  `+0x430` stays frozen while the look node moves (the camera follows the animation). Fix (`red_engine.cpp`): chair =
  `+0x158` 0, `+0x400` 0, `+0x418` 1; written like the seat (look yaw + pitch) within the FPP's yaw and pitch limits, only
  once the limits have held 300 ms and the look node's totals (`+0x1BC`/`+0x1C0`) match the FPP outputs within 10 degrees;
  no nudges. And whenever the FPP camera is active but its sources can't be written (sitting down, getting up, states not
  known yet) the gyro is off (no gameplay view) instead of the blind stick fallback (`... : gyro off`); the status line says
  why. On-foot view (view 1) on the chair. Builds, 1034 checks; not tested in game yet; `Gyro4All-v1.11.0.zip` repackaged.
- 2026-11-10 **Cyberpunk 2077 (1.10.1, user report): no gyro on foot, `view=cinematic` and `gyro enabled=0` the whole
  session (L2 hold and Always On both).** The log: `gameFPPCameraComponent keeps the active camera's orientation at +0xd0`,
  then `camera on foot -> cinematic` 170 ms later; "on foot" again only for under a second at two moments in 10 min.
  Our 2.50 test had learnt **+0xF0** (world orientation). +0xD0 is `entIPlacedComponent.localTransform`'s orientation
  (relative to the PlayerPuppet), equal to the world one only while the player faces world heading ~0, as on the first
  frame here. `orientation_offset` kept the first offset that matched on one frame, scanning up from +0x90, for that
  class for good. Fix (`red_engine.cpp`): an offset is kept only after it matched through a 10 degree heading sweep;
  until then any offset matching this frame counts (gyro on right away). The vehicles' third-person camera ("matched once
  then not") was likely the same bug: if it is learnt now, it reports no gameplay view (no gyro, no stick fallback), as
  before. Also: the FPP camera learns only on foot (FPP `+0x400` = 0; seated, a head turn in a vehicle facing heading 0
  would sweep the local orientation too), and a kept offset that stops matching is replaced by another that matches
  through the same 10 degree sweep (`... at +0x.. now (... +0x.. stopped matching)`), so a wrong one can't last the
  session; after an offset is kept, other offsets don't count until they replace it. Builds, 1034 checks; not tested in game. `Gyro4All-v1.11.0.zip` repackaged with this fix (version kept at
  1.11.0 on the user's choice; the zip also carries the uncommitted Glacier look changes in the working tree).
- 2026-11-08 **RDR2 (`kSettingAPI_Vulkan` in `system.xml`): GyroLib's panel shows but not the "Gyro4All settings" window; fix
  built, in-game test pending.** No `captured Vulkan swapchain` line: the panel goes through DXGI (`captured D3D12
  swapchain`; why a Vulkan setting ends on D3D12 is not established). The game captures a swapchain for one window, 2.5 s
  later another (same handle slot, new uniquifier: the first window was destroyed). GyroLib's overlay moved to the new
  swapchain, gyro4all.dll stayed on the dead HWND: `follow_window` only saw `g_window`, which the DX12 path never set, and
  `api_set_window` refused DX12 mode. So `GetClientRect` failed, `io.DisplaySize` was 0x0 and `RenderDrawData` returned at
  once with no error. Fix: `overlay::attach` passes the captured DX12 window to `panel_host::follow_window(dx12_window)`
  (a D3D10/D3D11 swapchain the game also makes does not move a DX12 panel) and `api_set_window` accepts DX12 mode.
  Expect `the panel's input moves from window ... to window ...` in the log.

- 2026-11-07 **DUSK (Steam, Unity Mono x64): controller fixed with the game's own `controllermode = ds4`; no code change.**
  Symptom (DualSense, no Steam Input): menu buttons mixed up, in game the view looked up and spun. Not our XInput: the log
  has `DualSense ... not shown: it imports the HID library` (UnityPlayer.dll opens 054C:0CE6), the game reads the pad
  itself. Decompiled `Assembly-CSharp.dll`: input is only Unity's Input Manager (`Input.GetAxis("joystickaxisN")`,
  `GetButton`); `XInputDotNetPure` only for rumble. `MyInputManager.LoadControllerBindings` reads `mappings/<preset>.ini`
  `[SETTINGS] controllermode` -> `Gamepad.Ds4` (right stick axes 3/6, South = button 1) or `Gamepad.X360` (right stick
  axes 4/5, South = button 0). All four shipped presets say `xbox360`; with a PlayStation pad axes 4/5 are L2/R2 resting
  at -1, hence the view pinned up and turning. User set `controllermode = ds4`: fixed (and `DualSenseAsDualShock4=1`
  learned). Gyro view: `MyMouseLook` `xRotation`/`yRotation` (shipped as `profiles/DUSK.ini`). Gyro4All's XInput can't
  serve this game: Unity's legacy joystick input doesn't see an XInput-only pad and the Input Manager axes
  (`GetAxis`/`GetButton`) aren't hooked (The Forest only covers `JoystickButton` keys). `docs/GAMES.md` row added.

- 2026-11-07 **1.11.0 — Unity engine module** (MSVC 14.51, 1022 checks pass; `Gyro4All-v1.11.0.zip`). Since 1.10.1: new
  Unity module (Mono and IL2CPP, `src/engines/unity/`): `Camera.main`'s rotation as the generic view, `[Unity]` profiles
  for script floats or a body's Transform, the camera probe (with the input-free search `probe_discover`), new profiles
  (Metal, Risk of Rain 2, Subnautica, TheForest, Viewfinder; Borderlands4 for Unreal), and the probe crash fix below.
  Mostly build and unit tests; each entry says what ran in game. Unity entries from 2026-11-06 on.
- 2026-11-07 **The Planet Crafter Demo (Unity 6000.0.59, Mono): the automatic probe crashed the game (twice). Fixed, built;
  last fix not run in game.**
  - Discovery scanned 572 scripts (2110 floats), nudged 244 look-named floats: none turned the camera. Fallback body test:
    the camera's body is 1 level up (CharacterController/Rigidbody), `yaw on parent 1` passed (asked 3.0, moved 3.0). Crash.
  - Crash 1 (`Player.log`: `Transform:get_rotation_Injected` <- winmm): `unity_probe.cpp`'s body test pitch stage turned
    `g.camera`, the **Camera component**, as a Transform (also in the undo on probe key / lost gameplay). Fix:
    `body_target()` (camera's own Transform for pitch, the parent for yaw) and `body_undo()`. Retest: both stages passed
    (asked 3.0, moved 3.0, kept), profile written (`[Unity] YawParent=1`, `Source=auto`).
  - Crash 2 (`end()` -> `release_scene()` -> `mono_gchandle_free`, dump symbolized with cdb): this Mono
    (`mono-2.0-bdwgc.dll`, Unity 6) has pointer-sized GC handles; the classic `mono_gchandle_*` take/return 32 bits, so the
    handle was truncated (0x2D6A3401600 -> 0xA3401600). Fix (`unity_runtime.cpp`): use the `_v2` exports
    (`mono_gchandle_new_v2`, `_new_weakref_v2`, `_get_target_v2`, `_free_v2`) when all four exist, else the classic ones
    typed 32-bit. IL2CPP unchanged. Older Mono games with handles below 4 GB never showed it.
  - Debug note: the PDB name was stored as literal `%%%winmm.pdb%%%` (`/PDBALTPATH:%_PDB%` via the VS generator); now
    `/PDBALTPATH:$<TARGET_PDB_FILE_NAME:...>`. For older builds copy the PDB under that literal name next to the DLL.
  - Run of the fixed build: yaw and pitch turn, but after looking up with the stick and down with the gyro, the next stick
    touch puts the camera back up. Decompiled: `SpaceCraft.PlayerLookable` is disabled in `Start` (its `Update` never
    runs); `InputOnLookDirection` calls `UpdateRotation`, which clamps `_pitch` to `+-MaxPitch` (80) and sets
    `m_Camera.transform.localEulerAngles = (_pitch, 0, 0)`; body yaw via `transform.Rotate`. So the camera pitch is rebuilt
    from `_pitch` on input, and a write to `_pitch` alone only shows on the next input (why nudges never saw it).
  - New `[Unity]` key `PitchTransform=1` (`unity_profile.cpp`, `turn_pitch_transform`): with a pitch float, the gyro also
    turns the camera's own Transform (after the body's yaw), in turns and recentering. `profiles/Planet Crafter.ini`:
    `ObjectType=SpaceCraft.PlayerLookable`, `PitchPath=_pitch`, `YawParent=1`, `PitchTransform=1`, `PitchLimit=80`.
    1024 checks; not run in game.
  - Automatic version (`probe_pitch_state`, `PitchStateMatch` in `unity_candidates.hpp`): for a view whose pitch is on the
    camera's Transform (`YawParent=`, no pitch float), floats of MonoBehaviours on the camera and ancestors are scanned
    once per camera; after each of the player's own turns (look input this frame and last; one sample per 0.5 degree of
    pitch) each float is compared with the camera pitch in four units (-1, 1, +-radians, wrapped 0..360; 0.05 degree
    tolerance). A float matching 5+ times over 10+ degrees, missing at most a quarter as often, is written with
    `PitchTransform=1`, `Source=auto`; the Source=auto check confirms it. Dropped after >3 misses and more than a quarter.
    `reject_auto_view` also clears `PitchTransform`. 1034 checks (`test_unity_pitch_state`); not run in game. The shipped
    manual profile means the watch only runs without it.
- 2026-11-07 **The Forest (Steam, Unity 5 Mono x64, Rewired): no controller in game even with `XInput=2`. Fixes built, not run
  in game; the controller is still open.** DualSense, no Steam Input.
  - `unity=failed`: this `mono.dll` doesn't export `mono_array_length` (required), so the whole Unity module was off. Now
    optional; `Runtime::array_length` reads `MonoArray.max_length` at +24 (32 bits). 1013 checks.
  - `XInputTrace=1`: `unity: Mono runtime attached`, `camera=true`, target `Camera.main (Mono)`. `xinput1_4.dll!XInputGetState
    called from (no module: generated code)`: a Mono P/Invoke stub calls the export directly, no game module imports it, so
    the import hooks never saw it. New `Engine::managed_input()` (Unity: true) -> `Settings::export_hooks`: inline hooks on
    the exports of xinput1_4/1_3/9_1_0. `XInputTrace` can't hook those any more (MinHook refuses a second hook; logs `could not
    be hooked`); the `the game polls XInputGetState` line and per-slot counters show the calls instead.
  - In game: slot 0 answered (1343 polls in 10 s), the menu moves with the stick and shows (A), but A does nothing. The stick
    goes through Rewired (`UICamera.GetAxis = TheForest.Utils.Input.GetAxis`), confirm through NGUI's
    `UICamera.submitKey1 = KeyCode.JoystickButton0` via `UnityEngine.Input.GetKeyDown`, which is Unity's own Raw Input + HID
    code in `TheForest.exe` and never sees an XInput-only pad. Fix: `hook_legacy_keys` (`unity_engine.cpp`) hooks the native
    `Input.GetKeyInt/GetKeyDownInt/GetKeyUpInt` (via `mono_lookup_internal_call`, Mono only) and answers `JoystickButton0..9`
    / `Joystick1Button0..9` from `xinput::shown_buttons` in Unity's Xbox order (`unity_legacy_keys.hpp`). `Input.GetButton`/
    `GetAxis` not covered. 1022 checks; not run in game.
  - Open: `TheForest.exe` was gone ~30 s after the last log line, shortly after the probe started nudging 600 floats among
    56212 scripts; unknown whether the user closed it. Rewired (2016) keeps its Windows code in `Rewired_Windows_Lib.dll` as
    a DES-encrypted resource: not decrypted.

- 2026-11-07 **Risk of Rain 2: UI focus and the still-camera wait (DualSense, no Steam Input).** Focus named real menus and
  the HUD no longer counted (`GenericMenuButton (Singleplayer)`, `SettingsEntryButton`, ..., with `MPEventSystem` (pad));
  between menus focus is gone ~1 s, and 2.6 s before the stage loads.
  - Run 1: the search started in the intro: `Scene Camera < Main Camera` is `Camera.main` and stands still while
    `Menu Main Camera` follows the pan, so the still-camera wait can't see the intro. On the stage the gyro was on (`70/s,
    99% turned`) but the camera never settled within the 10 s played (a pad in hands), so no profile was written.
  - Run 2 (build that still counted `Selectable`s): search at 1.5 s on the intro (`Cursor.visible=0 lockState=1` from the
    start, so the cursor rule allows it); the stage's new camera `Scene Camera < Main Camera(Clone)` was never searched: its
    HUD counted as a menu (`6 interactive UI elements`) for the whole minute of patience. By hand the probe verified the
    expected `RoR2.CameraRigController`, `CameraField=sceneCam`,
    `_cameraMode.camToRawInstanceData[InstanceData].pitchYaw.pitch/.yaw`, -1 and 1.
  - Changes (builds/unit tests only): until a camera has had its search the gyro waits (`probe_discovery_pending`); a menu
    ends a search without counting it; 3 s after focus goes the search restarts; the start notice is gone (it showed over
    the intro). **Nothing can say for sure that a scene is gameplay, so the search no longer relies on one try:** it waits
    for a still camera (`ViewStillness`: within 0.5 degrees for 1.5 s; the intro pans ~0.85 deg/s; 20 s patience); it
    belongs to one camera (a new camera or a menu ends it); it returns on each new camera with no wait, once 30 s later on
    the same one, and when the quiet watch sees the player turn that camera (the one real proof of gameplay). Up to 12
    searches per session.
  - Open: a first-input signal could filter the intro, but the core has no button state for engine modules. A search that
    finds a menu's own script would write a profile whose object goes away with the menu (never seen).
  - **Dead end (never run in game):** counting 3+ active `Selectable`s as a menu; a HUD easily has that many, and being
    wrong costs the gyro in gameplay for a minute (focus being wrong only costs a search).

- 2026-11-07 **Unity: the camera is found with no profile and no player input (`probe_discover`). Builds and unit tests only,
  except Viewfinder.** In none of the Unity games tried was the default camera right. Unreal has one look state
  (`ControlRotation`); Unity has none: `Camera.main`'s Transform is usually the game's output, not its state, so writing it
  gets undone or, as in Viewfinder, sticks and leaves the game's state behind. Class names are per game, so a catalogue
  wouldn't reach most games.
  - **Search with no input:** in gameplay, perspective camera, mouse captured, 1.5 s on the same camera; the gyro waits.
    Floats scored by name words and by being on a camera's rig (`discovery_score`, unit-tested; settings, limits, old
    values and effects on top skipped) get +0.25 for 100 ms, 16 at a time, split in half when a camera moves. One value
    left: the camera's turn gives its axis and units per degree (`nudge_per_degree`) and it must be kept by the game. Then
    the 3-degree live test decides; the profile gets `Source=auto`. Budget 15 s, up to 4 searches, one per camera.
  - **Yaw with no float** (classic script's `body.Rotate`): the nearest ancestor with a `CharacterController` or `Rigidbody`
    gets the live test, `YawParent=N`; pitch stays on the camera's Transform. Also serves the manual probe. Risk: a car's
    Rigidbody under its camera would take the yaw; only the check below catches it.
  - **`LookStateCheck` (the first real turn is the proof):** with stick or mouse and no gyro in the frame, 20 degrees of
    camera turn with under 25% in the state, twice in a row, removes the `[Unity]` keys, adds the object to `Rejected=` and
    searches again (nudging can't tell the look state from an effect added on top).
  - **Nothing found:** the old Scan, Pick, Capture and Test run quietly up to 5 min per camera with the gyro on and no
    notices; a new camera ends a watch that saw no turn. Ctrl+F7 starts the normal probe. A float profile with no
    `PitchLimit=` stops at 89 degrees.
  - **Viewfinder by hand (array fix):** verified `ObjectType=PlayerHeadRotation`, `PitchPath=pitch`, `YawPath=yaw`; camera
    `Player World Camera < ScreenShaker < Head < Player Body < ...`. `MoveScreenTexturesWithLook.previousHeadRotation` failed
    as `rewritten by the game`, as it should. Bug shown: `PitchPerDegree=-1.0638`, `YawPerDegree=0.7811` from the correlation
    with a lagging camera (yaw 22% short while passing the 1-degree tolerance); the live test's own measure now replaces the
    correlation's and snaps to -1 and 1.
  - **In game, profiles deleted (user):** *Viewfinder found with no input*: `PlayerHeadRotation.pitch/.yaw`, exactly -1 and 1,
    13 s after the camera appeared; stick turns confirmed both; user confirms the camera. The search went on for a second
    candidate (`PlayerMovement._lookInputVector`, `KinematicCharacterMotor._transientRotation`); now it stops at one per axis
    (a failed test comes back), a turn over 16 degrees or still going is no angle, Quaternion/Matrix4x4 fields aren't scanned.
    *Risk of Rain 2 not found* and it searched while loading/in the menu (it keeps `lockState=1` and draws its own cursor):
    used its 4 tries before gameplay. Now an active GameObject in any `EventSystem.currentSelectedGameObject` = a menu: no
    search / generic view there, up to a minute per camera; 8 tries. Open: a mouse-driven menu has no focus.
    Why it wasn't found: the state is `rig._cameraMode.camToRawInstanceData` (a `Dictionary`) -> `InstanceData.pitchYaw`; the
    scan stopped at one reference and `kTypeClass`. Scripts named like a camera or on its rig now go 3 references deep
    through arrays, `List<>` and `Dictionary<,>` values; paths gained `name[Class]`. `RoR2.CameraModes.prevGyroAngle` (the
    game's own gyro code?) not looked into.
  - **Deliver At All Costs:** `Cursor.visible=0 lockState=2` (Confined), so the cursor rule keeps the gyro off
    (`camera without mouse look`) and this search doesn't run there.
  - To verify: Viewfinder profile-less result, stick+gyro mixing, the wait's feel, Deliver, how far the name words reach.

- 2026-11-07 **Viewfinder (Epic, IL2CPP): the probe never gave its instructions, and the stick breaks the view after gyro use.**
  Fixes built, not run in game.
  - **Probe, our bug on every IL2CPP build without `il2cpp_array_addr_with_size`:** log had `probe: started` and nothing
    else. `Runtime::array_element`'s fallback put elements at offset 24 (`max_length`); `Il2CppArray` is object 16 + `bounds`
    8 + `max_length` 8, elements at 32. Element 0 read the length as a pointer, the last was lost, one camera = empty list,
    `scan_step` retried silently for ever. Fixed (with a layout check); also a start notice, a camera-count log line, and a
    notice + end after 5 empty tries. The fallback also serves `ObjectType=` lookups.
  - Stick+gyro hypothesis (from `global-metadata.dat` v31): `PlayerHeadRotation` has floats `pitch`, `yaw` (and
    `ProcessLookInput`, `UpdateHeadTransform`, ...). The generic view writes the camera's world rotation and leaves those
    floats behind. (Confirmed by the entry above.)
  - Cursor rule seen: `lockState=1` in gameplay; pause gives `lockState=0` and `no gameplay view (camera without mouse
    look)`. The status line's `applied pitch=-91` is the gyro's running sum, not the view's pitch. 942 checks.

- 2026-11-07 **Deliver At All Costs, first run of the new Unity build (Epic, IL2CPP, DualSense): no F10 panel, and the gyro
  turned the opening cutscene.**
  - **Panel, a core bug:** `panel: captured D3D11 swapchain` twice: the game's window, then 0.5 s later a second window that
    never presents (made just before Epic's `EOSOVH-Win64-Shipping.dll` hooked imports; which module is not established). It
    replaced the target. `overlay::attach` won't load `gyro4all.dll` for a target that doesn't present (Alan Wake 2 guard,
    2026-10-30) and `adopt_presented` required the DLL already loaded: neither could go first, so no `F10 pressed` line and
    the pad shortcut's panel closed at once. Fixed: adoption needs only a target idle for 500 ms (a D3D12 swapchain still
    can't be adopted). Any D3D10/D3D11 game; Viewfinder may have had it. Second run: `the panel moves to it`, `gyro4all.dll
    loaded`; the panel not confirmed by the user.
  - **Cutscene:** metadata has Rewired, `Slate` cutscenes, `DAAC.UI.CinematicManager`, `IsPlayingCinematic`,
    `IsGameplayInputEnabled`, `DisableMouse`, `CursorLockMode`.
  - **Dead end (removed at the user's request): "gyro off on each new camera until the stick or mouse turns it"**: it also
    failed on its own terms (`the camera answers look input` switched the gyro on, the fixed-camera verdict parked it 28 s
    later; likely a shot cut while the stick moved).
  - **New rule (user's idea), a hypothesis:** a hidden cursor with `Cursor.lockState` other than Locked means no mouse look
    -> `view_id` 0 (parked camera). Logged as `unity: Cursor.visible=... lockState=...` on every change. Profiles aren't
    subject to it. Kept: the fixed-camera windows split motion with/without look input; a camera turning less than twice as
    fast with it doesn't count. Next candidates if Deliver locks the cursor: Rewired's mouse state, `IsGameplayInputEnabled`.

- 2026-11-07 **Unity module: review against the plan and Unreal, fixes, and a new probe (build and unit tests only).** 937
  checks. Replaces what the entries below say about the managed probe and the pitch clamp.
  - Facts checked in the binaries: x64 `mono_string_chars` is `rcx+0x14`, `mono_string_length` `rcx+0x10` (old offset 12 read
    the monitor pointer as length, so names read empty or wrong). `System.Type.GetFields`, `Type.get_FullName`,
    `MemberInfo.get_Name`, `FieldInfo.get_FieldType` are abstract in RoR2's `mscorlib` and `mono_runtime_invoke` doesn't
    dispatch virtually, so the first managed probe couldn't have worked (it never ran). Viewfinder's and Deliver's
    `GameAssembly.dll` export the native reflection (`il2cpp_class_get_fields`, `il2cpp_field_get_offset`/`_flags`/`_type`,
    `il2cpp_type_get_type`, `il2cpp_class_from_type`, `il2cpp_gchandle_*`, ...), only `il2cpp_array_addr_with_size` is missing.
    In `RoR2.dll`, `PitchYawPair` is a value type and `InstanceData` a class, so the shipped profile's writes reach the game.
  - Engine fixes: `g_follow.ignored` never ended and skipped pause/cursor; the follow check compared yaw plus pitch to yaw
    moved (looking up/down alone counted as ignored); the pitch clamp read back in the same frame as the write (now next
    frame, 3 in a row, `limit_turn`); `PitchLimit=` didn't apply to profiles; a profile value was copied into 8 bytes with no
    type check (a `Vector3` overwrote the stack); a destroyed object's wrapper still read valid (now `m_CachedPtr` and a weak
    GC handle); `Cursor.visible` ignored `lockState`; `resolve_api` was all-or-nothing and permanent (now only Camera and
    Transform required, retried 30 s); one log line per frame on an exception. `EngineFrame::no_aim_camera` is gone.
  - Fixed-camera verdict measures motion between the end of the mod's write and the next frame, so the gyro's own writes no
    longer count (before: anything under ~37 deg/s at 75 FPS did). Window dropped if pause/cursor/focus/panel closed it. Look
    input needs 300 ms; pitch counts. **Open:** it still needs stick or mouse input, which a pad player may never give in
    Deliver, so the gyro keeps turning the scene camera until they do.
  - New probe (`unity_probe.cpp`): floats of all active non-Unity scripts (value types, one object reference deep) through
    native reflection; picks the camera that turns with stick/mouse; correlates each float with yaw/pitch over 100 ms samples
    (also gives units per degree); live test of the best 12 per axis; writes `[Unity]` and applies at once. New profile keys
    `PitchPath`, `YawPath`, `YawObjectType`, `PitchPerDegree`, `YawPerDegree`; `CameraField` optional. `Probe=`, `ProbeKey`
    as in Unreal (`core/probe_key.hpp`), `AutoProbe` when the camera ignores writes or the stick fallback lasts over 3 s.
  - Not done: known candidates before `Camera.main` (Cinemachine, Starter Assets, Standard Assets), choosing among cameras,
    yaw around the rig's up axis (likely cause of Viewfinder's turned world), an automatic zoom reference FOV.

- 2026-11-06 **Unity Mono/IL2CPP module, first implementation (build and unit tests only; 866 checks).** Detects
  `UnityPlayer.dll`, resolves the loaded Mono (`mono-2.0-bdwgc.dll`, `mono-2.0-sgen.dll`, `mono.dll`) or IL2CPP
  (`GameAssembly.dll`) exports on the game thread. Both enumerate assemblies and invoke `Camera.get_main`,
  `Component.get_transform`, `Transform.get_rotation`/`set_rotation`, `Camera.get_fieldOfView`/`get_orthographic`,
  `Time.get_timeScale`, `Cursor.get_visible`. Generic turn = world-Y yaw * current quaternion * local-X pitch (negative X is
  up), roll preserved. Missing exports or a camera ignoring writes -> right-stick fallback.
  - Risk of Rain 2 (Unity 2021.3.45f2 Mono): `UnityCrashHandler*` is a helper (`is_launcher`); the message pump is in
    `UnityPlayer.dll` so `module_prefix` is `Unity`. `Camera.main` moves a UI/crosshair camera, not the aim. In `RoR2.dll`
    `CameraRigController.sceneCam` is the displayed camera and `LateUpdate` rebuilds its Transform; the look input is
    `CameraModes.CameraModePlayerBasic.InstanceData.pitchYaw` via `_cameraMode.DebugGetInstanceData(@).pitchYaw` (pitch down
    positive, yaw right positive). Added `[Unity]` ObjectType/CameraField/PitchYawPath, managed field access and the shipped
    RoR2 profile. Method lookup walks base classes; Mono/IL2CPP field getters copy a value type into caller storage (no box),
    so the profile copies the two-float PitchYawPair directly. **User confirms gyro aiming works.** Zoom compensation reads
    `sceneCam`'s vertical FOV. NativeFlickStick enabled (flick uses the same pitchYaw path; restart required).
  - Deploy script recognizes the Unity exe by its `_Data` folder (not `unins000.exe` by size).
  - Rewired (decompiled) P/Invokes `CreateFileW`, `ReadFile`, `ReadFileEx` in `Rewired_Windows.dll`, bypassing UnityPlayer's
    import table: native-pad tracking now uses function hooks in normal games too (GyroLib/SDL excluded by caller) and wraps
    ReadFileEx's completion routine so its report is edited before managed code sees it; Raw Input gets the same panel/flick
    edits. Targets the reported panel input leak and native vertical look; needs the in-game retest.
  - First IL2CPP game (Viewfinder): `unity=failed` because `GameAssembly.dll` doesn't export `il2cpp_array_addr_with_size`
    (the other 16 required are there). Optional now, with an x64 `Il2CppArray` fallback (first written wrong, fixed in the
    2026-11-07 Viewfinder entry). Deliver At All Costs has the same export set.
  - Viewfinder: gyro turns the camera but pitch had no limit (the game's controller clamps; the Transform write bypasses it).
    The generic view now learns the clamp (a pitch write that comes back pulled toward zero; starts at +-89;
    `unity: learnt the game's pitch clamp`); `PitchLimit=` pins it. To re-verify: aim past where the mouse stops.

- 2026-11-10 **RDR2 "drift", reporter's third report: "almost always, very noticeable" after switching to defaults, "random and
  barely noticeable" with their backed-up files.** The backup holds an older `winmm.dll` (an earlier build also labelled 1.10.1,
  size differs from the 1.10.1 release zip: the log line can't tell the two apart), their `gyrolib.ini` and the game's
  `system.xml`. Not established which "defaults" they mean (Gyro4All files, the game's settings, or both).
  - Their `gyrolib.ini`: Hold on L2 (`gyro.activation=3`, `activation.trigger=1`, `activation.trigger_threshold=0`), smoothing
    0, tightening 0, sensitivity 2, `calibration.automatic=2`, and a saved manual calibration
    (`calibration.device.<id>.manual`, bias about 0.49 -0.16 0.06). A fresh `gyrolib.ini` has no manual reference: GyroLib then
    learns only under the stricter limits (mean under 3 deg/s, noise under 0.9 deg/s RMS) and, during active use, only within
    0.15 deg/s, so a pad that is always hand-held may keep a wrong bias. That fits "almost always", unconfirmed.
  - Their trace (backup setup, old build, no B lines): applied against raw fits r > 0.999, gain 2.0, zero points x 0.517,
    y -0.028, matching the logged bias (0.48-0.53, -0.03). The scan finds only short slides of 0.2-0.5 deg, all `pad reads`.
    The pad's own zero moves between sessions: y -0.14 and z +0.09 on 2026-11-06, y -0.03 and z -0.09 here.
  - `system.xml` differs only in graphics and display (DLSS, Reflex On + Boost, 160 Hz, HDR off); nothing in it reaches the gyro
    path. The game's targeting and lock-on settings live elsewhere, and their defaults (aim assist on) would pull the aim while
    L2 is held: still the main game-side suspect if "defaults" meant the game's.
  - A trace shows what Gyro4All asked of the camera, not where the camera went: the RAGE module has no `view_angles`, so the
    game's own turns (aim assist) can't be separated from the gyro's yet.
- 2026-11-06 **RDR2 "drift", reporter's update and a long-session trace.** Rare and random, usually after sudden movements,
  "as if the gyro occasionally leaks"; "menus only" didn't help (on RAGE a saved "menus only" becomes "any time" at the
  next start, `host.cpp`), switching back to "any time" it almost disappeared. A bias knocked off by a sudden movement is
  open again (the trace below had no such event). Added to the trace: `B` lines (GyroLib bias, calibration state, still flag,
  gate, 10 Hz) and `M` lines (F9, with a notice); `tools/motion_trace_drift.py` lists bias changes, slides labelled `bias off`
  or `pad reads`, the time since the last fast motion, marks. Only checked on the older trace and unit tests.
- 2026-11-06 **RDR2 "drift" on L2 hold: the reporter's `MotionTrace=60` shows the turn comes from the pad's own motion, cut
  by the Hold gate; learned bias, GyroLib and sensor pacing add nothing.** 1.10.1, DualSense 1000 samples/s, no Steam
  Input; 8 presses in 60 s (first deliberate, left out; 3 had a jerk before the gate opened, 4 none). No code changed.
  - Applied vs raw gyro over 500 ms windows: r > 0.998, pitch gain 2.0, yaw 2.5, zero points x 0.534 y -0.141 = the logged
    bias; totals agree to 0.002 deg; same gain at low rates (no tightening active).
  - The jerk: peak 12-15 deg/s, ~2 deg of pad pitch in the 400 ms before the gate on two presses (0.7 on the third), gate
    opening at its end (much smaller than the simulation below assumed). Edge bump (first 150 ms): with a jerk pitch up to
    0.62 and yaw up to 1.41 camera deg, without at most 0.07. Slide while held: with a jerk pitch +0.18..+0.54 deg/s, yaw
    -0.15..-0.19 (0.6-2.0 deg per press), without +0.05..+0.12 / ~0; always the opposite sign of the jerk, fading over
    seconds: most likely what the reporter calls drift.
  - Reading: the pad tilts during the pull with the gate closed, then creeps back while open, so only the return reaches the
    camera. Hand settling vs sensor effect can't be told (no accelerometer lines). A ramp on the gate would remove the bump,
    not the slide. Next: reporter tests with the pad on a table and `gyro.tightening_dps` ~0.5-1 (needs smoothing on); a
    lower trigger threshold should open the gate before the tilt. Their `gyrolib.ini` is still missing.
- 2026-11-05 **RDR2: "drift" while aiming with the gyro on L2 hold, worse on a quick pull (reporter); not reproduced by the
  user, cause not confirmed.** 1.10.1, Steam, DualSense 1000 samples/s, 60 FPS, no stick fallback. No code changed.
  - **Sensor pacing ruled out:** the real `sensor_pacer.cpp` over a simulated jerk out and back (40-300 deg/s peak, 30 ms
    each way, 1 kHz, 60 FPS, tremor 0-3 deg/s, all phases): at most 0.05 deg vs GyroLib's turn.
  - **Candidate 1, the activation edge (fits quick vs slow):** Hold opens the gate at the trigger threshold, mid-jerk, so
    part of a net-zero motion gets through the same way each press (simulated 0.2-0.75 deg at 40 deg/s peak, 1.6-5.6 at 300;
    jerk assumed). Possible fix: a short ramp after the gate opens. (Later trace: the jerk is much smaller; see above.)
  - **Candidate 2, the game's lock-on** (test with Lock-On Mode = Free Aim). **Candidate 3, bias learned from the hand-held
    pad:** `sees_menus()` is false on RAGE so "menus only" becomes "any time"; GyroLib read the pad as still during a hold
    and bias x went 0.55 -> 0.43 deg/s between status lines (GyroLib: 0.15 deg/s and 8 s constant in active use, 2 s
    otherwise; whether a suspended Hold counts isn't documented; the host never calls `gl_set_auto_calibration_allowed`).
    Ruled out for the later trace.

- 2026-11-05 **007 First Light: the L2 stall after a long session is a transition camera on a recycled entity, not the
  camera list; list check and more logs added** (853 checks; **not tested in game**). Reporter (1.10.0, ~90 min, DualSense
  USB/basic BT, no Steam Input): gyro dead at the start of each L2 aim from 23:36 on, fine on L1, the same aim works until
  L2 is released.
  - The 2026-11-03 hypothesis (camera list longer than the 64-entry buffer) is **refuted**: all 12 logged transitions had `4
    cameras in the list`, no failed transition in the session.
  - Log: for the first hour the transition camera is entity 0xD5 (no orbit state, `turn_all` runs). From 23:36 the same
    camera comes up as `orbit camera 13070112`: entity 0x112 (generation 07 to 28, a new one on nearly every press; also
    0x114), ~500 ms per press. Those stretches asked 371.7 deg and the view moved 17.7; 15 of 17 lines `NOT FOLLOWED`. The aim
    camera 1304006B right after follows (178.8 asked, 202 moved). Entity 0x105 was a transition camera in an earlier session
    and an orbit camera in this one. Mouse/keyboard mode ruled out (input 0 on all 27 lines).
  - Cause (inferred): `orbit_of` only checks that the entity's state and limits look valid. The rig with limits -85..85
    appears at 23:12 (0x103, 0x105, 0x10F, 0x111, 0x112, 0x114); component arrays seem to keep values when an entity is
    freed, so a transition camera that gets one of those entities finds a valid-looking state, the write lands in a slot
    nothing reads ("100% turned"), and `turn_all` never runs. Depends on which entity the camera gets.
  - Fix (`glacier_look.cpp`): an active camera with a state but missing from the orbit update's list is treated as a
    transition camera: `turn_all` runs and its own state and point are still turned. A hedge: the list held 4 entries while 5
    different cameras had been active, so not proven that every active orbit camera is listed (then it keeps turning and the
    list's cameras turn along). An empty/unreadable list says nothing (old behaviour). Static check on a local build with a
    different update address (exe+0x52DEC0 vs reporter's exe+0x52DDE0): one thunk, a virtual method, so the list belongs to
    one system object.
  - New log lines: `glacier: gyro press for N ms ...` (first 20 presses and any 250 ms+ before the view responded, up to 300);
    `glacier: camera <handle> has an orbit state the orbit update doesn't use` (once per entity); `glacier: orbit update's
    list, new set: ...`; `the orbit update's list object changed` (up to 20). The transition line gains `on an old orbit
    state`, `(ended N ms ago)`, duration, follow check, list entities (first 12 and any with unturned frames / old state /
    unfollowed view, up to 300); `orbit camera` line gets `in the list`/`list unknown`; `NOT FOLLOWED` up to 200. The 30 s
    status adds gyro frames by kind, degrees asked vs moved, presses and slow presses, list length.
  - Check in the test log: `old state` frames now `followed`; any long aim-camera stretch `on an old orbit state` (then the
    list misses aim cameras); any `gyro press` still slow on plain `orbit` cameras (a further cause).
- 2026-11-05 **RDR2 (Steam): Gyro4All ran in the launcher `PlayRDR2.exe`, never in `RDR2.exe`; launchers now hand the session
  over with any engine, also when the game starts through another program** (853 checks; verified with a fake launcher,
  `bash tools/launchertest/run.sh`; real game not yet). User log: only `Gyro4All 1.10.0 in ...\PlayRDR2.exe`, `engine: RAGE
  (detected)` (RAGE detection looks at the folder: `common_0.rpf`, `shaders_x64.rpf`), `rage: PlayRDR2.exe is not the known
  RDR2 build`. On Steam PlayRDR2.exe starts the Rockstar Games Launcher (another folder), which starts RDR2.exe later; a
  non-Steam RDR2.exe does the same itself. Causes in `dllmain.cpp`: (1) engines with their own frame thread
  (`Engine::frame_source`, RAGE) returned before the session handle was kept and the CreateProcess hook installed; (2) the
  CreateProcess handover only covers an exe of the same folder started by this process, only before the first gyro frame.
  Fix: session and CreateProcess hook set up before `frame_source`; the handover runs while this process has never turned a
  camera; plus a takeover window: the session owner keeps a shared-memory block (`Local\Gyro4All-session-<folder hash>`:
  state, ack, owner pid, deadline) open for 180 s (`kLauncherHandoverSeconds`, no setting). A later game exe of the folder
  (engine detects it, not a known launcher) that finds the session taken claims it while open; the owner stops at its next
  gyro frame (log/session handles closed, ack; GyroLib stays loaded, no longer polled, because destroying it could leave the
  XInput hooks' pad pointers dangling); the new process waits up to 3 s for the ack. The window closes for good when the
  owner turns a camera. Fake launcher: both runs (direct, and through `relay\Relay.exe` in another folder) end in
  `Gyro4All 1.10.1 in ...\RDR2.exe`, the second with `session: taken over ...`. Risk: a tool of the game's folder that an
  engine detects, started within 180 s while the game is in its menus, takes the session.
  - Then on Steam (`RDR2.exe` 1.0.1491.50): `rage: RDR2.exe is not the known RDR2 build` 7 s after start. Read-only scan:
    director `[exe+0x3ea05e8]` (camera `+0x7a0`, vtable `exe+0x339a6c0`, unit front `+0x7b0`), zoom vtables `exe+0x3399308` /
    `exe+0x3399c08`, projection `exe+0x3ec0aa4`: same data offsets as the Rockstar copy. First guess (check ran before the
    anti-tamper unpacked) -> check retried every 500 ms (note after 60 s); still failed. Real cause: `build_ok()` read the
    vtables' first entry with `mem::rdp()`, which requires 8-byte alignment, and the Steam copy's aim vtable starts with
    `exe+0x41e88c` (read as 0). Fix: `mem::rdp(vtable, 1)`. Tested in game (Steam): "matches the known RDR2 build after 0.0
    s", camera found, gyro turns it (99% of updates), zoom compensation on aim and binoculars part of the time.
  - Same session: stick fallback started three times in 18 s ("the game undid the gyro's camera writes 8 frames in a row",
    cause not in the log, possibly binoculars). Each saved `StickFallbackLearned`, the last from 0.5 s of data after a 97%
    model was an 80% one (curve 3.00). Fix (`stick_fallback.cpp`, `take_save`): save only if the fit is at least the one
    saved earlier this session minus 0.02 (a model restored from the profile has no fit, so the first good one replaces it).
    853 checks; not tested in game. Open: why the game undoes the writes there.
  - Log: the 10 s `xinput: sticks in the last 10 s ...` line (`xinput.cpp`) filled logs with idle copies; now written the
    first time and when the game starts/stops asking for our slot, otherwise at most once a minute and only when something
    happened (stick axis range over 1000, a button, or the gyro's stick).

- 2026-11-05 **1.10.1 — Cyberpunk 2077 frame rate, RE4 remake crash with the DX12 panel** (MSVC 14.51, 853 checks;
  `Gyro4All-v1.10.1.zip`). Since 1.10.0: REDengine safe-read page check uses `QueryWorkingSetEx` instead of `VirtualQuery`
  and the camera system is kept between frames (Gyro4All's frame 9-12 ms growing -> ~120 us); a camera that matched last
  frame keeps matching within ~20 degrees, so fast turns no longer flip to "cinematic" for a frame (both user-tested). DX12
  panel: the captured swapchain gets a private vtable (RE4 crash below).
- 2026-11-04 **RE4 remake: crash ~10 s after start with REFramework and the DX12 panel on; the captured swapchain now gets a
  private vtable** (853 checks; standalone D3D12 test; verified in game by the user). re4.exe 1.5.9.0 access violation with
  REFramework (own and official nightly), log ending after "DX12 panel ready". Runs without Gyro4All, with the panel off,
  and with OptiScaler as `dxgi.dll`. Panel on: Steam overlay off + REFramework crashes; overlay on + no REFramework runs.
  - Cause (four WER minidumps, cdb): stack overflow on the present thread (reported as an access violation in `nvwgf2umx`):
    thousands of `hk_present` -> Steam's Present hook (`gameoverlayrenderer64!OverlayHookD3D3+0x1440..`) -> `hk_present`.
    Steam's overlay DLL is loaded even with its overlay off; it hooks the Present of every new swapchain, keeping one saved
    original. REFramework hooks D3D12 ~10 s in with a dummy device/factory/swapchain; that creation bypassed our factory
    hooks (no "set aside" line), Steam read dxgi's shared swapchain vtable, found our `hk_present` in slot 8 and took it as
    its original; dxgi's Present (inline-hooked by Steam) and ours then called each other. With OptiScaler's `dxgi.dll` the
    swapchain is its wrapper with its own vtable: no loop.
  - Fix: `com::make_private` gives the captured DX12 swapchain its own copy of its vtable with our hooks
    (`overlay::hook_dx12_swapchain`; the copy runs to the end of the code-pointer run: 357 entries + the RTTI entry). dxgi's
    shared vtable is no longer patched for DX12. The hooks call whatever the shared slot holds at call time (another hooker
    still runs; if the slot holds our own hook too, they call the saved original). The D3D10/11 path (`on_other_swapchain`)
    still patches the shared vtable with the creation set-aside; if no copy can be made the shared vtable is patched as
    before (logged). A second swapchain keeps the untouched shared vtable.
  - Dead ends (both crashed the same way in game): (1) when our hook is re-entered from inside its own original on the same
    thread, call the slot's first saved original (that original led back into Steam's hook); (2) call dxgi's real Present
    (slot value from `dxgi.dll` on disk relocated, 25/25 slots matched): Steam hooks the code of dxgi's Present itself, its
    saved original was our hook, so even the real function looped. (Minidumps don't hold code pages, so `u` showed the file's
    unhooked bytes.)
  - First private-vtable build: no loop, but one start froze (REFramework suspending threads) and the next crashed ~27 s in
    (`reframework_crash.dmp`: RIP 0 from `hk_set_color_space1`). REFramework had pointed the swapchain to its own copy of our
    private vtable (logged 3 ms after "Hooked DirectX 12"), so `original_of` didn't recognise the object and called the unset
    fallback `g_set_color_space1`; our Present also used its fallback, skipping REFramework's pointer hook on the shared slot,
    so REFramework re-hooked D3D12 every 10 s. Fix: `com::private_objects` (object -> shared vtable) so `original_of` finds
    the shared vtable when the object's vtable is someone else's copy; `com::slot_hooks` says whether a shared slot holds
    our hook; every fallback set from the shared vtable; a swap logged once ("another module replaced the captured
    swapchain's vtable"). Modules hooking at the time: `RTSSHooks64`, `nvspcap64`, REFramework, Steam's overlay.
  - In game (panel on, REFramework, Steam overlay on): responding 30 s after start, main menu reached, our panel drawn, no
    second re-hook; user confirmed gameplay, F10 panel and REFramework's menu work together. Cyberpunk 2077 (GOG, frame
    generation on) also fine: "hooked through a private vtable", 4K ResizeBuffers, HDR colour space 12, no swap line. To
    retest: a UE5 game with Streamline frame generation, 007 First Light, a game with ReShade.
  - Debugging: our PDB is stored as `%%%winmm.pdb%%%`; copy `winmm.pdb` under that name, `.symopt+ 0x40; .reload /f /i
    winmm.dll`. WER minidumps include module data sections (our globals) but no heap.

- 2026-11-03 **007 First Light: the gyro sometimes stops while aiming (L2); per-aim diagnostics added** (853 checks; not tested
  in game). User log (1.9.0, DualSense USB, GyroLib trigger activation on L2): no gate problem, motion at 1000 samples/s,
  every status block "100% turned". But "turned" only means the write landed: `turn_camera` reported success even when
  `turn_point` skipped the aim camera's point (unreadable, < 0.1 m, > 1e12 m), and nothing checked that the view followed.
  Reporter: with the mouse cursor over the pause menu the game switches to its keyboard layout. The game reads the mouse
  through DirectInput (non-exclusive, background). Hypothesis (unconfirmed): in keyboard-and-mouse mode the aim camera takes
  its aim from the mouse and rewrites the gyro's point.
  - New line per stretch of gyro turns on one active camera (`glacier_look.cpp`, `aim_frame`/`aim_turned`/`aim_end`):
    `glacier: aim|camera <handle> for N ms ...: N gyro frames; point turned/none/unreadable/near/far, rewritten by the game
    N; asked yaw/pitch, view moved yaw/pitch over N frame pairs: followed|NOT FOLLOWED|too little turn to judge; game mouse
    input N, keyboard N, cursor moved N`. The follow check compares the derived view's movement between consecutive gyro
    frames (<= 100 ms apart) with the previous frame's turn; judged from 2 degrees and 10 pairs, NOT FOLLOWED below 50%.
    "Rewritten": the point differs from the one written last frame by more than 1e-3 of its size. Aim lines: first 40;
    NOT FOLLOWED: up to 50. A stretch ends on a camera change, a transition, or 500 ms without a turn.
  - `dinputpad::desktop_input` counts the game's DirectInput mouse/keyboard reads with motion or something held. Camera line
    fix: "once per camera" kept 16 full handles (generation changes on every switch) so it spammed after 16; now 64 entities.
  - **Reporter: the gyro is dead for the first 1-2 s of every L2 aim, then works until release; L1 fine.** That is the zoom's
    transition camera (no orbit state). `turn_all` copied the orbit update's camera list into a 64-entry buffer and turned
    nothing when longer. Fix: up to 65536 entries (static buffer); the transition line now also comes for every transition
    whose frames didn't all turn (up to 50), with the list length. **Refuted by the reporter's log, see 2026-11-05.**
- 2026-11-03 **Halo: The Master Chief Collection (Xbox app, package 1.3528.0.0)**: plan in `games/HaloMCC/MODDING_PLAN.md`.
  `MCCWinStore-Win64-Shipping.exe` is a UE4 shell; each game is a Blam! DLL (`halo1\halo1.dll` ... `halo4\halo4.dll`) loaded
  into the same process. `winmm.dll` loads, XInput translation reaches Halo: Reach, but the Unreal module saw
  `MCCPlayerController` with a `DefaultPawn` ("gyro off: no gameplay view"). New module `src/engines/halo/`: detected by the
  game DLL folders four levels above the exe, ahead of Unreal (the fallback is now `fallback_engine()`, still Unreal); no
  camera hook, so the gyro goes to every game as right stick (blind stick fallback); menus by the mouse cursor only. User
  test: gyro turns Halo CE as right stick, launched via the `HaloMCCShippingNoEAC` entry
  (`explorer.exe shell:AppsFolder\Microsoft.Chelan_8wekyb3d8bbwe!HaloMCCShippingNoEAC`). The normal (EAC) entry fails at start
  with the proxy installed (no log).
- 2026-11-03 **Halo CE (`halo1.dll`, build 2025.08.16) view found statically.** Live searches failed (Z-up engine; copies were
  the camera's up vector / reused buffers; `guided.py` gained `--up z` and `--module <dll>`, camtrace `Proc.add_module`). The
  script functions `get_yaw_rate`/`set_yaw_rate` (via the `hs` function table) read per-player look rates at RVA `0x2ea0610`
  (stride `0x8a0`: yaw 120, pitch 60 deg/s). Input update `+0xa97cf4` turns stick x rate x tick into a per-tick action (yaw
  `+0xc`, pitch `+0x10`, radians); player-control update `+0xa99c1c` adds it: array pointer at `+0x2d8fe70` (`mov rdi,[rip+..]`
  at `+0xa99c3d`), stride `0x58`, unit handle `+0x10`, **desired yaw `+0x1c`** (radians, wrapped 2 pi), desired pitch `+0x20`
  (clamped +-1.49226). Write test +0.349 rad on yaw kept and the view turned: the source. Module finds the pointer by the
  update's `mov rdi,[rip+..]` (`48 8B 3D ?? ?? ?? ?? 33 F6 4C 8B 0D ?? ?? ?? ?? 4C 8B 15 ?? ?? ?? ?? 0F 29 70 B8`, one
  match) when `halo1.dll` loads; writes entry 0 (gyro yaw right = Halo yaw down), only while it has a unit (`+0x10` != -1).
  The string `player control globals` has no code reference here.
- 2026-11-03 **MCC's other six games: the same player control, found statically.** The string `player control globals` names the
  game-state allocation (Halo 2: `0x2f8` bytes in a global; Halo 3+: pointer in the DLL's thread-local block, `gs:[0x58]` ->
  `[_tls_index]` -> slot); every update loads the pitch clamp +-1.49226 and does `yaw += delta`, wraps by 2 pi, pitch at yaw+4.
  Entry 0 (first local player):

  | DLL | globals' pointer | entry stride | unit | desired yaw / pitch | update |
  |---|---|---|---|---|---|
  | `halo1.dll` | global `+0x2d8fe70` | `0x58` | `+0x10` | `+0x1c` / `+0x20` | `+0xa99c1c` |
  | `halo2.dll` | global `+0x15e40d0` | `0xb8` | `+0x18` | `+0x38` / `+0x3c` | `+0x6c3df0` |
  | `groundhog.dll` (H2A multiplayer) | TLS slot `0xb8` | `0x200` | `+0x120` | `+0x138` / `+0x13c` | `+0xa78d4` |
  | `halo3.dll` | TLS slot `0xc0` | `0x110` | `+0xc0` | `+0xd4` / `+0xd8` | `+0xf7df0` |
  | `halo3odst.dll` | TLS slot `0xd8` | `0x2c0` | `+0xc0` | `+0xd4` / `+0xd8` | `+0x113ab8` |
  | `haloreach.dll` | TLS slot `0xd8` | `0x180` | `+0x80` | `+0x94` / `+0x98` | `+0x6375c` |
  | `halo4.dll` | TLS slot `0xb0` | `0x1f0` | `+0x120` | `+0x138` / `+0x13c` | `+0xa8b34` |

  One table in the module (`kGames`): per game a signature of the update (globals load to entry 0's unit read) and one of its
  `yaw += delta` (one match per DLL, rip/branch displacements wildcarded). For TLS games the index comes from the DLL's TLS
  directory and the pointer from scanning every thread's TEB (`+0x58`, via `NtQueryInformationThread`), at most once a second
  while there is no player. First run (Halo CE): the globals pointer is only 4-byte aligned (`...484`) and `mem::rdp`'s
  8-byte check returned 0; with the player missing the module reported a menu, so neither gyro nor stick fallback ran. Now
  read with 4-byte alignment; user confirmed the gyro turns Halo CE (USB, Steam Input off). Halo 2 confirmed 2026-11-04.
- 2026-11-03/04 **MCC FOV (read-only scans with sound cues; zoom scan = snapshots not zoomed, zoomed, not zoomed again, each
  read twice a second apart and required stable while still).** MCC's FOV settings (first/third person, up to 120 deg) are
  per game (ODST showed 83 while Halo 3 was at 100). Each game's module finds the camera FOV, the zoom reference and the
  settings by code signatures (one match each); `ZoomReferenceFOV=` (horizontal degrees) wins, else the setting for the
  camera in use (third-person setting applies while the camera FOV equals it exactly, last mode kept through a zoom). A
  first version using the widest FOV held for 1 s was dropped (after a vehicle with wider FOV, walking read as zoomed).
  - **Halo CE:** FOV 1.36136 rad (78 deg, the default) -> 0.68068 with the 2x zoom: the game divides the angle by the
    magnification (no tan). Rendered camera `halo1.dll+0x2d9b9c0`: position `+0x4`, **FOV `+0x20`**, forward `+0x24`, up
    `+0x30`; written by the camera update `+0xb14c48` (`lea rcx,[rip+..]` at `+0xb14c6b`; signature `4C 8B C2 0F 29 7C 24 30
    48 8B FA 0F 28 FB 48 8B D1 48 8D 0D ?? ?? ?? ?? E8`). Settings (radians) at `+0x2b32cdc` (first) / `+0x2b32ce0`
    (third), found by the static initializer that clears both (`44 21 35 ?? ?? ?? ?? 44 8D 43 F8 44 21 35 ?? ?? ?? ?? 48 8D
    0D ?? ?? ?? ?? 33 D2 4C 89 35 ?? ?? ?? ?? 4C 89 35 ?? ?? ?? ??`). Taken as horizontal (not confirmed by moving the
    slider). Unverified: vehicle camera uses the third-person setting.
  - **Halo 2:** 83 deg = 1.44862 rad -> 0.48287 (a third), so also divides by magnification. Player cameras at
    `halo2.dll+0x15f2978`, stride `0x368`, 4 players, set by `+0x6f1a60` (`lea r14,[rip+..]` at `+0x6f1a7d`). In a camera:
    active byte `+0`, position `+0x4`, **FOV `+0x3c`** (horizontal radians), `+0x40` 1.3333, `+0x50` 1.1717, **zoom scale
    `+0x54`** (1/magnification). Signature `48 81 EC A0 00 00 00 F3 0F 10 0D ?? ?? ?? ?? 4C 8D 35 ?? ?? ?? ?? F3 0F 59 C8 48
    8D 2D ?? ?? ?? ?? 33 F6` (lea at +15). Reference = `ZoomReferenceFOV=` or current FOV / zoom scale (any unzoomed FOV
    is its own reference). Tested in game.
  - **Halo 3:** settings in degrees at `+0x2d3ede4`/`+0x2d3ede8` (`+0x74`/`+0x78` of player 0's profile at `+0x2d3ed70`; 4
    profiles of `0xd78`, constructor `+0x6f8de8`). Render camera `+0x2d2f688`, followed by two FOVs: `+0x2d2f6b0`
    0.91977 -> 0.28672, **`+0x2d2f6b4`** 0.93079 -> 0.31026 (exactly a third). With the setting at 100 (3840x2160)
    `+0x2d2f6b4` read 1.1214286: linear, setting x 9/14 in degrees with pi = 3.14, taken as the **vertical** FOV (same at
    1600x1200: Hor+, ignores aspect); `+0x2d2f6b0` read 1.14514 (unexplained, unused). When paused/in a menu at 2142x1231
    `+0x2d2f6b8` reads 0.0 instead of 1.0. Camera: `lea rcx` to `+0x2d2f688` at `+0x24cc12` (`48 8D 0D ?? ?? ?? ?? 48 03 D3
    E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 49 81 C0 A0 00 00 00 48 03 D3 0F 57 DB`),
    FOV `+0x2c`; settings via the profile constructor (`48 83 EC 38 48 C7 44 24 20 FE FF FF FF 4C 8D 0D ?? ?? ?? ?? BA 78
    0D 00 00 41 B8 04 00 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 83 C4 38 C3`, lea at +31). `Game` fields
    `fov_settings_at`, `fov_settings_scale` (3.14/280), `fov_vertical`; settings match the camera within 1e-4.
  - **Halo 3: ODST:** neither Halo 3 signature matches. 83 -> `+0x2d735c4` 0.9307858 -> 0.46539 (half), same 9/14 formula,
    other FOV at `+0x2d735c0`. 4 per-player views at `+0x2d73590`, stride `0x2810`, set up by `+0x6b60` (`48 89 5C 24 08 57
    48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 10 28 00 00 48 83 EF 01`, lea at +10),
    FOV `+0x34`. Profiles: 4 of `0xeb0` at `+0x2d81860`, settings at `+0x78` (Halo 3's constructor pattern with size `B0 0E`).
    The same view-setup pattern with wildcarded stride also matches Halo 3 (`+0x2d2f680`, stride `0x2820`).
  - **Halo: Reach:** settings (degrees) `+0x2a03cdc` (`+0x8c` into profiles at `+0x2a03c50`, set up by `+0x7aa0`). 3x zoom scan:
    `+0xc9fb08` (copy `+0xc9fc5c`) 0.91977 -> 0.28672 and `+0x291c77c` 1.06949 -> 0.33339; ratio 0.3117, not a third. At
    setting 100 `+0xc9fb08` read 1.1451385 = Halo 3's other FOV at the same setting/zoom, so both share one function of v
    (setting x 9/14 / magnification); the zoom ratio equals tan(v/2)'s (0.3114). Fit v' = 2 atan(a tan(b v)), a = 0.6725665,
    b = 0.6814241 (exact at 100 and zoomed, 0.08% off at 83; true formula unknown). Module: view at `+0xc9fae0` (`lea rcx`
    at `+0xc3729`, `49 8D 97 D0 F5 FF FF 45 8B C6 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 8B 2D ?? ?? ?? ??`), FOV `+0x28`
    converted back with the fit (`fov_fit_a`/`_b`); settings via the profiles' setup (`48 83 EC 40 48 C7 44 24 30 FE FF FF
    FF 48 89 5C 24 58 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8`, lea at +18) then `+0x8c`; match tolerance 0.2%.
  - **Halo 4:** settings `+0x30ebf6c` (`+0x9c` into profiles at `+0x30ebed0`, stride `0xcc0`; Reach's setup pattern). View at
    `+0x10dafe0`, FOV `+0x28` (0.91977 -> 0.28672 at `+0x10db008`), found by a prologue (`48 83 EC 50 48 8B D9 0F 29 74 24
    40 4C 8D 2D ?? ?? ?? ?? 49 63 E8 49 8B CD 48 63 F2 45 8B F9`, lea at +12). Same fit and scale as Reach.
  - **Halo 2: Anniversary multiplayer (`groundhog.dll`, Halo 4's engine; H2A's campaign runs on `halo2.dll`):** gyro confirmed
    (offline custom game, USB, Steam Input off; globals found in one of two threads' TLS blocks). Settings `+0x1b1f4f4` (`+0xa4`
    into profiles at `+0x1b1f450`). Reach's transformed FOV at `+0x10cd108` 0.91977 -> 0.43466 (fit predicts 0.43461 for v/2);
    a horizontal FOV at `+0x11ec68c` 1.44862 -> 0.72431 (no usable code reference). Module uses the view at `+0x10cd0e0`
    (Halo 4's view pattern), `+0x28`, Reach's fit.
  - Halo CE/2 zoom tested in game; the other games' zoom compensation was builds only at the time.
- 2026-11-04 **Halo 4: fatal error on loading a checkpoint; gyro writes now made on the game's thread.** Loading a checkpoint
  showed MCC's "fatal error" (`UE4Minidump.dmp`, cdb): `int3` at `halo4.dll+0x3bda47` after "game_state: failed to verify hash
  for in-place gamestate - this is an unrecoverable error!" (checkpoint load `+0x607c8` -> in-place load `+0x49470` -> hash
  check `+0x49a60`; game state `0xae6000` bytes at the pointer in `+0x27c6090`, hash header at `[+0x27c60a8]+0x2d0ac`, copied
  by `+0x49df8`). Only `halo4.dll` and `groundhog.dll` have that string. The player-control globals are inside that state,
  and Gyro4All wrote them from MCC's main thread while the game runs on its own: a write between the copy and the hash check
  breaks it.
  - First fix dropped (user rejected: these games must keep direct writes): no writes + gyro as right stick.
  - Fix: `Game::write_in_update` (Halo 4, groundhog). The main thread queues the turn (radians, mutex); an inline MinHook hook
    at the start of the player-control update (found from the signature via `RtlLookupFunctionEntry`, chained entries
    followed: `halo4.dll+0xa8b34`, `groundhog.dll+0xa78d4`; both prologues `mov rax,rsp; mov [rax+10h],rbx; ...`, one direct
    caller) adds it to entry 0's yaw/pitch on the game's thread. The update takes floats in xmm, so the hook enters through
    a thunk saving rcx/rdx/r8/r9 and xmm0-3, then jumps to MinHook's trampoline. Removed on DLL unload
    (`LdrRegisterDllNotification`) and by `find_game` on any module change. `view_angles` returns false while hooked so the
    stick fallback can't take over (hook writes land next tick and would look undone).
  - Second report with the hook: same fatal error, same stack; the hooked update never ran, so **the mod wrote nothing before
    this crash**. **Confirmed by the user: the same save fails without Gyro4All** (`winmm.dll` renamed): the save was already
    damaged (broken hash written in an earlier session by the main-thread race). The hook stays to keep new saves intact.
    Still to test: the update runs on the game thread (log line), gyro turns, checkpoint reached with gyro on loads. An
    unrelated `halo3.dll` dump (`+0xbca3` -> `+0x269936`) not looked at.
- 2026-11-04 **MCC's pause menu found** (read-only; sound-cued scan, 5 phases, exe and game DLL writable sections, in Halo 2 then
  3). With MCC's pause menu open (pad, no cursor) the gyro kept turning the view. Bytes that followed the menu in both:
  `exe+0x3e4efef` (set 1 at `exe+0x1e8074`, 0 at `exe+0x1e8b41`, ~30 readers) and `exe+0x3e4efe3`. The function at
  `exe+0x1e1780` sets `+0x3e4efe3` to 1 when any shell UI flag (`+0x3e4efe2`, `+0x3e4efef`, `+0x3e4efe5`, `+0x3e4efe4`,
  `+0x3e4eff0`) is on or the count `+0x3e4efe8` > 0: the aggregate "the shell holds the game" flag. Signature from
  `exe+0x1e1789` (six `cmp byte ptr [rip+..],0` with `je`/`jne`, the `cmp dword`, `mov byte ptr [rip+..],0` at +0x3f and `...,1`;
  81 bytes, one match in `.text`). Module reads it on the first frame (`shell_paused`): `EngineFrame::paused` stops the gyro
  and drops turns queued for the game's thread. Confirmed in logs: gyro stops/returns with the menu in ODST and H2A
  multiplayer (which doesn't pause). The flag is also 1 on ODST's loading screen. Multiplayer and MCC's other overlays unchecked.
- 2026-11-04 **Halo 3: ODST black screen after resuming: not Gyro4All.** After resuming the campaign it loads, then stays black
  while MCC's menu works (the log showed the window losing focus to an `explorer.exe` XAML shell window). Same without
  Gyro4All (`winmm.dll` renamed away), also after reverting to checkpoint and restarting the mission.
- 2026-11-03 **1.10.0 — One panel on the controller at a time** (MSVC 14.51, 851 checks; tested by the user in a DX11 and a DX12
  game, 007 First Light the DX12 one). With the Gyro4All settings window expanded, the d-pad also moved GyroLib's panel behind
  it (in DX12 games GyroLib's DX12 frontend has its own ImGui context reading the pad through SDL; GyroLib 1.3.1 has no call
  to stop that). Now expanding the window closes GyroLib's panel (`gl_overlay_set_open(0)` in DX12, `gl_set_panel_open(0)`
  otherwise), folding reopens it (`overlay::sync_settings_window`, from the panel ABI 9 calls `settings_event` /
  `settings_expanded` / `close_settings`). The window then runs alone (frames, capture, Escape and 250 ms heartbeat kept;
  `gyro off: Gyro4All settings open`; pad idle for the game). The menu key closes everything; GyroLib's Back+Start opening its
  panel folds the window. The folded button moves gamepad focus into the window when clicked, is no longer navigable
  (`NoNav`), and a fold with no GyroLib window clears the nav focus (in DX12 it kept focus; Cross/A on any GyroLib row expanded
  the window again; pre-existing). Square/X is no longer fed to the Gyro4All window's ImGui (held, it opened ImGui's window
  switcher; GyroLib's own DX12 context still does, titled "Untitled": GyroLib's to change).
- 2026-11-02 **1.10.0 — Zoom compensation: ZoomStrength, manual zoom levels, live ZoomReferenceFOV** (MSVC 14.51, 851 checks;
  `Gyro4All-v1.10.0.zip`; tested in game). Reviewed draft PR #13 (MERLev, Mass Effect 3 LE; written pre-1.9.0, duplicated what
  main had landed: `EngineFrame::fov` vs `zoom_fov`, a UE3 `FovPath` on the same `CameraCache` address, `offer_zoom` for every
  view, a vertical `ZoomReference` defaulting to 50.5 that made ME3 1.4x faster unzoomed). Taken: `ZoomStrength`, the zoom
  status line, its tests and the ME3 finding. Credit: MERLev.
  - `src/core/zoom.hpp` (header-only, unit-tested): `zoom_scale` (tan ratio ^ strength), `fov_for_scale` / `strengthened_fov`
    (GyroLib's formula is fixed, so the core reports the FOV that gives the wanted scale), `ZoomLevels` (aim Hold/Toggle, cycle
    with wrap, remember, reset), `ZoomRamp` (linear in log-scale, `ZoomRampMs`). The tan ratio is aspect-invariant.
  - `host.cpp`: `ZoomStrength=` (0..200, default 100) applies to the engine's FOV too; manual zoom (`ZoomAimButton`,
    `ZoomAimMode`, `ZoomCycleButton`, `ZoomLevelsFOV` horizontal degrees, `ZoomRememberLevel`, `ZoomResetButtons`,
    `ZoomRampMs`, `ZoomSource=Auto|Manual`; reference `ZoomReferenceFOV=` or 90) makes view 1 zoom-capable in any game
    (`sync_zoom_support`), all reloading on save (`load_zoom`). Logs `zoom: level N of M (F degrees)` / `not aiming`; status
    `status: zoom: <source> field of view ...`. A profile on the aim button works together with it.
  - `Engine::settings_changed()` (new, on save): UE3 and Glacier re-read `ZoomReferenceFOV` (they read it only in
    `configure()`); Unreal now also falls back to Gyro4All.ini's generic `ZoomReferenceFOV=`. `fov::game_window_aspect()`
    (core, cached 500 ms) replaces UE3's and Glacier's `window_aspect()` (an `EnumWindows` every frame). Panel: the
    `ZoomReferenceFOV` help said "vertical degrees" while every engine reads horizontal (fixed); new rows for all the above.
  - **Mass Effect 3 LE:** rendered unzoomed FOV is 100 horizontal (`Camera.CameraCache.POV.FOV`), `PlayerController.DefaultFOV`
    is 85 (main's automatic reference made unzoomed play ~1.3x faster with Adapt to Zoom). Profile `[Unreal3]
    ZoomReferenceFOV=100`. To verify: ME3 (strength 75), a game without FOV source with manual levels, live panel edits.
- 2026-11-02 **DX12 panel in HDR: device removal found and fixed (007 First Light, HDR10).** Summary of the hunt (all
  diagnostics removed; vendored ImGui DX12 backend byte-identical to GyroLib's copy again; verified: F10 opens without crash
  with the game started in HDR10):
  - **Root cause: zero `MipLevels` in the two composite SRVs.** Both explicit `D3D12_SHADER_RESOURCE_VIEW_DESC`s left
    `Texture2D.MipLevels` 0 (invalid); the device was removed (`0x887A0001`) right after creating them (the later
    font-texture/upload-buffer `0x887A0005` and the null dereference in the stock backend were secondary). Now `nullptr`
    like GyroLib's Composer.
  - Earlier suspects: the dxc-built DXIL composite shader (exonerated; the composite is now compiled at runtime with
    D3DCompile/DXBC exactly like GyroLib's composer, `overlay_hdr.hpp`; the old note "D3D12 can't consume DXBC" was wrong).
    Sync/lifetime bugs fixed to GyroLib's DX12 frontend model (`overlay.cpp`): unbounded fence waits (now a removed device fails
    at once, else `SetEventOnCompletion` + 5 s timeout, then the renderer disables itself: the game can't freeze on a device
    loss); no one-outstanding-submission rule (`wait_submissions()` before every submission); GPU objects released while in
    flight (`shutdown()` waits first, and skips cleanup if the wait fails on a healthy device); zero-init `FillMode` and
    `RenderTargetWriteMask` in the PSO desc; per-frame PSO-create retries (now failures are remembered per format/size);
    `ImGui_ImplDX12_CreateDeviceObjects()` called after Init; every HRESULT checked and logged once.
  - Format at startup: `init()` built ImGui for RGB10A2 when the colour space already said HDR10, then rebuilt for the
    composite's RGBA8 target on first open; now RGBA8 from the start like GyroLib, waits for the last submission before any
    mode-dependent replacement, rejects a backbuffer index outside the buffer count. The panel's ImGui context is selected on
    the render worker (`api_dx12_present`/`before_resize`/`lost`), since the DX12 backend keeps renderer data in the current
    context.
  - The temporary `Dx12Composite` INI gate is gone: the composite is the normal path on scRGB/HDR10; the SDR fallback is only
    for MSAA swapchains or a failed composite, logged once. `pick_mode` checks the colour space against the backbuffer format
    (HDR10 = PQ on RGB10A2, scRGB = G10 on FP16). Log must say `the window drawn as HDR10` with no DEVICE_REMOVED.
- 2026-11-01 **Gyro4All's own settings window in the panel (g4a ABI 6), DX9–DX12 + Vulkan + OpenGL**:
  - GyroLib's panel (its DX12 frontend in gyrolib.dll, or the static `gl_panel_draw` this DLL draws in non-DX12 games) has no
    host-extension API and ImGui objects must not cross gyrolib's ABI. So the Gyro4All settings (Gyro4All.ini) get their
    **own ImGui window in gyro4all.dll**, drawn next to GyroLib's panel whenever that is open (F10): folded it is a small
    "Gyro4All settings" button at the top centre; clicking expands the window. One code path for every API.
  - `gyro4all_panel.h` **ABI 5 -> 6** (both DLLs ship together): `create` gains `const g4a_settings* settings, uint32_t flags`
    (G4A_CREATE_DX12 = the DLL draws no gl_panel, only the Gyro4All window); new `dx12_present` / `dx12_before_resize` /
    `dx12_lost` in `g4a_api`. `g4a_settings`: `get` (effective value, profile wins, utf-8) / `set` (writes [Gyro4All] in
    Gyro4All.ini via `settings::set_string`). winmm side: `panel_host::attach_dx12(ctx, hwnd)` + `dx12_present/before_resize/lost`.
  - **DX12 renderer** (`src/panel/panel_dx12.cpp`, ImGui's DX12 backend): drawn inside the game's Present on winmm's render
    worker (`overlay.cpp render_job`), **after** `gl_overlay_dx12_render` on the same queue. Own RTV heap, command
    allocators/list/fence per buffer index, no backbuffer reference between frames; `dx12_before_resize` releases RTVs before
    ResizeBuffers; `dx12_lost` from `release_job`. SDR: direct into the backbuffer. **scRGB (FP16) and HDR10 (RGB10A2)**: the
    window is rendered into an SDR RGBA8 texture and composited over a per-frame copy of the backbuffer in-shader (no
    hardware blending), UI white 203 nits, using GyroLib's own `overlay_hdr.hpp` shader verbatim (HDR10: PQ-decode the copy,
    Rec.709 -> Rec.2020, PQ-encode; pixels without UI are discarded). Compiled at runtime with D3DCompile (DXBC), like GyroLib's
    composer (the dxc/DXIL build removed the device in 007, see 2026-11-02). Root signature: param 0 = table of two SRVs
    (panel, scene), param 1 = {whiteNits, pq}; PSO rebuilt when the backbuffer format changes. `pick_mode` returns Hdr10 for the
    explicit PQ colour space (12), same rule as the D3D11 panel (no auto-detect: an RGB10A2 swapchain stays SDR until the game
    calls SetColorSpace1; the first pick_mode only knew scRGB and drew [0,1] colours straight into the PQ backbuffer in 007).
  - `overlay.cpp`: the DX12 swapchain hooks `SetColorSpace1` (slot 38) and tracks `g_dx12_color_space`. `attach()` loads
    gyro4all.dll in DX12 games too. `hk_wndproc` forwards every message to **both** frontends (capture = union; a consumed
    WM_INPUT still gets `DefWindowProcW`). `panel_main.cpp`: in DX12 mode `api_frame` never calls `gl_set_panel_open` (the overlay
    owns the open state; its 250 ms heartbeat closes it; on timeout it only stops drawing). Escape closes the static panel only.
  - Settings shown (via the `g4a_settings` callbacks), grouped: XInput (Off/Auto/Always), XInputNintendoLabels,
    XInputTouchpadBack, NativeFlickStick, StickCamera + Speed/SpeedPitch/Deadzone/Curve/Edge/EdgeSpeed/EdgeRamp/InvertY/
    PassButtons, the ADS/ALT_ADS/OTHER sensitivity profiles (button + mode combos), ZoomReferenceFOV, SensorPacing +
    SensorPacingDelayUs, Notices. Each (?) help says live vs restart: only the pacer (`load_pacer`) and stick camera
    (`load_stick_camera`) reload via the per-second settings-file watcher; the rest is read in `host_configure` (restart).
  - Fixes from testing: the windows call `BringWindowToDisplayFront` every frame (GyroLib's panel is opaque and a click
    focuses it, which buried the Gyro4All window), but only while no popup is open (otherwise combo dropdowns, their own
    windows, were buried: selects "never opened"). `settings::set_string` edits Gyro4All.ini line by line replacing the
    commented `;Key=default` in place (`WritePrivateProfileString` doesn't count a commented line and appended a duplicate;
    unit-verified for commented defaults, active keys, LF/CRLF, insertion before the next section, missing file). Sliders and
    checkboxes take the real defaults as fallbacks (StickCamera: speed 360, pitch 200, deadzone 10, curve 2.0, edge 90, edge
    speed 720, edge ramp 400; SensorPacing=1, delay -1, Notices=1, XInputTouchpadBack=1). Profile-button selects use two fixed
    naming sets chosen by the connected controller's name (dualsense/dualshock/playstation/ps4/ps5: L2 R2 L1 R1 L3 R3 Cross
    Circle Square Triangle; else LT RT LB RB LS RS A B X Y; shared Back Start d-pad Touchpad Misc paddles) because
    `gl_get_button_label` fell back to positional "South/North..." in game; the stored value matches by SDL ordinal, so a value
    written with the other set still displays.
  - **Gamepad access (ABI 7, `toggle_expanded`)**: ImGui's gamepad nav cannot cross windows, so winmm.dll polls an L3+R3 chord
    while the panel is open (`host.cpp`) and folds/unfolds through the panel API. Unfolding moves nav focus into the window
    (`FocusWindow` + `RestoreFocusedChild`); folding returns focus to GyroLib's panel by name ("GyroLib###GyroLibPanel"), only
    meaningful in shared-context games (in DX12 that window lives in gyrolib.dll's context).
  - The window replicates **GyroLib's own scaling recipe** (MIT source `src/panel.cpp gl_panel_draw`): `scale = clamp(max(1,
    dpi, height/1080), 0.75, max(0.75, min(3, height/760)))`, `PushFont(nullptr, 18*scale)`, centred geometry `min(w-24s,
    960s) x min(h-24s, 870s)`, header bar 44*scale. The folded button sits in the middle of the footer on the "Reset all
    settings" row (centre ~32*s above the bottom edge), left-aligned at the 22*s inset; recomputed from the same geometry so it
    also works in DX12 games where GyroLib's window is invisible to this DLL. The expanded window opens above it with GyroLib's
    header style (opaque, TitleBgActive bar, a × button, content at (22*s, bar+18*s)). The cog icon is drawn ImDrawList
    geometry (ImGui's default font has no U+2699).
  - Verified: build + 709 unit checks. In-game (F10 window, live edits persisting, resize, Streamline swapchain recreation,
    gamepad/mouse) to test per game, as with every panel change.

(Record exact class/function/property names, signatures, offsets, UE versions and what worked or failed.)

- 2026-10-08 **GyroLib accepts external samples**: `gl_register_endpoint(ctx, &gl_endpoint)` + `gl_submit_sample(ctx, endpoint,
  &gl_sample{sensor_ns, arrival_ns, gyro_dps, accel_g})`. Endpoint ids nonzero and unique (max 32); motion qualifies after 2+
  increasing sensor timestamps with arrivals <= 100 ms apart; stale after 150 ms. SDL reader: `gl_sdl_create(ctx, 0)`; the host
  must pump SDL events on the owner thread. Steam: `gl_steam_runtime_create` borrows the loaded `steam_api64.dll`.
- 2026-10-08 **Games that ship Sony's `libScePad.dll`** (Pacific Drive) already drive the DualSense natively. Alternative sensor
  source: hook `scePadReadState` + `scePadSetMotionSensorState` and feed `gl_submit_sample`. Coexistence confirmed: with
  gyroprobe's SDL reader running (11 min alongside gameplay, ~460 samples/s, 0 dropped) the game's haptics and adaptive
  triggers kept working (USB, Steam Input off), so the libScePad hook fallback isn't needed.
- 2026-10-08 **gyroprobe** (`tools/gyroprobe`, built against the GyroLib 1.2.2 SDK): headless `SdlInput` sees the DualSense over
  USB, ~490 samples/s. Before calibration a stationary pad drifts ~0.6 deg/s, so the mod must calibrate. GyroLib settings
  (`gyroprobe --settings`): `calibration.automatic` defaults to **0 = Off** (1 = menus only, 2 = any time) (why calibration
  never ran); `ui.menu_key` defaults to **F10**; pad shortcut Back+Start = Create+Options (check it doesn't also trigger game
  actions). Settings needing host support: `camera.recenter_button`, `flick.*` (suppress the game's right stick),
  `gyro.zoom_compensation` (FOV).
- 2026-10-08 **Pacific Drive + UE4SS v3.0.1-1028** (`dwmapi.dll` + `ue4ss\`, UE 4.27): AOB scan OK except
  `ConsoleManagerSingleton`; Lua mods start. **UE4SS's GUI console crashes the game at startup** (stack overflow: UE4SS,
  `gameoverlayrenderer64`, SCSKiller `d3d12`, ReShade `dxgi`, `nvoglv64` recursing); fix `GuiConsoleEnabled = 0`.
  BPModLoaderMod created an empty `LogicMods` folder.
- 2026-10-08 **Pacific Drive camera path (probe v1-v3, USB, Steam Input off):**
  - PC `BP_PlayerController_MainCharacter_C`, pawn `PDP_MainCharacter_C` both on foot and in the car (no vehicle possession; at
    the main menu the pawn is `DefaultPawn`), FOV 90. **On foot:** camera rotation == `ControlRotation`; `SetControlRotation`
    moves the view in exact degrees; `AddYawInput(10)` -> +25 (`InputYawScale=2.5`, `InputPitchScale=-2.5`).
  - **In the car:** view target = attach parent = `BP_PlayerCarNew_C`, active camera `FirstPersonCamera`. `ControlRotation`
    changes but the camera doesn't. Stick look writes `CameraControllerRotation_Target` (FRotator on the car, @0xB00), smoothed
    into `_Current` (@0xAF4), copied into `FirstPersonCamera.RelativeRotation`. **Direct writes to `_Target` work**: yaw +10 /
    pitch +5 land exactly, `_Current` follows in ~50 ms, holds 3+ s. The BP clamps yaw at **±140**; pitch limits not measured
    (seen -19.8..+11.5).
  - Auto-recentering (`IsAutoControlling` @0xB68, `Camera_AutoControlAmount_current/_target` @0xC28/0xC2C, `TimeNotSteering`)
    pulls a gyro-moved view back while *driving*: **bug (user)** the gyro stopped moving the view while driving (R2) after a
    while, a stick look brought it back. v0.2.2 added profile keys per view: `OnGyro=Prop:value,...` (scalar writes on the view
    object whenever the gyro moves it), `Watch=Prop,...` (log on change), `Dump=1` (all properties once); Float/Double/Int/Byte/
    Bool. Pacific Drive InCar: `OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0,Camera_AutoControlAmount_current:0`:
    **fixed, user-verified while driving** (the watch log showed the BP re-arming auto-control 6 times, OnGyro cancelling ~3 ms
    later). Other car properties: `CameraControllerRotationSpeed` 55, `CameraYaw/PitchMinMaxAngles`, `AutoRecenteringSpeedSlow/
    Fast` 0.3/1.3, `TimeSteering/TimeNotSteering`.
  - Apply rule: pawn attached to `BP_PlayerCarNew_C` -> deltas to `car.CameraControllerRotation_Target`, else
    `SetControlRotation(GetControlRotation() + delta)`; real degrees in both, no `InputYawScale`. Probe keys: avoid **F11**
    (windowed toggle); F10 was Steam's screenshot key (user moved it).
- 2026-10-08 **GyroUE v0.1-0.2.1 in Pacific Drive: works** (user: on foot and in the car; stops in menus/pause; no crashes).
  - Standalone `version.dll` proxy (UE4SS disabled); gyro frame from an IAT hook of `PeekMessageW` on the game thread. Shape-scan
    found GUObjectArray (exe+0x54722B0, stride 0x18) and FNamePool.Blocks (exe+0x5435D10). Layout matches UE4SS: SuperStruct
    0x40, ChildProperties 0x50, FField.Next 0x20, Name 0x28, ElementSize 0x3C, Offset_Internal 0x4C, sizeof(FProperty) 0x78;
    engine class `DrivingGameEngine`. (First build missed sizeof(FProperty): the size loop started unaligned and skipped 0x78.)
  - Main menu: PC `BP_MainMenu_PlayerController_C`, pawn `DefaultPawn`, cursor=1 -> camera off. Pause: `WorldSettings.
    PauserPlayerState` set. v0.2.1: no gameplay view while the pawn is a `DefaultPawn` (SpectatorPawn derives from it); v0.2
    had applied a tiny turn to the main-menu camera when the cursor was hidden.
  - v0.2: in-game settings panel (GyroLib DX12 overlay, F10 / Create+Options). Gated by `gyrolib.ini`: with `ui.menu_key=` empty
    and `ui.gamepad_menu_shortcut=0` no graphics hooks are installed (read in DllMain). Hooks inside our module (no inline
    patching): IAT of `CreateDXGIFactory`/`1`; factory vtable CreateSwapChain (10) and CreateSwapChainForHwnd (15), which capture
    the swapchain and its DIRECT queue; swapchain vtable Present 8, ResizeBuffers 13, Present1 22, ResizeBuffers1 39; WndProc
    subclass forwarding to `gl_overlay_win32_message`. Works next to ReShade + RenoDX + SCSKiller + Steam overlay (swapchain
    captured ~0.7 s after start). Without a renderer (non-DX12) the panel is force-closed so it can't hold the gyro invisibly.
    Limitation: gamepad input still reaches the game while the panel is open. The panel saves to `gyrolib.ini` and GyroUE
    reloads it (1 s poll). Automatic calibration ("Menus only") works (bias ~(0.10..0.24 -0.08..-0.19 -0.25..-0.65) deg/s,
    matching gyroprobe). `gl_diagnostics.calibration_state` stays 0 (tracks manual calibration only). Toggle activation and
    per-context settings (`context.2.inherit=1`) work; car/foot switching correct.
- 2026-10-08 **Second game: Returnal (Steam 1649240), UE 4.25.1 licensee, MODULAR build** (264 `Returnal-*-Win64-Shipping.dll`).
  No anti-cheat (EOS SDK for co-op); ships `libScePad.dll`; has ReShade (`dxgi.dll`, RenoDX), SCSKiller (`d3d12.dll`), Streamline.
  - **v0.3** for modular builds: every import hook applies to the exe + all `<Project>-*.dll` modules, later-loaded modules are
    patched from hooked `LoadLibrary*`; memory scans cover all game modules. `version.dll` imported by Core, `PeekMessageW` by
    ApplicationCore/Core, DXGI by DXBridge/StreamlineD3D12RHIPreInit (loaded later). **No profile or game code needed:**
    GUObjectArray in `Returnal-CoreUObject…dll+0x2F6D28`, FNamePool in `Returnal-Core…dll+0x5C2950`, same 4.25+ layout, engine
    `GameEngine`, PC `BP_PlayerController_C`, pawn `BP_PlayerCharacter_C`; generic ControlRotation view active.
  - **Panel bug (v0.3.2):** Returnal calls `ResizeBuffers` on a different thread than `Present` (RHI thread vs another);
    GyroLib only accepts render calls on its one render thread, so `before_resize` returned -1, the panel kept backbuffer
    references and the image froze on the black loading screen. Fix: while the panel is closed, release GyroLib's backbuffer
    references at the end of every frame on the render thread. (A cross-thread resize *with the panel open* still failed; solved
    by the render worker in 0.4.4.)
  - **User test (v0.3.2): ~7 min combat, works, no crashes**; pause/menus gate the gyro; user set `gyro.activation=3` (hold)
    with `activation.trigger=3` (L2 or R2, 0.2). Triggers read through SDL work while the game reads the pad via libScePad;
    haptics and adaptive triggers OK.
  - **Panel shortcuts:** a physical F10 didn't open the panel (system key, not delivered as a normal WM_KEYDOWN; my SendInput
    test had worked). Create+Options opened and closed it at once (Options also opens the game's pause menu; GyroLib closes the
    panel when owner snapshots are older than 250 ms). **v0.3.3:** GyroUE polls the configured F-key (`GetAsyncKeyState`) on the
    game thread, toggles with `gl_overlay_set_open`, no longer forwards that key's messages; Returnal gets
    `ui.gamepad_menu_shortcut=0`. Physical F10 confirmed, but only from menus/pause, not during gameplay (user accepted;
    undiagnosed then, probably the render-thread problem found in 0.4.x).
  - Generic installer `tools/deploy-gyroue.sh "<Win64 dir>" [remove]` (never overwrites `gyrolib.ini`, refuses to replace a
    foreign `version.dll`).
- 2026-10-08 **GyroLib updated to 1.3.0** (drop-in: ABI 1, INI schema 0.2.0 unchanged; fixes: "Recalibrate gyro" works, manual
  calibration more tolerant and saved per SDL sensor, Steam mouse Block/Convert). Host rule: report menu state independently of
  camera permission (`menu_open` = cursor; `camera_allowed` separate). SDK pinned in `tools/gyrolib-sdk.sh` (used by the build
  and deploy scripts). v0.3.4 logs the SDK version. Performance: `bench/BENCHMARKS.md`; no measurable FPS impact in Returnal.
- 2026-10-08 **Third game: Cronos: The New Dawn (Bloober Team), UE 5.4.4, monolithic.**
  - GyroUE 0.3.6, no profile: GUObjectArray exe+0xAD91670, FNamePool exe+0xACADED0; UE 5.4 layout FField.Next 0x18 / Name 0x20
    / ElementSize 0x34 / Offset_Internal 0x44 / sizeof(FProperty) 0x70. ReShade + RenoDX present; DX12 panel works. Main menu
    `view=none` (correct). Gameplay: PC `SHPlayerControllerPlay_BP_C`, pawn `CronosCharacterPlay_BP_C`. The generic view writes
    `ControlRotation` (applied yaw reached -220) **but the camera doesn't move** (PC prefix `SH` = Bloober's Silent Hill 2 Remake
    framework).
  - 0.3.7 generic probe (`Probe=1`): F7 snapshots every float/double/rotator (nested structs to 3 levels) of PC, pawn, PCM, view
    target and components; F8 logs changes. **Result (right stick):** the stick does not change `ControlRotation`; changed
    rotators are all outputs (`SHPlayerCameraManagerPlay.ViewTarget/CameraCachePrivate/LastFrameCameraCachePrivate.POV.Rotation`,
    PCM `TransformComponent0.RelativeRotation`, `Animation.LastAimRotation`; also SpringArm `TargetArmLength` 180 -> 102 looking
    up, anim `AimOffset.Y`): the look state is non-reflected C++. 0.3.8: the probe also diffs raw memory of every probed object
    and logs aligned floats/doubles equal to the POV yaw/pitch. **Raw result:** the look state is the pawn's **`View`
    (`SHCharacterPlayViewComponent`)**: two non-reflected **double rotators at +0x2A0 and +0x420** equal the POV in both runs.
  - 0.4.0: views can target any object and raw offsets: `Object=Root.Sub...` (Root = PC, Pawn, Camera, ViewTarget), `Offsets=`,
    `RotatorSize=24|12`. Cronos profile: `Object=Pawn.View`, `Offsets=0x2A0,0x420` (both get the delta). **User test: works,
    "perfectly acceptable"**; stick still works; pause gates the gyro.
  - Calibration gap: Cronos menus show no cursor with a gamepad (`cursor=0`), so "menus only" auto-calibration rarely ran.
    **0.4.1:** `menu_open = !pc || cursor || paused || no view` (generic); verified, bias learned in the main menu within 10 s.
  - **F10 panel missing in one session** (worked in 0.4.0) with HDR on + RenoDX. 0.4.2 added logging (F-key requests, panel
    state, render results, ResizeBuffers HRESULTs); 0.4.3 re-inited on a real-format change, **but its log disproved the format
    hypothesis** (every ResizeBuffers keeps 2 buffers format 24). **Real cause: UE moved Present to another thread** after the
    3rd ResizeBuffers; `gl_overlay_dx12_render` returned -1 "render requires initialized render thread"; the 0.4.2 retry made
    it worse (shutdown from the new thread failed, init failed "already initialized"), panel disabled for the session. Probably
    also why F10 failed in Returnal gameplay.
  - **0.4.4: render worker.** Every GyroLib render call (init/render/before_resize/shutdown) runs on GyroUE's own thread; the
    presenting/resizing thread hands the job over and waits. Worker stuck 5 s disables the panel. 0.4.4 still didn't open F10:
    the "skip GyroLib while closed" optimisation was the cause (GyroLib refuses/closes the panel when the render side is stale
    > 250 ms). **Rule: call `gl_overlay_dx12_render` every frame, even while closed.** 0.4.5 verified (HDR on, RenoDX, ReShade):
    panel works in menus and game, Present hook cost with the panel closed avg 16-20 us (p99 29-42 us), open 4-7 ms (GyroLib's
    own draw/fence wait). Fallback idea (not needed): detect the failure, disable the panel for the session with a clear log
    line.
  - 0.4.6: probe lines labelled with the profile path of each object (`Pawn.View(SHCharacterPlayViewComponent)`), roots tried in
    order PC, Pawn, Camera, ViewTarget, so the part before "(" can be copied into `Object=`.
- 2026-10-08 README.md written for GitHub (contents, install, profiles, probe walkthrough, GyroLib MIT notice, third-party
  table); deploy ships `licenses/` (GyroLib SDK licenses, `SDL-CHANGES.md`, `GyroUE-LICENSE.txt`) as GyroLib's THIRD_PARTY.md
  requires.
- 2026-10-09 **Gears of War: E-Day** (Xbox/Game Pass GDK build, UE 5.6, `Content\FairlightConcept\Binaries\WinGDK\GoWEDay.exe`;
  encrypted/ACL'd, pefile/dumpbin can't read it; Themida-protected). Already present: UE4SS as `dwmapi.dll`, ReShade as
  `dxgi.dll` (+ RenoDX), `EACbegone.bat`.
  - First reports: default `version.dll` wrote no log (the DllMain gate only accepted `*-Shipping` / `*-Win64-*` exe names);
    renamed to `winmm.dll` it failed with "entry point `timeGetDevCaps` not found" (the DLL exported only version.dll's 17
    functions). **0.4.7:** gate also accepts exes under `Binaries`; the proxy exports version.dll + all 180 named winmm.dll
    exports (`proxy.def`) and forwards to `System32\<own file name>`, so the same DLL works under either name; `deploy-gyroue.sh`
    takes `PROXY=winmm.dll`. **0.4.8:** the exe-name/location gate is gone; a mutex `Local\GyroUE-<hash of the lowercased install
    folder>` makes the first process that loads the proxy the only one running GyroUE; later ones are plain forwarders.
  - 0.4.8 log: loads, but `hook: PeekMessageW not imported by any loaded game module yet`. 0.4.9 dumped the import table: **the
    exe's PE import directory is a protector stub** (RVA 0xfbe4795, 52 DLLs with 1 slot each and no name table); the real
    imports are resolved later by the protector (SecureEngine/Themida, `SecureEngineSDK64.dll`) into its own table, after our
    DllMain. No IAT hook can work (not PeekMessageW, not LoadLibrary, not DXGI factory). VERSION.dll *is* in the stub list.
    Options: inline hook (MinHook), scan the protector's table, a timer thread. 0.4.9 also accepts API-set DLL names and matches
    slots by resolved address when there is no name table.
  - **0.5.0 (user chose inline hooks):** MinHook **1.3.4** (BSD-2) in `third_party/minhook-1.3.4` (`git clone --depth 1 --branch
    v1.3.4 https://github.com/TsudaKageyu/minhook third_party/minhook-1.3.4`), static lib. `hooks::init` detects the stub (imports
    present, no name tables) and switches to inline mode (every `hooks::add` with a named DLL hooks that export; LoadLibrary hooks
    skipped); `GYROUE_INLINE=1` forces it. Releases ship `licenses/MinHook-LICENSE.txt`. Two silent runs (02:08, 02:18) remained
    unexplained (a still-held instance mutex, or the user deleting the log); 0.5.1 added `GyroUE-boot.log` (one line per loading
    process: time, version, pid, thread, exe, outcome; restarts past 64 KB).
  - **0.5.1 run: inline hooks work** (MH_OK on `USER32!PeekMessageW` and ReShade's `dxgi.dll!CreateDXGIFactory/1/2`); gyro frame
    ran on the game thread; first host frame took 2.43 s (GyroLib/SDL init). **Crash:** AV in `system32\dxgi.dll+0x2c459` via
    GoWEDay -> `sl_interposer` (Streamline) -> **our inline `CreateDXGIFactory*` hook** -> `RTSSHooks64` -> system dxgi (four
    layers chained on those exports: Streamline, ReShade, RTSS, us). **0.5.2:** in inline mode the overlay no longer hooks dxgi
    exports; a thread started from DllMain loads `System32\dxgi.dll`, creates its own factory and patches that **real factory's
    vtable** (CreateSwapChain 10, CreateSwapChainForHwnd 15). Build emits `version.pdb`. **0.5.2 run: no crash, F10 panel works**
    (user), auto-calibration ran. Gyro not applied: **GUObjectArray never found** (only FNamePool at `GoWEDay.exe+0xEBA1CD0`);
    UE4SS also fails several AOBs here but its object cache works.
  - **0.5.3:** GUObjectArray scan also tries strides 0x28/0x30 and, after ~10 failures, scans writable+executable sections every
    10 s (logs the section table and up to 8 header-valid candidates). Result: sections unnamed R-X, RW- data at rva 0x0E497000
    (FNamePool in it), **.themida RWX**, **.boot R-X**; no object-array header anywhere. Explanation from
    `ue4ss/UE4SS_Signatures/GUObjectArray.lua` (a custom signature for the Steam build): "The global holds an XOR-encoded
    FUObjectArray pointer, not an inline array." (`0F BA EF 19 83 E0 FB 89 46 ?? 48 8B 0D <rel32> 48 BB <imm64 key> 48 33 CB 75
    ?? B9 B8 00 00 00`: mov rcx,[global]; mov rbx,key; xor rcx,rbx; ...; mov ecx,0xB8 = sizeof(FUObjectArray)). UE4SS 3.0.1 failed
    with it on the Xbox build too (also GMalloc, FName::ToString/ctor, StaticConstructObject, FUObjectHashTables, GNatives,
    ConsoleManager, GameEngineTick).
  - **0.5.4:** `ue.cpp` scans the executable sections for the generic idiom `REX.W 8B /r [rip+disp32]` -> `REX.W B8+r imm64` ->
    `REX.W 33/31`, keeps up to 8192 unique (global, key) pairs, decodes `*global ^ key` and accepts it if a valid ObjObjects
    header + 16 live items sit within the first 0xB8 bytes (`object_array_stride()`). Run: `0 XOR-encoded pointer loads` (code
    virtualized or the scan didn't cover it). **0.5.5:** the code scan logs MB/regions/faults; new last resort: a background
    thread scans every committed `MEM_PRIVATE` `PAGE_READWRITE` region (raw prefilter on Max/Num/MaxChunks/NumChunks, then the
    stride check), at most every 30 s, 10 times; `module_offset()` prints `heap 0x...` for non-module addresses. Tested with a
    fake heap FUObjectArray.
- 2026-10-09 **GyroUE 0.5.6-0.5.7:** the DLL carries a Windows version resource (`src/GyroUE/version.rc.in`, configured by CMake
  from project(), so the version still lives only in CMakeLists.txt); the Comments field was removed in 0.5.7 (user request).
- 2026-10-09 **Gears of War: E-Day WORKS (user-verified, 0.5.5 renamed to `version.dll`): gyro turns the camera and the F10 panel
  works.** Pad/connection/Steam Input not reported.
  - 0.5.5 as `winmm.dll` didn't load at all (like the 02:08/02:18 runs). Running then: RTSS + RTSSHooksLoader64, MSI Afterburner,
    NVIDIA Overlay, Steam. `RTSSHooks64.dll` statically imports WINMM.dll. Hypothesis (unconfirmed): an injected DLL sometimes
    loads System32 winmm.dll first, so ours is never mapped. `version.dll` loaded first try. **Use `version.dll` for Gears.**
  - Code scan: 224.5 MB in 3 regions, 0 faults, **0 XOR-encoded loads** (the Steam-build idiom isn't native code in the Xbox
    build). **Heap scan found GUObjectArray:** `heap scan found ObjObjects (1.37 GB in 1959 regions, 344 ms)`, stride 0x18, 75910
    objects, ~30 s after start (scan starts at the 3rd failed init). Layout: SuperStruct 0x40, ChildProperties 0x50, FField.Next
    0x18, Name 0x20, ElementSize 0x34, Offset_Internal 0x44, sizeof(FProperty) 0x70; engine `GameEngine_2147482572`.
  - Front end: pc `FairlightPlayerController_FrontEnd_BP_C`, no pawn (view=none, calibration runs). Campaign: pc
    `BP_PlayerController_Campaign_C`, pawn `BP_Pawn_Marcus_Casual_Holster_LipScar_C`, **generic ControlRotation view works, no
    profile**. Perf: idle avg 90 us p99 274 us; active avg 109 us p99 373 us; Present hook avg 25 us p99 59 us.
  - Open then: why winmm.dll sometimes doesn't load; the 30 s delay before the heap scan finds the array.
- 2026-10-09 **0.5.7 deployed to Cronos, Pacific Drive, Life is Strange Remastered (`LIS\Binaries\Win64\LiS-Win64-Shipping.exe`,
  UE4SS as dwmapi.dll) and Out of Sight** (`Binaries\Win64` holds two full builds, `OutOfSight-Win64-Shipping.exe` and
  `OutOfSight.exe`; one runs at a time; ReShade 6.7.3 + RenoDX, a d3d12.dll, D3D12 Agility). No anti-cheat in any. Saves backed
  up with `um backup` first.
- 2026-10-09 **GyroUE 0.5.8:** with `Probe=1`, results go to `GyroUE\GyroUE-probe.log` (recreated each session, header with the
  F7/F8 steps and how to read rot/raw lines); `GyroUE.log` only gets `probe: snapshot #N ...` / `probe: diff of snapshot #N`.
- 2026-10-09 **GyroUE 0.6.0: probe for non-experts.**
  1. **Follow check (always on):** each frame compares the gyro rotation written (|yaw|+|pitch|) with how much the camera's POV
     moved (`PlayerCameraManager.CameraCachePrivate.POV.Rotation`, `ue::camera_pov`, offsets cached per class; `CameraCache`
     fallback). 2 s windows with >= 10 deg of gyro: ratio < 0.2 = ignored, > 0.5 = followed. Only when view target == pawn (or
     a profile view with ViewTargetClass), not paused, no cursor. 3 ignored windows and none followed -> one warning
     (`camera: the gyro turned X ... the game ignores it. ... put [GyroUE] Probe=1`); 3 followed -> `camera: confirmed`.
  2. **Automatic capture (Probe=1):** snapshot once the POV has been still 0.5 s (min 1.5 s after the last capture); when the POV
     has turned > 30 deg yaw and is still again for 0.3 s, a scored diff, unless the gyro wrote more than 20% of the turn
     ("discarded"). Re-snapshot if the pawn changes or after 2 min. Up to 8 per session. F7/F8 still work and switch the
     automatic capture off.
  3. **Ranking + suggested profile:** each diff with |yaw| >= 10 deg is scored. Candidates = reflected rotators and raw
     float/double (pitch, yaw) pairs that now equal the POV pitch/yaw (0.6 deg) and whose yaw changed. Ignored with a reason:
     `Camera` root (output), RelativeRotation / raw offsets inside the SceneComponent part (transforms), PC.ControlRotation (the
     default view), nested struct properties. Usable ones that hit every scored diff (all but one from 4 on) are grouped per
     object (raw offsets of one size together) and ranked Pawn > ViewTarget > PC, more rotators first, named before raw. From 2
     scored diffs on, written to `profiles\<exe>.suggested.ini` and the probe log.
  Read-only. Cronos validation: with only `Probe=1` the probe wrote `profiles\Cronos-Win64-Shipping.ini` itself after 5 scored
  captures: `Object=Pawn.View Offsets=0x2A0,0x420 RotatorSize=24`, exactly the manually verified profile.
- 2026-10-09 **GyroUE 0.6.1:** on-screen notices (`notify.cpp`): a GDI box at the top centre of the game's largest visible window
  (layered 225 alpha, click-through, NOACTIVATE/TOOLWINDOW/TOPMOST, own thread + message loop on first use, 250 ms timer to
  follow the window, hidden when the game isn't foreground, font = client height/40 clamped 16-36 px Segoe UI). Renderer-agnostic;
  not visible in exclusive fullscreen. `Notices=0` turns it off. Follow check is off after its first verdict.
- 2026-10-09 **Crash in Cronos with 0.6.1 at startup: EXCEPTION_STACK_OVERFLOW** (cdb + pdb): endless recursion
  `hk_create_swapchain_hwnd -> d3d12!Ordinal196 -> hk_create_swapchain_hwnd`. The d3d12.dll is **SCSKiller 1.2.4 (D3D12 proxy,
  "shader compilation stutter killer")**. **Bug in `patch_vtable` since the first panel versions:** we hooked factory vtable slot
  15, SCSKiller hooked the same slot after us (saving ours as its original), and on the next factory patch_vtable saw a foreign
  function in the slot and patched it again, saving SCSKiller's function as our original. **0.6.2:** each slot is patched once
  and the original kept per slot address (`g_slot_originals`); every hook calls `original_of(object, index, fallback)`
  (CreateSwapChain 10, CreateSwapChainForHwnd 15, Present 8, ResizeBuffers 13, Present1 22, ResizeBuffers1 39). Test `chain.cpp`
  (a second hooker overwrites slot 15 after us): 0.6.1 loops, 0.6.2 calls it once.
- 2026-10-09 **GyroUE 0.6.3-0.6.4:** probe-ready notice reworded (30 s). `GyroUE\GyroUE.ini` (all games in that folder): created
  with the defaults if missing, never overwritten, not shipped (like gyrolib.ini); `[GyroUE] Probe=0`, `Notices=1`; `setting()`
  reads the game's profile `[GyroUE]` first (override), then GyroUE.ini, then the default. If the game had no profile when it
  started and the result has a usable view, the probe writes `profiles\<exe>.ini` itself ("created by the camera probe ... NOT
  VERIFIED") and keeps updating it; otherwise `<exe>.suggested.ini`. A result with no usable view never becomes the profile.
- 2026-10-09 **Life is Strange Remastered probe test (0.6.4-0.6.13; DX11, UE4 <= 4.24).** Non-stock layout: `SuperStruct=0x40
  ChildProperties=0x48 FField.Next=0x28 FField.Name=0x18 ElementSize=0x34 Offset_Internal=0x44 sizeof(FProperty)=0x70`; GUObjectArray
  exe+0x35A3908, FNamePool exe+0x358C390; PC `LiSPlayerController_BP_C`, pawn `MaxCharacter_BP_C`, camera `LiSPlayerCameraManager`,
  view target mostly scripted `CameraActor_10xx`.
  - **0.6.5-0.6.6:** every snapshot had **0 values** because `field_type()` read the FFieldClass at a hard-coded +0x08; and
    **LiS is UE <= 4.24 (UProperty objects, no FField)**: the "FField" layout detected (Name 0x18, Next 0x28, ChildProperties
    0x48) is really UObject::NamePrivate / UField::Next / UStruct::Children; the property type is its UClass at +0x10 named by the
    UClass's own UObject name (+0x18). `detect_layout` now finds FField::ClassPrivate (0x08-0x40), tries FFieldClass::Name at +0
    then a UClass name at +0x18 (`g_field_class_name`); the layout line says "FField properties, UE 4.25+" or "UProperty objects,
    UE <= 4.24" and `Field.Class=`. Empty snapshots say "layout problem" instead of the misleading "press F7 first". Other games'
    detection is unchanged (every UE 4.25+/5 game read `StructProperty` with the old +0x08 first).
  - **0.6.7:** GyroUE-boot.log removed (0.5.1 diagnostic). The probe reads LiS (~3300-3780 values per snapshot). Every scored turn
    matched the same rotations: PC.ControlRotation, Camera ViewTarget/CameraCachePrivate POV, TransformComponent0 transforms, and
    **raw float rotators `LiSPlayerCameraManager` +0x33CC and +0x33FC**.
  - **0.6.8-0.6.9:** gyro output paused while an automatic capture runs (`probe_capturing()`, so the pad can stay in the hands);
    a turn only counts if the right stick was deflected (> 0.3) since the snapshot (`gl_get_flick_input`; otherwise "ignored:
    the right stick wasn't used (cutscene or scripted camera)"); raw rotators of the Camera root past the engine's
    PlayerCameraManager part (`engine_part_size`) are candidates (rank 1, like PC). "Put the controller down" removed from the
    notice. Probe wrote `Object=Camera Offsets=0x33CC,0x33FC,0x3ACC RotatorSize=12` (new candidates
    `LiSPlayerCameraManager+0x3ACC` and `Pawn.LiSCamera+0x1B0`); next session `camera: confirmed`.
  - **Interpretation errors (kept as lessons):** 0.6.9 concluded +0x33CC/+0x33FC were outputs the game rewrites because they equal
    the POV after one frame; **wrong**: GyroUE writes before the game ticks, the game reads our value, integrates it and writes
    the result back, so they always equal the camera including the gyro. A read-back after one frame can't tell "integrated and
    written back" from "overwritten" (so a "drop outputs by read-back" idea is invalid). Also `camera: confirmed` was a false
    positive from camera motion by the stick; 0.6.11: follow-check windows in which the right stick moved aren't judged.
  - **0.6.10:** every change of the gyro gate is logged with its reason (`gyro on` / `off: mouse cursor shown` / `off: game
    paused` / `off: no gameplay view`, plus the view target class). **A/B with `Offsets=0x33CC,0x33FC` (0.6.11): only pitch
    moves the camera** (follow check: "turned the profile's raw offsets by 1274 degrees but the camera moved only 90"): LiS
    splits axes: **pitch from +0x33CC/+0x33FC, yaw (following Max) from +0x3ACC**, and while Max walks the game turns +0x3ACC
    toward her walking direction (auto-recentering).
  - **Manual probe (F7 standing, walk without touching the camera, F8):** the walking camera is the camera mode object
    `PC.TP_Walk_Manual_C_<n>`; its `SmoothedAutoManualRatio` fell toward 0 (0.0719 -> 0.0025): the blend between the automatic
    follow camera (0) and manual control (1). The gyro isn't manual input for the game, so walking blends to auto and the follow
    camera wins the yaw. **0.6.12:** path step `@Class` = first subobject of that class (`ue::find_subobject_of_class`, cached),
    for instance names that change every session; `OnGyro=` entries may carry an object path (`Root.Sub.Name:value`, resolved like
    `Object=`); a failed write is logged once. LiS profile: `OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1`.
    **Verified (user): gyro yaw and pitch, fixed cameras and following Max, also while walking.**
  - 0.6.13: `profiles/LiS-Win64-Shipping.ini` added to the repo (shipped); no test code in the repo (test programs live in the
    scratchpad).
- 2026-10-10 **Switch Pro Controller judder (user, Cronos and Luto).** With GyroLib/SDL (Steam Input Block) the camera isn't
  smooth; Steam's gyro-to-mouse (Pass through) is; the DualSense shows almost no difference. gyroprobe with the Switch Pro: ~200
  accepted samples/s, 3 IMU samples per report arriving together every ~15 ms (~66 reports/s); the DualSense gives ~460/s. At
  frame rates above ~66 fps GyroLib outputs rotation only on frames where a burst arrived and 0 on the others; `gyro.smoothing_ms=20`
  barely helped (its filter steps only when samples arrive).
  - 0.6.14 pacing per burst (`Pacing=1`, `PacingMaxMs`, 50 from 0.6.15): "improved a bit, nothing like Steam Input mouse".
    Luto with Steam Input on gave only ~67 samples/s (the Steam-filtered isolated reader loses ~2/3); with it off ~200/s but the
    per-frame burst cadence was unchanged: a non-zero output every 21-27 ms (~5 samples each) at ~83 fps.
  - 0.6.16 `MotionTrace=1` (30 s to `GyroUE\motion-trace.csv`: every accepted sample via `gl_set_sample_observer`, every frame with
    raw and paced yaw/pitch; `tools/motion_trace_report.py`). Trace (Luto, Switch Pro BT, Steam Input off): 198 samples/s, sensor
    interval regular (5.24 ms; sensor timestamps drift ~0.7 s over 30 s against the host clock), arrivals bunched in the transport
    (~45 bursts/s instead of ~66 reports/s, gaps median 25.4 ms, p10 2.9, p90 38, max 68; 1-4 reports at once), the hook ran ~143
    times/s at ~85 fps (Luto pumps messages twice per frame). Raw: 73% of updates had no rotation, speed CV 1.99; 0.6.16 pacing: 26%,
    CV 0.78.
  - 0.6.17 jitter-buffer pacer: each update's rotation is split over its samples weighted by gyro speed, queued and played back at
    the measured sample rate with speed nudged x0.5-2 to keep `min(gap_mean + 0.5 sigma, PacingMaxMs)` queued; active only when
    `gap_mean > 1.5 x update interval` (DualSense unaffected); `tools/sim_pacer.py` replay: CV raw 1.56, 0.6.16 0.75, 0.6.17 0.44
    (empty slots 51% / 9% / 2%), queue delay median 18 ms. User: "clearly smoother, but not at the level of Steam Input".
  - **Decision:** stop investing in GyroUE-side pacing; the jitter belongs to acquisition/timing, which GyroLib owns. Pacer kept as
    a stop-gap (`Pacing=0`), later removed with GyroLib 1.3.1. Reported to GyroLib: (1) BT burst delivery should play back in sensor
    time with a small adaptive delay; (2) the Steam-filtered isolated reader delivers ~67 instead of ~200 samples/s. References:
    SDL's HIDAPI Switch driver, JoyShockMapper/GamepadMotionHelpers.
- 2026-10-10 **GyroUE 0.6.18: ships as `winmm.dll` by default, `version.dll` the fallback name** (user: winmm has wider support).
  The DLL exports both sets and forwards by its own file name. `deploy-gyroue.sh` defaults to `winmm.dll` (`PROXY=version.dll`
  for the fallback; Gears of War: E-Day needs it).
- 2026-10-10/11 **Release numbering:** production was 0.6.18, so what followed shipped as **0.6.19** (0.6.20/0.6.21 were local test
  builds); then user decided on **GyroUE 0.7.0** (minor bump: guard-page fix, `WorldRotation=1` + probe ranking + Severed Steel
  profile, GyroLib 1.3.1 with the GyroUE pacer removed, dependency check `deps.cpp`), and **0.8.0 = 0.7.0 merged into
  `feature/xinput-translation`** (minor bump for the XInput translation).
- 2026-10-10 **0.6.19: guarded reads no longer swallow stack guard-page hits** (from PSButtonsUE, "Luto crash with v0.1.0").
  `guarded_copy`, `guarded_store`, `scan_xor_loads`, `scan_heap_region` used `__except(EXCEPTION_EXECUTE_HANDLER)`; a plausible
  pointer landing on another thread's stack guard page raised STATUS_GUARD_PAGE_VIOLATION and swallowing it cleared PAGE_GUARD,
  so that thread couldn't grow its stack and the game later crashed in ntdll (UE 5.5.4). New `guard_filter` in `ue.cpp`: re-applies
  PAGE_GUARD to the faulting page (VirtualQuery + VirtualProtect on `ExceptionInformation[1]`) and handles it; AV and in-page
  errors as before; anything else is no longer swallowed. Latent bug; no GyroUE crash attributed to it.
- 2026-10-11 **Severed Steel (Demo, UE 4.27.2, `ThankYouVeryCool-Win64-Shipping.exe`, DX11).**
  - **Crash at startup (0.6.18, also on the full game):** log stops inside `gl_sdl_create`; AV reading 0x0 in `MSVCP140+0x12590`
    (`_Mtx_lock`) called from `gyrolib.dll`. The game ships VC++ runtime 14.24 next to its exe; the process loads that copy and
    `gyrolib.dll` (dynamic CRT, MSVC >= 14.40) gets it too. Known MSVC break: 14.40+ constructs `std::mutex` with a constexpr
    constructor that old `msvcp140` (< 14.40) can't lock. Our `winmm.dll` uses the static CRT. Workaround verified: rename the
    game's `MSVCP140.dll` and `MSVCP140_2.dll`. Real fix belongs in GyroLib (static CRT or `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`);
    reported upstream by the user. (This became the `deps.cpp` check.)
  - **Camera:** the generic view does nothing; the probe's profile (Pawn raw 0x1284,0x818), ReplicatedControlRotation and all raw
    Pawn rotators are ignored (0x7EC/0x818/0x1284 snap back every frame, 0x1278 keeps our writes but isn't read). Every moving
    rotator in the manual probe is a copy of `PC.ControlRotation` (= POV) or yaw - 90.
  - cdb write breakpoint on `PC.ControlRotation.Yaw` (`ba w4`): besides ours, the only writer is `APlayerController::
    SetControlRotation` once per frame, called natively (first thought Blueprint). `PlayerCharacter` (native under
    `ALSBaseCharacter`, 419 reflected properties) has +0x1D20 in an unreflected gap; writing it is ignored. Static disassembly: the
    tick reads look input `+0xBD8` (yaw) / `+0xBDC` (pitch), calls two functions (pitch at preferred `0x141705510`) that turn the
    `FirstPersonCamera` component (`+0x12A0`) from its current world rotation (`ComponentToWorld` quat at comp+0x1C0, cached
    rotator at comp+0x190; pitch clamped to StandingPitchMin/Max pawn+0x14A4/0x14A0) via `SetWorldRotation`, then
    `Controller->SetControlRotation(camera world rotation)` (vtable +0x688). The input handlers run after our PeekMessage frame.
    (An earlier "no location can drive this camera" conclusion was wrong: **the persistent state is the camera's world rotation**;
    writing RelativeRotation alone failed because the game rebuilt it from the stale ComponentToWorld the same frame.)
  - USceneComponent (UE 4.27, float): RelativeRotation +0x128, WorldRotationCache +0x180 (quat) / +0x190 (rotator),
    RelativeRotationCache +0x1A0 / +0x1B0, ComponentToWorld +0x1C0. **One-shot proof (cdb noninvasive, `ed` with float bit
    patterns):** RelativeRotation yaw +45 plus the matching quaternion at +0x1C0 kept, `PC.ControlRotation` followed by itself, the
    user saw the camera turn 45 degrees and the stick keeps working.
  - **`WorldRotation=1`** (view option with `Property=RelativeRotation`): `ue::add_world_rotation` finds ComponentToWorld by value once
    per component class (16-byte aligned quat in the SceneComponent part equal to quat(RelativeRotation), skipping the two caches),
    checks each frame that the stored quat still matches (else skips the frame and logs the reason), adds the delta (pitch within
    +-89) and writes rotator and quaternion. The probe now offers an attached component's `RelativeRotation` whose world rotation
    matches it (CameraComponent first; root components stay outputs). **Verified (user, DualSense): smooth and correct**
    (`Object=Pawn.FPCamera Property=RelativeRotation WorldRotation=1`; `camera: confirmed` 3 s in). The probe, run with no profile
    and 3 scored captures, wrote exactly that. Profile `profiles/ThankYouVeryCool-Win64-Shipping.ini`; README lists the msvcp140
    rename note. Regression check on Cronos (UE 5.4.4): same pick as before, works. ADVANCED.md: Example 3 + key docs.
  - Note: quitting cdb with `qd` from inside a `ba` command string ended the game process; detach with `bc *; .detach; q`.

- 2026-10-11 **GyroUE 0.6.19: dependency check before loading GyroLib (`deps.cpp`).** GyroLib (1.3.0, 1.3.1) links the VC++ runtime as
  DLLs (MSVCP140, MSVCP140_2, VCRUNTIME140, VCRUNTIME140_1; linker 14.51) and delay-loads SDL3.dll; Windows shares a DLL by name,
  so a copy the game already loaded, or one next to its exe, is what GyroLib gets. Standalone repro with 1.3.1 (static-CRT exe +
  gyrolib.dll + the demo's 14.24 runtime copies): `gl_sdl_create` segfaults, with the system 14.51 runtime it works (upstream
  repro). `deps::check_gyrolib()` (in `start_gyro()` before `LoadLibrary(gyrolib.dll)`) reads gyrolib.dll's linker version and
  imports (mapped as image resource), finds the copy of each runtime DLL it would get (already loaded > exe folder > System32)
  and reads its version from the module's version resource (not GetFileVersionInfo: version.dll is one of our proxy names).
  MSVCP* older than 14.40 with GyroLib built by 14.40+ = blocking (GyroLib not loaded, gyro off for the session, game starts,
  `deps:` log lines + 45 s notice naming the files and the fix: rename the game's copies, or install the Redistributable).
  Older than the build tools but 14.40+ = log warning only; SDL3.dll already loaded by the game = warning with both versions;
  otherwise one `deps:` info line per runtime DLL. Tested on the Severed Steel demo (blocked cleanly with 14.24 copies, works
  with them renamed).

- 2026-10-11 **GyroUE 0.8.0: SDL controller shown to the game as XInput** (branch `feature/xinput-translation`). Goal: games that
  only read XInput (UE's `XInputInterface`) and run without Steam Input (Epic: Luto) ignore a DualSense / Switch Pro except for the
  gyro. New `xinput.cpp` answers `XInputGetState` for one slot with the SDL state of GyroLib's selected controller
  (`gl_output.physical_id` / `gl_get_selected_device` -> `gl_get_endpoint` -> `gl_sdl_endpoint_for_instance` ->
  `SDL_GetGamepadFromID`), `XInputSetState` -> `SDL_RumbleGamepad` (refreshed every 500 ms, 1 s), `XInputGetCapabilities`,
  `XInputEnable`, `XInputGetStateEx` (ordinal 100, adds Guide 0x0400). State built on the game thread right after `gl_update`,
  read under a mutex by the hooks.
  - Findings: **Luto (UE 5.5.4) imports `XINPUT1_4.dll!XInputGetState/SetState` by name through the delay-load table**; `hooks.cpp`
    now also walks `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT` (an unresolved slot points to the linker stub inside the module; patched
    before the first call, original resolved later from `hooks::source_dll()`). The **Windows SDK's `xinput.lib` imports by
    ordinal** (xinput1_3/1_4: 2 GetState, 3 SetState, 4 GetCapabilities, 5 Enable, 100 GetStateEx), hence `hooks::add_ordinal`.
  - Only game modules are patched, so SDL in GyroLib never sees the virtual pad (no feedback loop); that is also why it's off in
    inline-hook mode (protected exes). UE only re-polls empty XInput slots after `WM_DEVICECHANGE`
    (`bNeedsControllerStateUpdate`), so we post `WM_DEVICECHANGE(DBT_DEVNODES_CHANGED)` to the game thread's `UnrealWindow`s when
    the pad appears or goes (whether needed isn't proven).
  - Settings (`GyroUE.ini`, profile overrides): `XInput` 0 off / 1 auto (default) / 2 always; `XInputNintendoLabels` (swaps A<->B,
    X<->Y unconditionally; the first version used `SDL_GetGamepadButtonLabel`); `XInputTouchpadBack` (touchpad click = View).
  - Auto mode keeps it off when the pad is Xbox-type or has a Steam handle, when any real XInput slot is connected (watcher thread,
    1 s), or when the game reads pads itself: `libScePad*.dll` or a non-GyroLib `SDL2/SDL3.dll` loaded, `RegisterRawInputDevices`
    usage page 1 / usage 4,5,8, `CreateFileW` on `\\?\hid#vid_<selected pad vendor>`, or `DirectInput8Create` (observed from game
    modules only). It waits 2 s after the pad appears. While translating it declares `GL_HOST_NATIVE_STICK_SUPPRESSION` and zeroes
    the right stick when GyroLib asks (`suppress_native_right_stick`, flick stick).
  - **Tested:** Luto, DualSense (Epic, no Steam Input): works, every input; both delay-load slots patched before the first call;
    shown 2.8 s after the SDL reader started; frame cost unchanged (idle ~58 us, active ~70 us). Switch Pro over Bluetooth: basic
    input works. **Terminator: Resistance** (UE4, Steam; `XINPUT1_3.dll` by ordinal in the regular table): works perfectly
    (DualSense). Decision: ship as **alpha with the default mapping**; extra buttons (DualSense mic, Edge paddles/Fn, Switch
    Capture; PS/Home via `XInputGetStateEx`) left unmapped.
- 2026-10-11 **Native pad libraries detected from import tables.** The Outer Worlds: Spacer's Choice (Epic, UE4,
  `IndianaEpicGameStore-Win64-Shipping.exe`, native DualSense) imports `libScePad.dll` **through the delay-load table**
  (`scePadInit/Open/ReadState/SetTriggerEffect`), so 0.7.0 could show the pad as XInput and then withdraw it. New
  `hooks::imported_dlls()` (regular + delay-load imports of game modules); auto mode treats an import of `libScePad*` or
  `SDL2/SDL3.dll` as native support (checked in DllMain and each watcher pass). **Tested (user, DualSense, Epic):** all good,
  `not shown: it imports Sony's pad library`, game's native input untouched, generic gyro works.
- 2026-10-11 **Hogwarts Legacy (Steam, UE 4.27, `HogwartsLegacy.exe`, UE4SS also installed), GyroUE 0.8.0.**
  - **Engine not readable:** GUObjectArray found (exe+0x97EB7E0) but `detect_layout` failed with `ue: core classes not found`. Added
    diagnostics (`ue: core classes missing: ...`, plus a 20 s notice after 60 s without readable engine data). Log: `9929 of 96600
    objects are classes (first: Object, p, i, e, s, r, i, ...)`: names decoded from wrong memory. **Root cause: a false FNamePool.**
    The exe image holds three qwords equal to name block 0; GyroUE took the first (rva 0x9754380: CurrentBlock 0, block 1 null,
    blocks 2+ set 0x20000000 apart); the real pool is at **rva 0x97AF290** (CurrentBlock 131, blocks every 0x20000). Fix in
    `find_name_pool`: a candidate must have every block up to CurrentBlock set and the next four null. Layout then detected
    (`SuperStruct=0x40 ChildProperties=0x50 Field.Class=0x8 Next=0x20 Name=0x28 ElementSize=0x3C Offset_Internal=0x4C
    sizeof(Property)=0x78`; FField 8 bytes larger than stock from Next on, detected at runtime).
  - **XInput:** the DualSense was shown at +3 s and withdrawn at +18 s when the game opened its HID device; the exe **imports
    `hid.dll`** (stock UE doesn't). Auto mode now treats a `hid.dll` import as native support.
  - PC `BP_Phoenix_Player_Controller_C` (ControlRotation +0x2B0), pawn `BP_Biped_Player_C`, view target
    **`BP_ThirdPersonCameraStackActor_C`** (gameplay). Follow check only judged when the view target is the pawn; `player_camera()`
    now also accepts a view target class whose camera turned 30+ degrees while the right stick was held (`camera: view target X turns
    with the right stick`). Calib stayed 0 (no menu calibration happened yet).
  - **Camera hunt:** probe picked `ViewTarget.CameraStackBehaviorConvertToTranslation_<n>` raw float rotator +0x1C8, but the number is
    per-session generated and the value is rewritten every frame (another output). Probe fix: a path step named `<Class>_<digits>`
    is labelled `@Class` when it resolves to the same object. Read-only `ReadProcessMemory` delta hunt (12 snapshots between stick
    turns, roots = camera stack, pawn, PC plus one pointer away): PC `ControlRotation` (+0x2B0) is an output;
    **`BehaviorStack` (class `CameraStack`, subobject of the view target, also view target +0x340) holds a float rotator at +0x68
    (P,Y,R) mirrored at +0x80**, about 3 degrees above the final pitch (the base rotation before offsets/smoothing; why exact-match
    probing missed it). One-shot proof (`WriteProcessMemory`): +45 yaw at +0x6C/+0x84 and +10 pitch at +0x68/+0x80 moved the camera and
    stayed. Profile `profiles/HogwartsLegacy.ini`: `Object=ViewTarget.BehaviorStack`, `Offsets=0x68,0x80`, `RotatorSize=12`.
    Lesson: a camera's state can be a smoothed/offset copy of the POV, so a delta-based scan finds what exact matching doesn't.
- 2026-10-11 **Probe with live test (0.8.0).** The passive probe accepted any rotation holding the camera's values outside known
  output locations, so outputs became the profile, while offset state was never seen. User's design: test every candidate by writing.
  - Candidates (`follows()`): a rotator that holds the POV's exact pitch/yaw **or changed by the POV's turn** (tolerance
    max(0.6, 3%) per axis), reflected or raw, float or double; `probe_groups()` groups raw rotators of one object+size, reflected
    one by one, usable first, passive "outputs" after.
  - Live test (`run_live_test`, Probe=1 automatic only), after 3 scored captures, up to 40 groups: camera at rest 600 ms, 250 ms null
    window (no drift > 0.2), write +3 yaw via the same `apply()` a profile uses; pass if the POV turns +3 (+-0.5) within 700 ms and holds
    150 ms, then undo and the camera must come back; then the same in pitch. Fail: `overwritten (output)` or `kept, camera didn't
    move`. Round 2 retests passes (a pass in round 1 only is `inconsistent`). Stick, pause, cursor, focus loss or another view
    target aborts and undoes (3 retries). A verified group becomes the profile (or `.suggested.ini` if the game had one), header
    `VERIFIED LIVE`; otherwise up to 8 captures, then only an unverified `.suggested.ini`. **An unverified guess is never written as
    the game's profile any more.** The gyro stays paused until the probe is done.
  - First run (Hogwarts) verified the wrong camera: `ViewTarget.Camera` (CameraStackCineComponent) raw 0x1B0,0x1D0 moved the camera but
    turns in place (no arm, free camera); BehaviorStack +0x68/+0x80 was missed because its offset to the POV depends on pitch. Fixes:
    groups need `max(2, (n+1)/2)` scored diffs; **orbit check**: each capture measures the POV location's movement for the turn
    (radius = chord / (2 sin(angle/2)), median logged); with radius >= 30 units (third person) the candidate's +3 yaw must also move
    the camera at least 40% of the expected chord, else `kTurnsInPlace` (first person skips). `ue::camera_pov_location`.
  - **Second run (Hogwarts, 3 captures, orbit radius median 205): correct** — `ViewTarget.BehaviorStack` raw 0x68,0x80 moves the camera
    in both rounds (10.7/10.8 for an expected 10.6); `ViewTarget.Camera` rejected (moved 0.0 of 10.6); outputs overwritten. The
    generated profile equals the hand-verified one. **Regression checks:** Severed Steel demo (first person, radius 0):
    `Pawn.FPCamera.RelativeRotation` (WorldRotation=1) ranks first, equals the verified profile. Cronos (UE 5.4.4, third person,
    doubles): `Pawn.View` raw 0x2A0,0x420 (RotatorSize 24) moves the camera (9.8 for 9.6), equals the verified profile. (One attempt
    ran the old 0.6.19 `version.dll` loaded first, one GyroUE per process; redeployed as `version.dll`.) The live-test probe
    reproduced all three verified profiles by itself.
- 2026-10-12 **Luto: flick stick and `XInputNintendoLabels` tested (user: works).** GyroLib flick stick (`context.1.flick.mode=2`) works
  with the translated pad: `GL_HOST_NATIVE_STICK_SUPPRESSION` advertised while shown as XInput, virtual right stick zeroed when
  GyroLib asks.
- 2026-10-12 **`XInput=2` keeps the game from reading PlayStation/Nintendo controllers itself** (before, double input in games with
  native DualSense). `libScePad.dll` (Pacific Drive, It Takes Two) imports exactly the `hid.dll` set that Hogwarts, Cronos,
  Banishers, Days Gone and Jedi: Fallen Order import from their exe (those most likely link Sony's library statically). With
  `XInput=2` the `CreateFileW` hook (installed in auto and always modes, also patched into a separate `libScePad*.dll` via
  `hooks::add_with_pad_library`) refuses HID paths of Sony (054C), Nintendo (057E) and the selected controller with
  `ERROR_ACCESS_DENIED`; `RegisterRawInputDevices` joystick/gamepad entries are dropped; Bluetooth HID paths (`_vid&0002054c_pid&...`)
  are parsed too. `XInput=2` also stays off while a real XInput controller is connected (DS4Windows/DSX/Steam Input/Xbox pad).
  **Tested (DualSense): Pacific Drive and Cronos work, no double input.** Bluetooth untested then.
- 2026-10-12 **Native flick stick (`NativeFlickStick=1`, off by default (user), `nativepad.cpp`).** GyroLib's flick stick in games with
  their own DualSense support without giving it up for `XInput=2`. Switching native/XInput at runtime rejected (the game keeps its
  HID handle; fake disconnect may pause; suppression changes with views/aim context). All eight native-DualSense games checked
  read through Sony's pad library, which opens with `CreateFileW` and reads with `ReadFile` + `GetOverlappedResult(Ex)` (It Takes
  Two's older `libScePad.dll` only `ReadFile`). `nativepad.cpp` owns the `CreateFileW` hook (moved from `xinput.cpp`;
  `nativepad::refuse_pads` / `opened_vendor`) and adds `ReadFile`, `GetOverlappedResult(Ex)`, `CloseHandle` (game modules +
  `libScePad*.dll`); Sony handles opened with read access followed (max 8); pending overlapped reads matched by `OVERLAPPED*`.
  Layouts: `0x01` RX/RY bytes 3-4, `0x31` (DualSense BT) 4-5 + CRC32 at 74, `0x11` (DS4 BT) 5-6 + CRC32 at 74 (CRC of 0xA1 + bytes
  0..73); reports whose CRC fails are left alone. The layout counts as checked when the report's right stick agrees with SDL's
  (byte*257-32768, tolerance 48) in >= 15 deflected frames (and >= 4x the misses); 30+ misses dominating -> never suppressed.
  - Pacific Drive first test: flick works but the stick still turned the camera: libScePad reads **640 bytes = ten 64-byte USB
    reports** per read and only the first was centred. Fix: the device's `InputReportByteLength` (`HidP_GetCaps` on the game's
    handle, fallback 64/78 by report ID) splits each read, every report centred. Second test: reads of 768 bytes (12 reports,
    ~490/s), 15/15 layout check, suppression follows GyroLib. **Cronos (Sony library linked, USB): works (user).** Bluetooth
    (`0x31`), DualShock 4 and the separate DLL over Bluetooth to verify.
- 2026-10-13 **Mortal Shell (Epic, UE 4.25+, `Dungeonhaven-Win64-Shipping.exe`): DualSense shown as a DualShock 4
  (`DualSenseAsDualShock4=1`, `nativepad.cpp`). Works USB and Bluetooth (user: almost every button checked).**
  - The game links an old Sony PC pad library (`WinDualShock`) that knows only the DualShock 4 (PID 05C4 / 09CC / 0CE6 in the exe);
    it opens the DualSense but ignores it. `XInput=2` can't help: UE's `XInputInterface::SendControllerEvents` never ran while its Sony
    plugin is active (corrected below).
  - What the library checks before accepting a DS4 (exe disassembly): `HidD_GetAttributes` PID; `HidP_GetCaps` Input >= 64, Feature
    >= 0x35; feature value caps must contain (page 0xFF80, usage 0x43, report 0xA3, count 0x30) and no (0xFF04, 0x27, 0x06, 0x34),
    a device field >= 0x13; feature 0x12 (address), 0xA3 (firmware 49: hardware version LE16 at 0x23 >= 0x3100), `SetFeature 0x14`,
    feature 0x02 (calibration 37). On rejection its cleanup frees uninitialised pointers (heap corruption 0xC0000374). The first crash
    also had GyroUE zero-filling the caller's whole stated buffer length: now only the DS4 report size.
  - Emulation: identity 054C:09CC "Wireless Controller"; caps 64/32/64 with 27 synthetic feature value caps; 0x02 from the DualSense's
    0x05, 0x12/0x81 address from 0x09, 0xA3 synthetic; SetFeature swallowed. Input: each 64-byte DualSense report rewritten in place
    as a DS4 USB report (sticks, buttons, triggers, IMU, timestamp /16, touch, cable/battery). Output: DS4 0x05 (motors 4-5, RGB
    6-8) -> DualSense 0x02 (48 bytes: enable bits, motors 3-4, RGB 45-47) in our own buffer. **Bluetooth:** the library accepts
    054C:05C4/09CC only with `HidD_GetAttributes` VersionNumber (decimal digits) >= 23 (Windows reports 0x0000 over BT), so we report
    0x0100; the game's buffers use the spoofed 64-byte input length while the device's is 78: we read into our own buffer and convert
    0x31 (USB layout shifted by one; 0x01 basic report also handled). Output 0x31, tag 0x02, effects one byte later, CRC32 seed 0xA2
    at 74; output length 547; feature reads use the device's feature length. Diagnostics kept: `input:` lines for Raw Input
    registrations, HID devices opened and Raw Input sources, first read of every followed HID handle.
  - **Learned, not a user setting (tested BT, no profile, auto):** first start showed the notice and the DualSense worked as an Xbox
    controller; GyroUE wrote `profiles\Dungeonhaven-Win64-Shipping.ini` (`[GyroUE] DualSenseAsDualShock4=1`) and the second start used it
    as a DualShock 4. Switching identity in the same session is a **dead end** (the Sony library only looks for controllers at startup,
    two passes 0.5 s apart, even with `WM_DEVICECHANGE`). Option C: `nativepad::dualsense_ignored()` = the game opened a DualSense,
    never read one, and has none open 5 s after the first open (a supporting library keeps its handle and reads at once: Pacific Drive
    0.5 s, Cronos 20 ms); with a Sony library among the imports (`xinput::sony_library()`, auto mode) the host writes the key into the
    profile, shows a 15 s notice (restart) and translates that DualSense to XInput for the session. The probe keeps the profile's
    `[GyroUE]` section when it rewrites it; a profile without camera views doesn't count as existing. Profile
    `profiles/Dungeonhaven-Win64-Shipping.ini`; generic camera confirmed (DX11, no panel).
  - **Auto mode: Sony-library signs only apply to PlayStation controllers.** `native_sign`: Sony (libScePad imported/loaded, hid.dll
    imported) blocks only vendor 054C; Any (own SDL, Raw Input gamepad registration, DirectInput, the game opening the pad's HID
    device) blocks every pad; the HID-open sign is ignored when a Sony library is present (its enumeration opens every HID device).
    **Jedi: Fallen Order (Steam), Switch Pro, `XInput=1`: works** (its Sony library opens 057E:2009 once while enumerating); the earlier
    DualSense-as-DS4 test there also worked.
  - **Correction: Mortal Shell does poll XInput** (UE's `XInputInterface`, call at exe+0xE7FA3B). `XInputTrace=1` (MinHook inline hooks
    on `XInputGetState/Ex` in every loaded xinput DLL, each calling module logged once) showed the exe calling `xinput1_3!XInputGetState`
    directly. Cause: **`EOSOVH-Win64-Shipping.dll` (EOS overlay) rewrites the exe's import slots for `XInputGetState` and
    `LoadLibraryW/A/ExW/ExA` ~2 s after start**, after we patched them. Fix: `hooks::verify()` (watcher thread, every second) checks every
    patched slot; a rewritten slot is logged once and, for XInput imports only, patched again (`patched again` / `left to it`; our hook
    keeps calling the real export, so EOS's XInput hook is bypassed). Any EOS (Epic Store) game can do this. Switch Pro `XInput=1` then
    worked perfectly.
- 2026-10-13 **Build: GyroUE 0.8.1** (user's version), `GyroUE-v0.8.1.zip`: DualSense shown as DualShock 4 (USB and Bluetooth), learned
  and saved in the profile; Mortal Shell profile; auto mode translates non-Sony controllers in games with a Sony library; XInput
  imports an overlay rewrites are taken back; diagnostics `XInputTrace=1` and `input:` log lines.
- 2026-10-15 **Refactor: engine-neutral core + engine modules** (still 0.8.2; names/layout unchanged). `src/GyroUE/` became
  `src/core/` (object library `gyro_core`, no include path to `engines/`: dllmain, proxy, hooks, overlay, xinput, nativepad, notify,
  deps, log, perf, new `engine.hpp/.cpp` (the `Engine` interface, `select_engine`), `settings.cpp`, `mem.cpp`, `util.hpp`, engine-neutral
  `host.cpp`) and `src/engines/unrealEngine/` (`ue.cpp`, `ue_engine.cpp`, `ue_probe.cpp`); `src/engine_registry.cpp` lists the modules;
  `src/CMakeLists.txt` holds the version. Engine-specific bits behind `Engine`: modular-build DLL prefix (`hooks::init(prefix)`), the
  window class receiving WM_DEVICECHANGE (`UnrealWindow`), the Probe= block of a new GyroUE.ini. Engine choice in DllMain: `Engine=<name>`
  in `[GyroUE]`, else the first module whose `detect()` matches (Unreal: `*-Win64-Shipping.exe`/`*-WinGDK-Shipping.exe`, or
  `..\..\Content\Paks` / `..\..\..\Engine\Binaries`), else Unreal; log `engine: Unreal (detected)`; `settings:` line split in two. The
  build script reconfigures when the cache came from another source folder. Verified: clean /W4 build, same 197 exports, same zip list;
  not tested in game.
- 2026-10-15 **Build: Gyro4All 0.9.0** (user's version): GyroUE renamed to Gyro4All on top of the split. `Gyro4All-v0.9.0.zip`. Renamed
  everywhere except history and the GitHub URLs (repo keeps the name gfgerard/GyroUE for now, user's decision): install folder
  `Gyro4All\`, `Gyro4All.ini`, logs, INI section `[Gyro4All]`, notices, version resource, session mutex `Local\Gyro4All-<hash>`,
  namespace, macros, scripts `tools/{build,package,deploy}-gyro4all.sh`, zip name. The DLL is still `winmm.dll`/`version.dll`.
  Compatibility: `settings::read` takes a key from `[Gyro4All]`, else `[GyroUE]`; writes go to `[Gyro4All]` (wins); the probe keeps both
  sections. `settings::migrate_from_gyroue` on start copies `GyroUE\GyroUE.ini` -> `Gyro4All\Gyro4All.ini`, `gyrolib.ini` and
  `profiles\*.ini` (never over existing files) then renames `GyroUE\` to `GyroUE.migrated` (log `settings: GyroUE folder found`).
  Gyro4All also takes GyroUE's mutex (`Local\GyroUE-<hash>`) so an old GyroUE proxy under the other name stays a plain proxy;
  `deploy-gyro4all.sh` recognises both DLLs and `remove` keeps `GyroUE*` folders. Migration verified with a scratch test (10/10);
  not tested in game.
- 2026-10-15 **RE Engine recon: Resident Evil 7 biohazard (Steam)**, first non-Unreal target (static analysis; nothing installed).
  - **Build**: `re7.exe` 1.0.0.7, PE timestamp 2026-03-24, the post-2022 "RT"/DX12 build. Engine version = **type database (TDB)**
    version: header `TDB\0` + u32 version in the exe's `.data`. **RE7 = TDB 70** (pre-RT DX11 build was 49; RE2/RE3 RT 70, RE8 69,
    RE4/SF6 71, DD2 73, MHWilds 81, RE9 83, per REFramework `TDBVer.hpp`/`GameIdentity.cpp`). Layout of types/fields/methods depends
    on it. **Protection**: no Denuvo/Enigma; `.bind` section = SteamStub. No anti-cheat. To check: IAT hooks from DllMain survive
    SteamStub.
  - **Loading**: imports WINMM (2), VERSION (3), DINPUT8 (REFramework's proxy name), XINPUT1_3 (4), `USER32!PeekMessageW`, dxgi, d3d11;
    D3D12 dynamic; HID.DLL + SETUPAPI (native PlayStation pad); delay-loads steam_api64/openvr_api. So the standalone `winmm.dll` proxy and
    the per-frame `PeekMessageW` hook should work; **REFramework is not needed** (MIT, a usable reference like UE4SS; studied at commit
    d052e8f).
  - **Reflection (UE equivalents)**: TDB found by `TDB\0` + version; managed singletons (`REGlobals.cpp`: `48 8D ? ? ? ? ? 48 B8 00 00 00
    00 00 00 00 80`, fallback TDB types deriving from SingletonBehavior); native singletons (`via.SceneManager`, `via.Application`); VM
    context (`REContext.cpp`: `48 8B 0D ? ? ? ? BA FF FF FF FF E8`). Gameplay objects are `via.GameObject` with component lists.
  - **RE7 camera (REFramework `re8_vr.lua`, `RE7.lua`, `RE8VR.cpp`)**: local player `app.ObjectManager` (managed singleton) field
    `PlayerObj`; camera component `app.PlayerCamera` (DLC: `app.CH8PlayerCamera`, `app.CH9PlayerCamera`), updated in `lateUpdate`.
    Fields: `<CameraRotation>k__BackingField` (quaternion, no shake), `CameraRotationWithMovementShake`, `CameraRotationWithCameraShake`,
    `PrevCameraRotation`, `CameraCtrlParam.CameraRotation`, `BaseTransSolver` (`CurrentController` -> `<rotation>k__BackingField`
    quaternion, `IsVerticalRotateLimited`; `CameraControllers[]`; `<currentType>k__BackingField`, 0 = MaximumOperatable = player has the
    camera, else scripted/cutscene). Upper-body pitch: `app.PlayerUpperVerticalRotate.doLateUpdate`; pad `app.Pad.update`. The TDB string
    pool also has PSVR plumbing (`ExternalCameraRotation(Enable)`, `<TrackingRotation>k__BackingField`, `IsTrackingApplyDisable`,
    `HmdTrackingController`, `GamePadTrackingController`, `IsVRMode`) and native pad types (`DualSense`, `DualSenseDevice`,
    `PSGamePadDevice`, `DualShock4RemotePlay`). Nexus RE7 mod 166: separate H/V sensitivity, ~25% stick deadzone. No existing gyro mod found.
  - **Plan**: offline `tdbdump` (TDB 70 parser after REFramework's `RETypeDB.hpp`); `src/engines/reEngine/` (detect: `re_chunk_000.pak` +
    `TDB\0`; TDB reader, singleton lookup, scene = ObjectManager.PlayerObj -> `app.PlayerCamera`); apply = rotate the controller's
    persistent `<rotation>` quaternion (yaw about world up, pitch about local right), verifying which field the game integrates the stick
    into, and test `ExternalCameraRotation` as an additive alternative; gate on `currentType != 0` and pause via `via.Application`/GUI.

- 2026-10-15 **`tools/tdbdump.py`: offline RE Engine TDB dumper** (Python stdlib; reads the exe only). TDB 69/70 first (layouts from
  REFramework): header `tdb70::TDB` (string pool pointer at 0xD0; 0xC8 in TDB 69), `RETypeDefVersion69` 0x50 bytes with 18-bit ids,
  `RETypeImpl` 0x30, `REField` 8 bytes (declaring:18, impl_id:20, **offset:26**), `REFieldImpl` 12, `REMethodDefinition` 16,
  `REMethodImpl` 12; string offsets masked with the next power of two of the pool size minus 1. **On disk the TDB is not
  initialised** (`initialized` = 0 at +8; table pointers are header-relative; method code pointers and `managed_vt` are 0), so offline
  gives names, types and field offsets only; the runtime module reads the initialised TDB. RE7 (TDB 70): 78687 types, 147763 fields,
  507723 methods; names match REFramework's RE7 scripts. Field base = int32 before the managed vtable (see below).
  - **RE7 camera objects (field offsets from the field base)**: `app.ObjectManager` (<- `app.SingletonBehavior`1`, static `_Instance`):
    `PlayerObj` +0x38. `app.PlayerCamera` (<- `app.DoomsBehavior` <- `via.Behavior`; DLC `CH8PlayerCamera`/`CH9PlayerCamera`):
    `<CameraPosition>` +0x20, `<CameraRotation>` +0x30 quat, `<CameraFov>` +0x40, `<OperatedCameraRot>` +0x60, `<DesiredCameraRot>`
    +0x70, `<inputHorizontal/VerticalRotationSpeed>` +0x84/+0x88, `<deltaHorizontal/VerticalRotation>` +0x8C/+0x90, `<IsControlledByAnimation>`
    +0x94, `Camera` +0xA8, `_CameraJoint` +0x1A0, `_CurrentXRotateLower/UpperLimit` +0x1B0/+0x1B4, `AdditionalHorizontal/VerticalRotation`
    +0x2A4/+0x2A8, `CameraRotationWithMovementShake` +0x490, `...WithCameraShake` +0x4B0, `IsGunAim` +0x4D0, `CameraCtrlParam` +0x538,
    `BaseTransSolver` +0x540, `CameraType` +0x548, `PrevCameraRotation` +0x570. `BaseTransformSolver`: `<rotation>` +0x10 quat,
    `<currentType>` +0x20 (`CameraTypeEnum`: MaximumOperatable, PivotRotation, ElasticRotation, ElasticRotationForCounter, HalfAnimation,
    HalfAnimationNoResetAngle, FullAnimation, InterpRotation2FullAnim, Return2MaxOperatable, QuickTurn), `CameraControllers` +0x30,
    `CurrentController` +0x50. `CameraController`: `<rotation>` +0x10 quat, `<weight>` +0x20, `<state>` +0x24, `XRotateLowerLimit/Upper`
    +0x74/+0x78, `IsVerticalRotateLimited` +0x7C. `CameraControllerParam`: `CameraRotation` +0x40, `HorizontalRotationSpeed` +0x90.
    `app.PlayerUpperVerticalRotate`: `CameraRotation` +0x250 (body follows the camera; not an input). `ExternalCameraRotation`/
    `TrackingRotation` are VR-only, not a gameplay input path.
  - Apply candidates planned: (a) `AdditionalHorizontal/VerticalRotation`; (b) rotate `CurrentController.<rotation>` when
    `currentType` is MaximumOperatable (chosen, works); (c) `delta*Rotation` (recomputed each frame; needs a hook).

- 2026-10-15 **RE7 first install (Gyro4All 0.9.0, `winmm.dll` next to `re7.exe`; saves backed up as `re7-saves`).** The proxy loads despite
  SteamStub; import-table hooks work; XInput hooked; overlay captured the D3D12 swapchain (ResizeBuffers from two threads). **The gyro
  frame never ran:** RE Engine's main thread (the DllMain thread, window owner) sits blocked in `GetMessageW`; the game loop runs on another
  thread, so no empty-queue `PeekMessageW` ever happens there (the `PeekMessageW` source is Unreal-specific). REFramework hooks the
  `via.Application` entry functions (`sdk/Application.cpp`: table by `89 81 ? ? ? ? 48 8B ? 48 81 C1 ? ? ? ?`, stride 0xD0 below TDB 74).
  - **RE Engine module step 1: the frame hook (works).** Core: `Engine::frame_source(FrameFn)` (true = the engine calls the gyro frame
    itself on its game thread; the core skips its `PeekMessageW` hook; same 0.5 ms limiter and perf; default false). `src/engines/
    reEngine/` (`gyro_re`, `Engine=RE`; detect = `re_chunk_000.pak` next to the exe): `re_tdb.cpp` finds the TDB header by `TDB\0` + version
    in writable sections (RE7: `re7.exe+0x7208840`); **the header's `initialized` field stays 0 in RE7**, so "ready" = types table is an
    absolute pointer. `via.Application` = type 49307; its instance via the runtime type's vtable[1] getter (`REType` at typedef+0x40) from
    a startup thread under `__try`; the entry table found by shape at instance+0x100..0x1000 (stride 0xD0, name +0x18, priority u16 +0x20,
    function +8; must contain BeginRendering and EndRendering). RE7: **301 entries**; gameplay ones `UpdateHID`(154), `PreupdateBehavior`
    (181), `UpdateBehavior`(187), `UpdateMotion`(200), `LateUpdateBehavior`(214), `PrepareRendering`(222), `BeginRendering`(245),
    `EndRendering`(268). Hook = swap the entry's function pointer; `FrameEntry=` picks it (default `LateUpdateBehavior`, original
    `re7.exe+0x5AEAF60`). In game (RE7, DualSense): TDB ready 0.5 s after start, first gyro frame on the game update thread, GyroLib 1.3.1
    loads, SDL 470 samples/s, DX12 panel ready.
  - **Step 2: scene, camera, controller.** Singletons via REFramework's REGlobals pattern over the exe's code (`48 8D ?5 disp32 48 B8 00..00
    80`): **145 static slots** in RE7; a slot's object type = `*(*(obj))` compared with the TDB types table; `app.ObjectManager` at
    `re7.exe+0x8FE9418` (filled ~2 s after start). **Field base = int32 just before the object's REObjectInfo: 0x50 for behaviours (not
    0x48 as ReClass suggested), 0x10 for plain objects**; native components vary. Player: `ObjectManager`+0x38 `PlayerObj` -> `transform`
    at +0x18 -> components chained via `REComponent` +0x18 (owner +0x10); ~85 components on Ethan. `<CameraRotation>`, the solver's
    `<rotation>` and `CurrentController.<rotation>` are the same quaternion. Free look: solver `currentType` 0, controller
    `app.PlayerCamera.MaximumOperatable`; scripted scenes: `ElasticRotation` (2), `FullAnimation` (6, IsControlledByAnimation=1),
    `InterpRotation2FullAnim` (7). Quaternion forward +Z, up +Y; **positive yaw (atan2(fx,fz)) turns left, positive pitch looks up**.
    **Write tests: rotating `CurrentController.<rotation>` turns the camera and stays** (the game rebuilds the others each frame); yaw =
    world-up rotation pre-multiplied, pitch = local-X post-multiplied; the game clamps pitch to -58/+75, but a rotation past the pole flips
    yaw 180, so we clamp first. `AdditionalHorizontal/VerticalRotation` stayed 0. Module: view 1 = free look; paused =
    `app.DoomsBehavior.IsPause` (+0x1); recenter levels pitch; status `camera=<controller> type= anim= aim= pause= yaw= pitch=`.
    **User-verified (Steam Input off): controller and gyro work, direction right on both axes, gyro stops in pause and cutscenes.**
  - **Controller:** RE7 creates DirectInput and imports HID.DLL but reads neither the DualSense nor the DualSense shown as a DualShock 4;
    auto mode hid the pad and then learned `DualSenseAsDualShock4=1`, which the game also rejected (no controller with Steam Input off).
    Shipped `profiles/re7.ini`: `XInput=2`, `DualSenseAsDualShock4=0`. Static analysis of `re7.exe`: native support for the **DualShock 4
    only, found through DirectInput**: one routine (`re7+0x29051D0`) walks HID interfaces, opens each, keeps the one whose
    HidD_GetAttributes VID/PID equal a DirectInput device's (`IDirectInputDevice8` `DIPROP_VIDPID`), then flags it a DS4 only if DirectInput
    vendor is 054C and the HID caps have the DS4 descriptor's shape (input >= 0x40, output >= 0x20, feature >= 0x40, >= 9 input value caps,
    >= 45 feature value caps, ...). So `DualSenseAsDualShock4=1` can't pass (DirectInput still says 0CE6); a DS4 emulated by another program
    doesn't work either. Faking DirectInput VID/PID and caps for PlayStation prompts only: not worth it; RE7 stays on `XInput=2`.
  - **Core flaw it showed:** auto mode learned `DualSenseAsDualShock4=1` from "Sony library + DualSense closed unread" and never undid it.
    Fix: `learn_dualsense` also writes `DualSenseAsDualShock4Learned=1` and learns nothing when the profile already has the key;
    `nativepad::learned_ds4_rejected()` (opened, closed unread, none open 5 s later) makes `xinput.cpp` show the DualSense as XInput
    regardless of native signs, and `unlearn_dualsense` writes `=0` and removes `Learned`. Values set by the user or a shipped profile are
    never changed. Tested in RE7 (XInput shown 5 s after start, profile rewritten to `=0`).
- 2026-10-15 **Build: Gyro4All 1.0.0** (user's version: first engine besides Unreal). `Gyro4All-v1.0.0.zip` (8 profiles incl. `re7.ini`).
  New since 0.9.0: the RE Engine module, `profiles/re7.ini`, learned DS4 identity taken back when rejected, `tools/tdbdump.py`; docs for
  the RE Engine install folder, RE7, `FrameEntry=`, CREDITS (REFramework layouts).
- 2026-10-15 **RE Requiem (`re9.exe`, Steam app 3764200): TDB 83 and its player camera** (not released). Saves backed up as `re9-saves`.
  The user had REFramework (`dinput8.dll`), ReShade + RenoDX, Ultimate ASI Loader as `version.dll` (RTX40MFG.asi), DLSS-G -> FSR3, DSX.
  - **Protected exe**: sections `.xtext`, `.xtls` (333 MB, the real code), `.trace`, `.sbss`; the import table is partly a decoy. Import-table
    hooks still work for kernel32 (nativepad sees the game read the DualSense) but not for graphics: the game never calls the
    `dxgi.dll!CreateDXGIFactory2` import, so the overlay never saw a swapchain.
  - **TDB 83 layouts** (ported to `re_tdb.cpp`/`tdbdump.py`): header counts num_types +0x08, num_string_pool +0x58; tables types +0x68,
    typesImpl +0x70, methods +0x78, methodsImpl +0x80, fields +0x88, fieldsImpl +0x90, stringPool +0xD8; type def 0x50 with 19-bit ids,
    runtime type at **0x38** (69/70: 0x40), managed_vt 0x40; names packed 28+28 bits; REField declaring:19 impl_id:19 field_typeid:19;
    REFieldImpl flags:15, offset:26, name:28; methods 0xC bytes, no code pointer; `via.Application` entries from TDB 74: stride 0xC8, name +0x10,
    priority +0x18. Field base and native layouts unchanged. 178180 types. In game: TDB ready 1.3 s after start, **338 entries**
    (two `UpdateBehavior`; the hook takes the first), frame hooked before `LateUpdateBehavior` (`re9.exe+0x2A2220`). **REFramework coexists**
    (swaps the same entries 4 s later, calls ours). The game reads the DualSense itself (report 0x01): no XInput translation.
  - **Native gyro:** `app.CameraInputSubSystem` (`_IsMotionSensor`, `_MotionSensorYaw/PitchDiff`), `CameraInputUserData._MotionSensorSensitivityYaw/
    Pitch`; on PC it works only while aiming (L2); Gyro4All's gyro is off then (ADS controller): no doubled motion.
  - **Camera:** `app.CameraSystem` (`app.AppSingleton`1`, `re9.exe+0xE9A16A8`, 148 slots) -> `_CameraBlenders` -> the `_Role` 0
    `app.CameraBlender` -> `<BusyCameraController>` (`PlayerTPSCameraController` / `PlayerFPSCameraController`; others ADS, Action, Event,
    Fixed, Gimmick, Grapple, Bike, DetailSearch, PhotoMode, ConstTarget). The player controllers' GameObject has an
    `app.PlayerCamera{TPS,FPS}ActionInterpolation` component -> `_RotationInterpolation` (`_PitchRangeDeg` = via.Range -60..60) ->
    `_NextInfo` (`PlayerCameraRotationCalc`) -> **`<CameraRotation>k__BackingField`**: a live write (yaw +20, pitch +15) turns the camera
    and stays. Pause: `app.PauseManager` slot `re9.exe+0xE998348`, `_CurrentPauseTypeBits` (0x1 loading; 0x80 / 0x2 seen briefly; 0x2 matched
    the moments GyroLib's panel had the controller). **Pitch sign is the other way round from RE7**, yaw the same. Module: camera models per
    family (`re_camera.hpp`: `re7_camera.cpp`, `requiem_camera.cpp`), the first whose classes are in the TDB is used (`re: camera model: ...`);
    per-model pitch sign. **User-verified third and first person** (recenter not tested).
  - **Panel at first not available:** `OverlaySystemFactory=1` (hook the real System32 DXGI factory) got the swapchain but with DLSS frame
    generation the game crashed 2 s in Streamline (`sl.interposer` -> `sl.dlss_g` -> `sl.common`), and without it hung (REFramework and
    Streamline hook the same real swapchain). Solved next day:
- 2026-10-16 **RE Requiem: the settings panel works, through Streamline's factory** (automatic, `overlay.cpp`). In import-table mode
  Gyro4All always hooks Streamline's factory when `sl.interposer.dll` loads; the first Streamline factory decides: if the game's own dxgi
  factory import was called before, the panel stays on the import (UE games, user-verified with frame generation on and off); else it uses
  Streamline's (`overlay: the game makes its DXGI factory through Streamline ...`). Protected exes (inline mode) keep the system factory.
  `OverlaySystemFactory=1` became the undocumented diagnostic `OverlayFactory=` (`streamline`, `game`, `system`).
  - Path: `LdrRegisterDllNotification` (or already loaded) -> MinHook on the interposer's `CreateDXGIFactory`, `...1`, `...2`,
    `slUpgradeInterface` -> `hook_factory` on the proxy factory -> `CreateSwapChainForHwnd` returns Streamline's proxy swapchain, which gets
    the usual hooks. The game calls the interposer's `CreateDXGIFactory2` then `...1`.
  - **Dead end 1, device mismatch:** `DX12 init failed (-1): Queue/swapchain device mismatch` (the queue is a Streamline proxy on a proxy
    device while the proxy swapchain's `GetDevice` returns ReShade's). Fix (any mode): when devices differ, try the native objects under
    Streamline's proxies (`slGetNativeInterface`) and use the first matching pair (proxy swapchain + native queue); Present is still hooked on
    the proxy, so the panel draws above frame generation.
  - **Dead end 2, crash toggling frame generation:** the game creates a new swapchain for the same window while we and GyroLib still held the
    old one; `sl.dlss_g` -> `sl.common` crashed in `CreateSwapChainForHwnd`.
  - **Dead end 3, release just before the new swapchain:** fixed OFF -> ON, but ON -> OFF froze (`0x80070005` E_ACCESSDENIED: the window still
    has a swapchain; the game retried every ~12 ms and waited on an error dialog hidden behind its window, seen with `cdbX64 -pv`). With the
    panel off ON -> OFF works: with the panel the proxy outlived the unloading of frame generation.
  - **Fix** (Streamline path only): hook `Release` (slot 2) of the captured proxy swapchain; when its count falls to our references plus
    GyroLib's (ours 2, GyroLib's 1), the game has let go: shut the renderer down and release ours (`the game released its swapchain: ...`).
    `slGetNativeInterface` AddRefs what it returns. The default path holds old swapchains the same way (worth the same fix if a game
    recreates its swapchain). **User-verified** (DualSense, report 0x01): frame generation off/on, ON->OFF and OFF->ON toggles, panel works
    each time, gyro turns the camera, quit normally; `system` hung with FG off.
- 2026-10-16 **Pragmata (`PRAGMATA.exe`, RE Engine, TDB 83): crash ~32 s after the frame hook, Capcom anti-tamper** (Gyro4All alone, no
  REFramework). TDB 83 ready (119105 types), 359 entries, frame hooked before `LateUpdateBehavior` (`PRAGMATA.exe+0x24400`), panel on
  Streamline's factory, then `EXCEPTION_ILLEGAL_INSTRUCTION` at `PRAGMATA.exe+0xCB07FF0`: the last 16 bytes of the RWX `.impdata` section,
  holding `0F 0B` (**UD2**), a planted gadget reached from a job dispatch; no Gyro4All frame on the stack. Sections: `.xcode`, `.sdata` (IAT),
  `.idata` (173 MB RWX, obfuscated code), `.impdata`, `.xtext`, `.ecode`. This is the anti-tamper penalty REFramework's
  `IntegrityCheckBypass.cpp` describes for RE9 (random scheduler job pointers replaced with UD2 gadgets; also a PE-header check and a renderer
  heartbeat): **RE Requiem was only ever tested with REFramework installed**, whose bypass hid it; Gyro4All has never run alone in a
  Capcom-protected RE Engine game. Unknown which write trips it (IAT patches or the entry pointer swap). With an older REFramework (2026-04-18,
  partial bypass: `Could not find thread scheduler corruptor` ...) it crashed ~7 s in; **with a newer REFramework (2026-10-11) the game starts
  and runs, frame hook and F10 panel work**.
  - **Bug: the panel didn't idle the game's pad.** The game calls `CloseHandle(NULL)`; `find_device(nullptr)` matched a free slot, so `untrack`
    dropped the device count to 0 with the DualSense handle open. Fix in `nativepad.cpp`: `find_device` never matches nullptr/INVALID_HANDLE_VALUE.
  - **Camera (TDB offline):** `app.CameraSystem` `_CameraBlenders` +0x28; `CameraBlender` `_Role` +0xC0, `_BusyCameraKind` +0xB8,
    `_BusyCameraController` +0x70 (plain field; Requiem's is a backing field). One `app.PlayerCameraController` (no TPS/FPS split, no
    `PlayerCameraRotationCalc`): `_Blender` +0x188, `_CameraPadInputUpdater` +0x190, `_CameraAngleLimit` +0xE8 (`_DefaultPitchLimit` +0x08),
    `_CameraInputGyro` +0xF0; from `app.CameraControllerRoot`: `<CameraObjectRotation>k__BackingField` +0x10, `<CameraJointRotation>` +0x30,
    `_IsBusy` +0x66. Pause: `app.PauseManager._CurrentPauseTypes` +0x58. Native gyro `CameraInputGyro` (`_EnableOption`, `_SensitivityH/VOption`):
    works only while aiming (L2), a player option; decision: the player turns the game's gyro option off to use Gyro4All's (no doubled motion).
- 2026-10-17 **Per-engine tool folders** `tools/<engine>/` with READMEs: `tools/reEngine/` (`tdbdump.py`, new `camera_finder.lua`),
  `tools/unrealEngine/` (README only). **`camera_finder.lua`** is an automatic camera finder for any REFramework-supported game (like the
  Unreal probe): scan a few thousand objects per frame (cap 60000) from managed singletons and the primary camera's GameObject through fields,
  arrays, `List`1`, GameObject components and component owners, keeping `via.Quaternion` and yaw/pitch/angle/rot floats; 3 captures with the right
  stick (yaw change > 25 degrees, 30 still frames), keeping rotations whose yaw changed like the primary camera's transform (within max(2, 8%));
  live test queued to `re.on_pre_application_entry("LateUpdateBehavior")`: +5 yaw must move the camera 5 +- 1.5, then +5 pitch gives `PitchSign`
  (camera angles from the primary camera's transform assuming it looks down -Z). Output: menu, REFramework log,
  `reframework\data\gyro4all_camera.txt`.
- 2026-10-17 **Pragmata: camera found with `camera_finder.lua`, model `pragmata_camera.cpp`.** The main blender's controller is
  `app.PlayerCameraController`; a write to its `<CameraObjectRotation>k__BackingField` turns the camera and stays. First guessed pitch +1
  from a quaternion diff, **wrong: user-verified pitch was inverted, the sign is -1 like RE Requiem's** (don't infer the sign from a quaternion
  diff, check on screen). Model: `CameraSystem` -> `_CameraBlenders` -> `_Role` 0 -> `_BusyCameraController`; free look when it is an
  `app.PlayerCameraController`; pitch range `_CameraAngleLimit._DefaultPitchLimit` (game: -70..80); pause = `_CurrentPauseTypes` != 0 (0x2 brief
  moments, 0x40 pause menu and while the F10 panel is open). `resolve` needs the plain `_BusyCameraController` field so it never takes Requiem.
  Log: `re: camera model: Pragmata`, slots `PRAGMATA.exe+0xB9BCC90` (CameraSystem) and `+0xB9AD318` (PauseManager) ~6 s after start; controllers
  seen `TitleCameraController`, `ActionCameraController`, then `PlayerCameraController` (`gyro on`). **User-verified** (DualSense, REFramework
  installed): yaw and pitch right; the F10 panel now idles the game's pad (~21,000 reports); **aiming (L2) keeps `PlayerCameraController`**, so
  the game's own gyro option must be off (GAMES.md says so). Pragmata moved to Supported games.
- 2026-10-17 **RE8 (Village): no controller in the first session; core fix in auto XInput.** RE8 (TDB 69, no profile, `XInput=1`) behaves like
  RE7 (DirectInput, HID, opens the DualSense twice and closes it unread). Learned `DualSenseAsDualShock4=1` at +8 s but the pad stayed hidden:
  in `xinput::frame` the `Any` sign (DirectInput) was checked before the Sony branch and only a *rejected learned* DS4 identity skipped it. Fix:
  a Sony-vendor pad with `nativepad::dualsense_ignored()` also skips the `Any` sign (shown as XInput in that session). Expected next start: DS4
  identity rejected like RE7, `unlearn_dualsense` writes `=0`; then ship `profiles/re8.ini` like re7.ini.
  - **RE8 camera (TDB 69, tdbdump):** RE7's camera with renamed fields. Player = `app.PropsManager.<Player>k__BackingField` (via.GameObject; no
    `ObjectManager`/`PlayerObj`) -> component `app.PlayerCamera`: `<cameraRotation>` +0x30, `<IsControlledByAnimation>` +0x94, `<isGunAim>` +0x1c8,
    `BaseTransSolver` +0x548, `CurrentXRotateLower/UpperLimit` +0x278/+0x27c. Solver: `<rotation>` +0x20, `<currentType>` +0x30,
    `<CurrentController>` +0x68. Controller: `<Rotation>` +0x10, `XRotateLowerLimit/UpperLimit` +0x78/+0x7c, `GyroAdjustment`/
    `BaseDeviceOrientation` (RE8 has gyro code of its own). Third-person mode `app.PlayerCameraTPS` (Shadows of Rose / TPS option).
  - **`camera_finder.lua` runs on RE8 (REFramework v1.5.9.1 +514 then v1.5.9 +7):** run 1 visited 82 objects, 0 rotations (empty
    `sdk.get_managed_singletons`; the camera rotations aren't on the primary camera's GameObject but on the **player's**). The REFramework Lua has
    neither `sdk.get_managed_singletons` nor `sdk.get_tdb`: the script now asks `sdk.get_managed_singleton` by name (`app.PropsManager`,
    `app.ObjectManager`, `app.CameraSystem`, ...) and asks every GameObject for known camera components by type (first 30 logged). Run 3 found
    singletons `app.PropsManager`, `app.GameObjectManager`, GameObjects `MainCamera` and **`pl1000`** (player, 65 components), but only 72
    objects visited: a **script bug** (the queue was popped by `queue[head] = nil` and tested with `#queue`, undefined with leading nil holes; fixed
    with an explicit `tail`). Run 4, **first complete automatic run**: 2351 objects, 526 rotations, 10 kept after captures; live test
    **CONFIRMED `primary camera <GameObject>{app.OnPauseCameraCaller}.playerCamera.BaseTransSolver.<CurrentController>k__BackingField.<Rotation>
    k__BackingField` (`app.PlayerCamera.MaximumOperatable`), PitchSign=1**: RE7's rotation with RE8's names, same sign as RE7.
- 2026-10-17 **RE8 camera model: `re7_camera.cpp` now covers Resident Evil Village.** Each field is looked up by RE7's name, then RE8's
  (`either`). Player: `app.ObjectManager.PlayerObj`, else `app.PropsManager.<Player>k__BackingField`. Free look: controller is
  `MaximumOperatable` and not `IsControlledByAnimation`; RE7's `currentType == 0` test only when `app.PlayerCamera.CameraTypeEnum` exists.
  Pause (no `IsPause`): `app.HIDVibrationManager.IsOldPause` (+0x31) / `IsOldPartialPause` (+0x32). In RE8 `app.PlayerCamera` is **not** a
  component of the player (`pl1000`); reached from `app.PropsManager.<Camera>` (via.Camera) -> its GameObject (+0x10) -> component
  `app.OnPauseCameraCaller` -> `playerCamera` (checked to be a PlayerCamera; re-looked up when the GameObject changes). Slots:
  `app.PropsManager` `re8.exe+0xC4C33B8`, `app.HIDVibrationManager` `+0xC4F3E30` (163 slots). **User-verified (REFramework v1.5.9 installed):
  gyro, pause and inventory work**; the learned profile was rewritten to `DualSenseAsDualShock4=0` (RE8 rejects the DS4 identity like RE7);
  shipped `profiles/re8.ini` has only `DualSenseAsDualShock4=0` (auto XInput, not `XInput=2`). GAMES.md: RE8 Supported. Not covered:
  third-person mode (`app.PlayerCameraTPS`); RE8 without REFramework untried.
- 2026-10-17 **Build: Gyro4All 1.1.0** (user's version), `Gyro4All-v1.1.0.zip` (10 profiles incl. `re8.ini`). Since 1.0.0: Pragmata and Resident
  Evil Village supported; nativepad `CloseHandle(NULL)` fix (panel idles the pad again); auto XInput shows a DualSense the game's Sony library
  ignored in the same session even when the game creates DirectInput (RE8); GAMES.md; `tools/<engine>/` research tools.

- 2026-10-17 **Monster Hunter Wilds (demo): TDB 81 added; camera found; left unfinished by decision.** (Online multiplayer: test solo only.)
  - 1.0.0 stopped at `TDB 81 is not supported yet` (`MonsterHunterWilds.exe+0xCA613E0`). **TDB 81 layouts** (`kHeader81` in `re_tdb.cpp`,
    `HEADERS[81]` in `tdbdump.py`): TDB 83 minus two things: numStringPool at 0x54 (83: 0x58; padding u32 before the pointers), and
    `RETypeImpl` names are two int32 (name 0, namespace 4) as in 69/70 (83 packs them 28+28). Everything else is the 83 shape (0x50 type defs,
    19-bit ids, `REMethodDefinition` 0xC, entries stride 0xC8). `type_names` reads packed names only from TDB 83; `tdbdump.py` prints nameless
    array types (TDB 71+) as `<element>[]`. Demo: 293888 types, 629970 fields, 1834223 methods.
  - **Camera classes:** `app.CameraManager` (<- `ace.CameraManagerBase`, static `_Instance`): `_MasterPlCamera` +0x60
    (`app.PlayerCameraController`), `_IsEventCamera` +0x140. `PlayerCameraController`: `_Operator` +0x1C0 (`cPlayerCameraOperator`:
    `_RotAmount` +0x0, `_PadInput` +0x20, **`_GyroInputAmount` +0x30** (the game's own gyro path), `_MouseRotateAmount` +0x40, all vec2),
    `_AutoRotator` +0x1C8, `_MiniComponents` +0x90; base `ace.CameraControllerBase`: `BaseCamParam` +0x10, `AddCamParam` +0x18, `DirectCamParam`
    +0x20, `CurrentParam` +0x28 (`ace.cCameraParam`: `RotationDegree` +0x20 vec2 degrees, `FOV` +0x4C, `<ParamArgument>` +0x30). Pitch range
    `_BasicParam._PitchDegreeRange` +0xF4 (default -80..80, measured -75..75); pause `app.PauseManager._ActivePause` bitset.
    The finder only kept quaternions and yaw-named floats, so it never saw `via.vec2` angles; `camera_finder.lua` now keeps every `via.vec2` as
    two candidates (`.x`, `.y`) and the live test writes +5 degrees to one component.
  - **Controller:** with the game's native DualSense and Steam Input off for PlayStation pads, DirectInput is created and the pad opened but
    never read; XInput was never called (XInput translation shown, `XInputGetState` never called). The controller works in game **only with
    Steam Input on**; then SDL sees no pad (Steam lists the DualSense in `SDL_GAMECONTROLLER_IGNORE_DEVICES`, the game uses the Steam Input
    API in-process, and the removal of that variable did not make SDL see it). New in `host.cpp`: GyroLib's **Steam Input runtime reader**
    (`gl_steam_runtime_create`, borrows the game's `steam_api64.dll`, created once it's loaded, polled every frame between `gl_sdl_poll` and
    `gl_update`; log on change). It works (`motion source 2`, ~85 samples/s) but supplies motion only: the gamepad menu shortcut, gyro
    activation and recenter buttons don't work (F10 does); possible fix `gl_submit_controls` from the game's pad state, not done.
  - **Camera write hunt (all dead ends from our frame hook):** `BaseCamParam.RotationDegree` (x pitch, y yaw, yaw 0..360, turn right = yaw down)
    is an output the game recomputes every frame (`kept=4/2529`); an automatic input test of the operator's vec2 fields (`_MouseRotateAmount`,
    `_GyroInputAmount`, `_RotAmount`, `_PadInput`) moved nothing (the operator recomputes the amounts in the same pass that turns the camera);
    the state test of `BaseCamParam.RotationDegree`, `mcCam_LimitRotation._BeforeRotDeg`, `CurrentParam.RotationDegree` all rewritten. The exe's
    code is **encrypted on disk** (anti-tamper), only readable in memory; REFramework's `get_encoded_pointer` pattern isn't in this build. **Found
    the method base another way** (read-only ReadProcessMemory): encoded offsets are filled in memory (144 methods on PlayerCameraController), all
    multiples of 16; scoring each 16-aligned base by how many `base + offset` addresses follow int3/ret padding gave **base = exe+0x4BF2E0**
    (289/291; next best 77). New tool `tools/reEngine/methoddump.py` (read-only, README 2b).
  - **Disassembly** (rcx VM context, rdx this, r8 &arg, r9 flag; offsets include the field base, 0x50 controller, 0x10 params):
    `get_RotationDegree` (exe+0x8B31120) reads `[[[[this+0x60]+0x48]+0x78]+0x20]` = BaseCamParam -> `<EyeAttachSetting>` (+0x38) ->
    `<AttachParam>` (+0x68) -> **`_AttachDeg`** (+0x10, vec2). `addRotationDegree` (exe+0x8B31500) adds the arg to `_AttachDeg` with angle wrap
    (exe+0x9D41880), clamps pitch to `_PitchDegreeRange` unless r9, then the setter exe+0x532F520, which writes the vec2 into both the
    EyeAttachSetting and LookAtAttachSetting attach params. `overwriteRotationDegree` (exe+0x8B31430) is the same without the addition.
    **Checked live (WriteProcessMemory):** `_AttachDeg` y += 30 on both attach settings turned the camera left ~30 degrees and stayed; x += 10
    looked up ~10. Model `mhwilds_camera.cpp` (registered last; no `app.PlayerCamera`/`CameraSystem`/`CameraBlender` in this game) writes both
    `_AttachDeg`s: yaw right = y down (`kYawSign -1`), look up = x up (`kPitchSign +1`), pitch within `_PitchDegreeRange`, yaw wrapped 0..360.
    **User-verified (DualSense, Steam Input on, Steam's gyro off, GyroLib's Steam Input reader): the gyro turns the camera, but is lost at
    moments (not investigated).** **Left here by the user's decision:** MH Wilds depends on Steam Input entirely and Steam Input already has
    full gyro support for it. Open if picked up: the lost moments (Steam reader motion gaps / source switches), the menu shortcut.
  - Cleanup: removed `prepare_steam_environment` (ignore-list removal) and the `xinput: the game creates DirectInput` line. Kept: Steam Input
    reader lines, `the game opens a DirectInput game controller`, HID-import expiry lines.

- 2026-10-17 **Auto XInput: native-support signs from use, not from imports** (core `xinput.cpp`; 378 checks; MH Wilds exposed it). MH Wilds creates
  DirectInput and imports HID.DLL but never opens a HID device nor reads the DualSense; RE7/RE8 had the same false DirectInput sign.
  - **DirectInput:** `DirectInput8Create` alone no longer counts. `IDirectInput8::CreateDevice` (vtable 3) is patched in place; a created device whose
    type is joystick..1st-person is recorded with `DIPROP_VIDPID` and its `GetDeviceState` (9) / `GetDeviceData` (10) patched; the sign (Any) is the
    game reading one (`it reads a DirectInput game controller (VID:PID, GetDeviceState)`).
  - **hid.dll imported alone** (no libScePad): the Sony sign holds 30 s after the pad appears (Hogwarts opened it at 15 s), then only while the game
    has opened a PlayStation HID device (`opened_vendor(054C)`). Logs `the game imports the HID library but hasn't opened a PlayStation controller
    in 30 s: it doesn't read it itself` / `... its own PlayStation support is back`.
  - Risks: a game reading a DualSense through a statically linked Sony library that first opens it after 30 s gets the Xbox controller until then;
    a game reading through DirectInput is translated until its first read. MH Wilds then showed XInput after 30 s but the game never called
    `XInputGetState`: XInput isn't how it reads pads.

- 2026-10-17 **Resident Evil 2 remake (Xbox/Microsoft Store GDK build, `re2.exe`, TDB 70): works; camera model `re2_camera.cpp` (RE2 + RE3).**
  - **Install:** the GDK exe can't be read from disk (WindowsApps ACL), and in memory is "protected" (import table without name tables): no import
    hooks, XInput translation off. **`winmm.dll` never ran** (REFramework logged a late `Loaded: WINMM.dll` from another path); **`version.dll`
    loads at start** (`PROXY=version.dll`). Launch: `shell:AppsFolder\F024294D.GAMEResidentEvil2biohazard2_8fty0by30jkny!re2`. TDB 70 header at
    `re2.exe+0x7578930`, 80296 types, 302 entries (`LateUpdateBehavior`(215)), frame hooked (original `re2.exe+0x58B1CF0`).
  - **`0 singleton slots in the exe`** (the REGlobals code pattern isn't visible in the protected code): new `managed_object_slots` (`re_tdb.cpp`)
    treats every qword in the exe's writable (then writable+executable) sections that holds a managed object (type definition in the TDB's types
    table) as a slot, rescanned every 2 s (RE2: 1402 objects, 250 ms; `app.ropeway.camera.CameraSystem` at `re2.exe+0x93D9290`). New **`TdbDump=`** (RE
    module setting): writes `Gyro4All\Gyro4All-tdb.txt` with every matching type (parent, own fields with offset and type, statics) and each singleton
    slot's type; without a setting it runs only when no camera model fits (filter `camera`). TDB API `own_fields`.
  - **RE2 camera classes:** `CameraSystem`: `<BusyCameraController>` +0x48 (`CameraControllerRoot`, the camera in use), `<CameraControllers>` +0x8,
    `MainCameraController` +0x98. `CameraControllerRoot`: `<CameraRotation>` +0x40 quat. `TwirlerCameraControllerRoot`: `<IsControllable>` +0x90,
    `IsInputPitchLimit` +0xb0, **`<Pitch>` +0xb8, `<Yaw>` +0xbc (floats, radians)**, `<PitchMin/Max>` +0xc0/+0xc8, `<YawMin/Max>` +0xd0/+0xd4; subclasses
    `PlayerCameraController` (free look; `<IsOpenPause>` +0x128, a `ValueTrigger`1` with value at +0x0) and `PlayerSightCameraController` (aiming).
    Free look = busy controller is either, `IsControllable`, not `IsOpenPause`; turn adds to `<Yaw>`/`<Pitch>` with `kYawSign -1`, `kPitchSign +1`.
    **User-verified, without REFramework: works perfectly**; gyro off in Inventory/GimmickFixed/Title cameras and the pause menu. (REFramework v1.5.9
    in this install stayed at "currently initializing"; not needed.) The controller's quaternion looks down -Z so `read_rotation`'s pitch is inverted
    there (only the status line uses it). GAMES.md: RE2 Supported (`version.dll`).
- 2026-10-17 **RE2: GyroLib's panel didn't idle the game's controller; new `dinputpad.cpp`.** In protected exes `xinput`/`nativepad` were off, so nothing
  idled the pad. RE2 reads the DualSense through **DirectInput** (`DINPUT8.dll` opens 054C:0CE6; the exe only identifies it via HID IOCTLs
  0xB01A8/0xB0193/0xB019C; no `ReadFile`, `ReadFileEx`, Raw Input, or `XInputGetState`; no XInput emulator running). Fix: inline hook on
  `dinput8!DirectInput8Create` -> `IDirectInput8::CreateDevice` (vtable 3) -> game-controller devices' `GetDeviceState` (9) / `GetDeviceData` (10)
  (not mouse/keyboard, not created by Gyro4All/gyrolib.dll/SDL3.dll). The host passes panel state and GyroLib's selected pad: the game's own state
  buffer is remembered while SDL sees the pad at rest for 5 frames (no buttons, sticks |v| < 4000, triggers < 2000) so no knowledge of the data
  format is needed; while the panel has the pad `GetDeviceState` returns that buffer (fallback: DIJOYSTATE/2 buttons cleared, hats centred) and
  `GetDeviceData` returns none. **User-verified in RE2** (only the panel reacts, the game reacts again after closing; ~1200 states idled in 7 s).
  Also: `nativepad` works in inline mode (inline hooks on `kernel32!CreateFileW/CloseHandle`, `kernelbase!ReadFile/GetOverlappedResult(Ex)`); opens by
  Gyro4All, `gyrolib.dll` and GyroLib's `SDL3.dll` (unpacked to GyroLib's own folder under local app data) are never followed (following SDL's handle
  had idled the panel itself); no DS4 identity in inline mode; inline-mode diagnostics. Not done: a button held while the panel opens (buffered
  events dropped).
- 2026-10-17 **One DirectInput path for every game: `dinputpad.cpp` in both hook modes.** Before, unprotected games only had `xinput.cpp`'s DirectInput
  hooks (auto-mode sign, only with `XInput=1`). Now `dinputpad::install()` from `host_configure` (after `xinput::install`) in every game and XInput
  mode: `DirectInput8Create` via the game modules' imports or dinput8.dll's export inline; `CreateDevice` and the controllers' `GetDeviceState/
  GetDeviceData` patched in their vtables (up to 4 vtables for A/W; SDL's DirectInput devices skipped). Panel idling as above in normal exes too;
  xinput.cpp keeps only the sign via `dinputpad::on_read`. Log: `dinputpad: the game opens a DirectInput game controller (VID:PID, type, axes,
  buttons)`. **User-verified** that the same build works in other DS4/DualSense and XInput games, RE2's panel still idles the game.
- 2026-10-17 **Resident Evil 3 (remake) works out of the box with RE2's model** (user-verified; Game Pass build, DualSense, `version.dll`). TDB 70 (87733
  types); `CameraSystem` found through the exe's data (1357 objects), `offline.camera.CameraSystem`. Offsets differ from RE2, all resolved by name:
  `<BusyCameraController>` +0x50, `<IsControllable>` +0xa0, `<Pitch>` +0xc8, `<Yaw>` +0xcc; same signs; reads the DualSense through DirectInput; XInput
  translation off (protected).
- 2026-10-17 **Build: Gyro4All 1.2.0** (user's version, new DirectInput handling), `Gyro4All-v1.2.0.zip` (10 profiles). Since 1.1.0: RE2 and RE3 remakes
  supported (Game Pass builds, no REFramework, `version.dll`); `dinputpad.cpp`; nativepad inline mode; RE Engine singletons from the exe's data and
  `TdbDump=`; MH Wilds groundwork.
- 2026-10-17 **RE Engine `CameraWatch=1` diagnostic; RE3 "camera the gyro can't turn" investigated.** `watch_fields` (`re_camera.cpp`) logs every 500 ms the
  member fields of the active camera object and `CameraSystem` (one level into referenced managed objects) that changed; off by default.
  - RE3's `EventCameraController` (cutscene camera, not a twirler) has no look-around input. In a restricted scene the camera is the normal
    `PlayerCameraController` with narrowed limits (`<PitchMax>/<PitchMin>` +0xe0/+0xd8 `offline.DampingFloat`, `<YawMax>/<YawMin>` +0xec/+0xe8, +-0.262 rad
    around the scene's yaw; `IsInputPitchLimit` +0xc0 = 1). The gyro adds to the same floats, so the game clamps it the same way. (In one run GyroLib never
    enabled the gyro: `context.1.gyro.activation=3` hold with trigger.)
  - Correction (user): the gyro was always on and didn't move the camera. A per-frame write check (`writes=N changed by the game before the next frame=M`)
    showed in normal gameplay 0 of 213 writes changed but in the restricted scene **1487 of 1487** were undone. In that scene the camera is in forced-twirl
    mode: `PlayerCameraController.<ForceTwirlerParam>` (+0xf8: Time, Yaw +0x4, Pitch +0x8, BeforeYaw, BeforePitch, Interrupt, PermitOverwrite, SetPitch,
    SetYaw) holds the forced centre; the game rebuilds `<Pitch>/<Yaw>` every frame from that centre plus an input offset in no reflected field.
    Writing the gyro to `ForceTwirlerParam.Pitch/Yaw` lasted only during the ~0.5 s swing to the scene's aim (then the game nulls it): **dead end, removed.**
  - **Stick fallback (experiment; the origin of core `StickCamera`/stick fallback).** `Engine::stick_fallback(x, y)` every frame after `apply`;
    `host.cpp` passes it to `dinputpad::set_stick`, which adds it to the right stick (Z right, Rz down, within the game's `DIPROP_RANGE`; -32767..32767 in
    RE3's 272-byte DIJOYSTATE2) of what `GetDeviceState` returns (not while the panel has the pad). RE2/RE3 model: a direct write counts as undone when
    next frame the angles are back within half the step (steps > 0.11 deg only); 8 undone in a row start the fallback (the gyro's turn in those 8 frames
    is owed from the first, `starting with the (p, y) deg they lost`). Then `turn()` adds the gyro to an owed turn instead of writing; every frame the owed
    turn loses what the camera actually turned (closed loop), fades 3 %/frame, kept within +-8 deg; deflection = 0.12 + 0.88 * owed/5 deg (0 under 0.3 deg).
    A direct probe write every 90 frames ends it when kept. Status `stick fallback: owed (p, y) deg, stick (x, y)`. `StickFallbackTest=<seconds>` (test
    only): every other period of that length the model skips the gyro's writes and reports them undone.
  - Tests: first run (user: "good enough", small hiccup at the switch); fast turns saturated the stick (RE3 turns at most ~81 deg/s at full stick). Speed
    matching: the stick share = gyro speed / the game's speed at full stick (learnt from what the camera turned for last frame's share; shares over 0.25
    in the same direction only, 5 %/frame, bounded 20..720 deg/s, starts at 81) plus owed/5 deg. Second test: speed matching made little difference and
    **the right stick was almost dead while the fallback ran**: the loop took every camera turn (the player's stick included) off the owed turn and
    deflected the stick the other way; the learnt speed wandered 35..157. Fix: `Engine::stick_fallback(player_stick, x, y)`; while the selected SDL pad's
    right stick is past 6000, only the turn our last share should have made (share * learnt speed * frame time) is taken off the owed turn, and nothing is learnt.
- 2026-10-17 **Stick fallback made universal (core `stick_fallback.cpp`)** (400 checks incl. 7 simulated-game tests). User verdict on the RE2/RE3
  prototype with the stick fix: "Perfect". Core `StickFallback` (no game memory access, unit-tested): `frame(known, pitch, yaw, camera, player_stick,
  now)` before the turn, `direct()`, `turn(pitch, yaw, written)`, `stick(x, y)`, `status()`. A write is undone when next frame the angles are within half
  the step of where the camera would be without it, kept when within half the step of where it would be with it, both after the turn our own stick
  should have made (a probe write judged against bare pre-write angles looked kept because our stick had moved the camera); no verdict while the
  player's stick moves; deltas wrapped to +-180 (Unreal keeps pitch 0..360). Give-up: 45 frames of deflection over 0.5 turning the camera less than 1 deg
  end it for that camera ("the stick turns nothing": the game reads the pad through a path Gyro4All can't change). Learnt speed starts at 90 deg/s.
  - `Engine::view_angles(pitch, yaw, camera)` replaces the RE-only `stick_fallback` (RE models via `Camera::angles`, yaw negated for quaternion
    models; Unreal `read_view_rotation` of the active view, not `WorldRotation`/`Match=Camera` views). Injection on every pad path:
    `dinputpad::set_stick` (Z/Rz), `xinput::set_stick` (the shown controller in `our_state`, or a real XInput controller in `real_state` while selected),
    `nativepad::set_stick` (selected controller's right-stick bytes in the game's own reports, CRC recomputed). Host: `turning` gate,
    `g_stick_fallback.frame` before `apply`, apply only when `direct()`, 0 while the panel has the pad, `status: stick fallback:` line. Removed: the
    RE2-only fallback and `StickFallbackTest=`; kept `CameraWatch=`. Untested then: the XInput and nativepad injection and engines other than RE3.
- 2026-10-17 **Settings panel for D3D11 / D3D10 / D3D9 games: `Gyro4All\gyro4all.dll`.** User's choice: GyroLib's static ImGui panel inside the game's own
  swapchain, in a separate DLL loaded only when needed, 1:1 with the DX12 panel. Why a separate DLL: GyroLib 1.3.1's `panel.h` (`gl_panel_create`,
  `gl_panel_function_key`, `gl_panel_draw(w,h,dpi)`) is in `gyrolib_panel.lib` + `gyrolib_imgui.lib` (ImGui **1.92.9b core only, no backends**), both **/MD**
  while winmm.dll is /MT (LNK2038); moving winmm.dll to /MD rejected (Severed Steel's old MSVCP140 would break the proxy before `deps.cpp` can warn).
  API.md: one owner thread per context, so the static panel's ImGui frame is built on the game thread (`overlay::draw(pad)` in `host.cpp`, right after
  `gl_update`) and only its draw data is rendered on the presenting thread; one lock keeps frame, window messages and renderers apart. Closed panel: no
  ImGui frame. OVERLAY.md: the DX12 frontend adds the Steam Input mouse bridge (`gl_overlay_win32_message`), capture mask, gamepad nav, HDR composition
  and the 250 ms heartbeat. Backends: `imgui_impl_dx11/dx10` honour `FramebufferScale`, dx9 doesn't. `third_party/imgui-1.92.9b` (`git clone --depth 1
  --branch v1.92.9b https://github.com/ocornut/imgui third_party/imgui-1.92.9b`; only `backends/` compiled).
  - Built: `src/panel/` (target `gyro4all_panel`, OUTPUT_NAME `gyro4all`, /MD): `gyro4all_panel.h` (C interface, one export `g4a_get_api(G4A_ABI_VERSION)`),
    `panel_main.cpp` (ImGui context: no imgui.ini, `AddFontDefaultVector`, keyboard+gamepad nav, `NoMouseCursorChange`; Win32 backend with
    `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`; gamepad keys from the selected SDL pad; capture while open; Escape closes unless a popup is open; input cleared on each
    opening; 250 ms present heartbeat closes the panel), `panel_dx11.cpp` (own device-context state swap, RTV per frame; HDR: `SetColorSpace1` value, FP16 =
    scRGB, else SDR; panel drawn into an RGBA8 texture and composed at 203 nits, HDR10 against a scene copy with PQ decode/encode), `panel_dx10.cpp`
    (`D3D10CreateStateBlock`), `panel_dx9.cpp` (state block + saved targets, own Begin/EndScene, `InvalidateDeviceObjects` before Reset/ResetEx).
    Core: `com.hpp` (vtable slot patching), `panel_host.cpp/.hpp` (loads the DLL with `LOAD_WITH_ALTERED_SEARCH_PATH` once a target exists; D3D9 hooks:
    `Direct3DCreate9(Ex)` imports, IDirect3D9 slots 16/20, device 16 Reset, 17 Present, 121 PresentEx, 132 ResetEx, swapchain 3 Present, nested present
    guard); `overlay.cpp`: non-DX12 swapchains to `on_other_swapchain` (D3D11/D3D10 by `GetDevice`; slots 8, 13, 22, 38, 39); import hooks
    `D3D11CreateDevice(AndSwapChain)`, `D3D10CreateDevice(AndSwapChain)(1)`; F10 via `gl_panel_function_key`; once gyro4all.dll is attached no DX12 overlay is
    created. Wrappers (ReShade): on the first D3D10/D3D11 device (import mode, not `OverlayFactory=game`) the System32 DXGI factory vtable is hooked too
    (`hook_system_factory(lazy)`), same for System32 d3d9.dll; a swapchain/device created inside another hooked creation wins over the proxy around it
    (thread-local creation depth) so the panel draws after the wrapper's effects. DX12 unchanged (lazily hooked factories ignored for D3D12). Known gap vs
    DX12: the Steam Input mouse row/bridge.
  - **First tests (user):** Mortal Shell (DX11) works: Present hook 1.2-1.4 us closed, 15-21 us open; gyro frame ~190 us open vs ~50 us closed. **Finding:
    UE 4.25 DX11 presents RGB10A2 and never calls `SetColorSpace1`**; GyroLib's AUTO rule made the panel HDR10 on an HDR desktop, wrong (DXGI shows a
    swapchain without a colour space as SDR whatever its format, FP16 aside). Fixed: no colour space = SDR, FP16 = scRGB; `output_is_hdr` removed.
    **Freeze on clicking the close button:** ImGui's Win32 backend `ReleaseCapture()`/`SetCapture` sends `WM_CAPTURECHANGED` synchronously, re-entering
    `window_message` on the same thread while it held the non-recursive SRWLOCK: deadlock. Fix: `CRITICAL_SECTION` (re-entrant) and mouse buttons go to ImGui
    directly (`AddMouseButtonEvent`), so the panel never changes the game's capture. **No cursor** (game hides the OS cursor): each frame reads `GetCursorInfo`;
    when hidden ImGui draws its own (`io.MouseDrawCursor`; watch for two cursors in game menus). **Verified in Mortal Shell and Jedi: Fallen Order** (ReShade
    as `dxgi.dll` + RenoDX FP16, Streamline, UE4SS): System32 factory hooked lazily from the device-creation hook, the real swapchain captured inside ReShade's
    creation and ReShade's proxy skipped (`not used: the panel draws on the real swapchain inside it`), `panel drawn as scRGB`. Untested then: DX10, DX9, HDR10,
    exclusive fullscreen, Steam Input mouse gap. The old `GyroUE\` folder was gone before the first start (no migration).
- 2026-10-17 **Build: Gyro4All 1.3.0** (user's version, DX11/DX10/DX9 settings panel), `Gyro4All-v1.3.0.zip`: new `Gyro4All/gyro4all.dll` (loaded only in non-DX12
  games), the panel verified in DX11 (Mortal Shell; Jedi with ReShade/RenoDX scRGB), plus the core stick fallback.
- 2026-10-17 **RE4 remake (`re4.exe`, Steam): TDB 71 and camera model `re4_camera.cpp`** (400 checks; user-verified: "correct functionality"). 1.3.0 stopped at
  `TDB 71 is not supported yet` (`re4.exe+0x97F1DD0`; folder also had REFramework, ReShade, OptiScaler as `dxgi.dll`). **TDB 71:** header is TDB 70's
  (numTypes 0x0C, numMethods 0x10, numFields 0x14, numStringPool 0x50; types 0x60, typesImpl 0x68, methods 0x70, fields 0x80, fieldsImpl 0x88, stringPool 0xD0);
  table sizes give **RETypeDefinition = 0x48 bytes** (69/70/74+ are 0x50), REMethodDefinition 0xC, REField 8, RETypeImpl 0x30; otherwise TDB 74 shapes (19-bit
  ids, impl index bit 38 of the second qword, member_field bits 44..63 of the qword at 0x20 (3000/3000 agree), names two int32). `Tdb::type_size` (0x48 for 71),
  `tdbdump.py` reads 71. 162644 types. **Camera:** `chainsaw.CameraSystem` -> `_CameraControllerList` (`MainCameraController[]`, `_Role` +0x10 Main = 0,
  `_BusyCameraController` +0x28); the player's `chainsaw.PlayerCameraController` and `VehicleCameraController` are `TwirlerCameraControllerRoot` (`_Yaw` +0x98,
  `_Pitch` +0x9C radians, `_YawMin/_YawMax`, `_PitchMin/_PitchMax` DampingFloat; the game's own gyro plumbing `<GyroStartPitch/Yaw>`, `_OldEnableGyro`);
  `CameraControllerRoot._CameraRotation` +0x50, `_IsInputControl` +0x82. Pause `share.PauseManager._CurrentTargetPauseUpdate` +0x30 (u32). Model = RE2's
  twirler on those names (gyro adds to `_Yaw`/`_Pitch` while the pause value is 0; yaw -1, pitch +1). Docs: GAMES row.

- 2026-10-17 **Frostbite module base: Dead Space (2023)** (`src/engines/frostbite/`, `Engine=Frostbite`; verified in game, EA app build `v-1.1.14.17871`,
  DualSense, RivaTuner closed). Frostbite (`Data\layout.toc`, `Data\initfs_Win32`), no anti-cheat, DX12 only, ~400 MB exe with Denuvo + EA OOA, base
  0x140000000, no ASLR; EA app exe code encrypted on disk and imports resolved by the protector, so inline hooks (`Engine::inline_hooks()`).
  `winmm.dll` loads only with RivaTuner closed (`RTSSHooks64.dll` pulls System32's WINMM first); `version.dll` never works (Defender's `MpOav.dll` loads the
  system one first). The EA app makes the install folder read-only on every launch, so files go to `%LOCALAPPDATA%\Gyro4All\<exe stem>\` when
  `<game>\Gyro4All` isn't writable. Reflection (`fb_types.cpp`; layout from Frosty Tool Suite's Bf2042 type classes, read only): TypeInfo (static) +0 info data,
  +8/+0x10 prev/next; info data +0 name, +0x0C flags (kind = (flags>>4)&0x1F), +0x0E size, +0x20 namespace, +0x32 field count, +0x38 parent/fields; found by shape
  (string "DataContainer" -> info data -> the TypeInfo whose links lead to other TypeInfos): ~23,885 types ~5 s after start; `TypeDump=*` writes
  `Gyro4All-types.txt`. The game reads the DualSense through DirectInput8 (`GetDeviceState`, DIJOYSTATE2).
- 2026-10-18 **Tooling used on Dead Space:** core `WriteWatch` (`src/core/writewatch.cpp`; `WriteWatch=on` then an address in the ini while running): hardware
  write breakpoint, `r:` read/write, `x:` execute (with `WriteWatchFields` sampled from rcx), real stack unwind, `di+<off>` = the buffer of the game's last
  `GetDeviceState`, engine-named addresses (Frostbite `cam`), self-disarm after 100,000 hits; works on the protected exe with no Denuvo reaction.
  `dinputpad` logs each place the game reads a controller from. External read-only scripts in `tools/frostbite/` (README): `findimm.py`, `riprefs.py`,
  `funcof.py`, `statefollow.py` + `stateanalyse.py`, `forcefloat.py` (write test), `levelrate.py`, `hop.sh`, `disasm.py`; offline `exefile.py`.
- 2026-10-18 **Frostbite camera model and frame source** (`fb_camera.hpp`, `deadspace_camera.cpp`, `Engine::frame_source`). One model per game in code (no
  profile file). Finds `HybridRotationInputChimeraBehavior` without exe addresses: TypeInfo by name -> its getType (`lea rax,[rip+TypeInfo]; ret`) -> the vtable
  slot holding it (slot 0) -> the update at slot 16 (EA exe+0xFF0040, Steam exe+0xFF00E0); the slot is swapped for a hook (data in the exe). Every camera mode
  change (aim on/off) makes a new behaviour object (the old updates ~0.4 s more); all stop in the pause menu, menus and cutscenes, so "a behaviour updated in
  the last 100 ms" is the gameplay gate. **The game thread stops calling PeekMessageW ~10 s after start**, so Frostbite supplies the frame from its own thread
  woken by each Present (`overlay::on_present`, 50 ms wait, 8 ms timer without Present; ~150 frames/s; the panel opens with F10). Turning the behaviour's rotation
  interface (`[this+0xF8]`, vf48 get / vf70 set, degrees) moved only the camera (Isaac followed in jerks; aiming only the weapon camera moved in a cone): Isaac turns
  from his own input.
- 2026-10-18 **What turns Isaac: the character's action levels.** Global action map (buckets `[exe+0x5605FE8]`, 67 buckets, nodes {u32 hash, u32 index, next}):
  **EIAYaw -> 11, EIAPitch -> 8**; the input state holds floats at **state+0x500 + index*4** (EIAYaw +0x52C, EIAPitch +0x520), hold times at +0x780 + index*4, action bits
  +0xA18/+0xA2C/+0xA40; its aim angles at +0x60/+0x64 are written from the final camera (outputs). **Forcing state+0x52C to 0.5 from outside (`forcefloat.py`) turned
  Isaac and camera together like the stick, on foot and aiming.** The player's input update (EA exe+0x1A55550, Steam exe+0x1A571C0) runs through a 16-byte delegate
  invoker (`mov rdx,[rcx+38h]; mov rcx,[rcx+30h]; jmp <update>`, one of 41) whose address is held in writable data (EA exe+0x5F68B08, Steam exe+0x5EF7B08); this =
  [context+0x30]; P = [[this+0x90]+0x118]; state = [P+0x148]. Turn speed per level (`levelrate.py`): yaw 0.25 -> 11, 0.5 -> 24, 0.6 -> 31, 0.7 -> 43, 0.8 -> 63, 0.9 ->
  105, 1.0 -> ~180 deg/s; pitch ~0.7x, positive = up; positive yaw = right. Writes from outside the update never count (the mapper rebuilds each level every tick,
  `movss [r13+rax*4+0x500],xmm1`, EA exe+0x1CDC0D0); outside forcing worked only because it wrote every ~7 us.
- 2026-10-18 **Dead Space Steam build: code plain on disk** (link time 2023-02-15 02:46 UTC; EA build 02:36). Still the Denuvo layout (random section names, a
  0x13B967FD-byte RWX section), but code entropy 6.55 (EA's 8.00) and a full `.pdata`: disassemblable offline. Layout differs from EA's by a few KB (level store exe+0x1CDDE20,
  input output exe+0x1A56C00); Gyro4All finds everything at run time. New `tools/frostbite/exefile.py`. **Traced offline (Steam):** input update exe+0x1A571C0 ->
  exe+0x1A73A60 (input manager at [this+0x90]) -> exe+0x1A74150 (loops over player-input objects at manager+0x540) -> **exe+0x1A73BE0(P, frame)** -> the mapper exe+0x1CDD2A0:
  first loop each mapping entry's input value from exe+0x1CB46D0, second loop over the action list, **level = clamp(base + value)** stored at exe+0x1CDDE20 (then
  exe+0x1CDC9D0 only sets/clears action bits). In 0x1A73BE0, for the controlled entity: mapper, then **P's vtable slot 16 with (P, dt in xmm1, state in r8)**, then the
  post of the state (exe+0x119ACE0), exe+0x1AEDCA0, slots 12 and 14, per-player snapshots: every consumer comes after slot 16 (exe+0x1A6F150, walks the input components'
  vf48).
- 2026-10-18 **Solution: the gyro as action levels from P's slot 16** (`deadspace_camera.cpp`; **verified in game**, Steam build, DualSense). The input update's invoker is
  swapped for one that, before the game's update, reads P and its state and on the first call checks P's vtable slot 16 (prologue up to `mov eax,[rip+..]`, `mov rsi,rcx;
  mov rdi,[rip+..]` at +0x1A, `sub rsi,rax; xor ebx,ebx` at +0x24) and swaps it for `hk_player`, which adds GyroLib's queued turn to EIAYaw/EIAPitch (only for the
  player's state) through the curves (stick level -> speed, + gyro deg/dt, -> level, capped at 1), then calls the game's. dt = the game's argument (33.3 ms, a 30 Hz
  input tick). The behaviour hook stays only as the gameplay gate; GyroLib runs on the frame thread and only queues the turn. **User: works as expected, smoother and
  correct** (Isaac, arm and camera turn together, on foot and aiming; P's vtable exe+0x3F691E0, 1954 level writes, 209 capped). Open: fast turns beyond full stick
  are capped (180 deg/s yaw, ~110 pitch). **Crash after ~6 min** (the game's minidump): AV in game code exe+0x2480C43 (garbage vtable) on a job thread in the
  character's update during a camera change, no Gyro4All address on the stack, gyro off for 2+ minutes; not ruled out entirely.

- 2026-10-18 **DOOM (2016), id Tech 6: first engine module `src/engines/idTech/`** (Steam build 13954591; no anti-cheat, no packer). Imports `WINMM.dll`, `XINPUT1_4.dll`
  (ordinal), `DINPUT8.dll`, pumps with `PeekMessageA` (core now hooks both). `DOOMx64.exe` is a launcher: with `r_renderAPI "1"` it relaunches `DOOMx64vk.exe`, and as
  first process it kept the session mutex (and the legacy GyroUE mutex and locked log), so the real game stayed a plain proxy; the core now hands the session to a child
  exe of the same folder (`CreateProcessA/W` hooks). In `DOOMx64vk.exe` the DllMain thread never pumps: two other threads do; before the first frame the core adopts the
  thread that gets messages for the visible game window (opt-in `Engine::adopt_pump_thread`). The Unreal fallback found no GUObjectArray and its heap scans stalled frames
  ~400 ms each. The module detects `base\gameresources.resources`.
- 2026-10-19 **DOOM (2016): id's type info in the exe; view angle fields** (`DOOMx64vk.exe`). Static tables `classTypeInfo_t {name, superName, size, 0, variables}` (5653
  classes) and `classVariableInfo_t {type, ops, name, u32 offset, u32 size, ..., comment}` stride 0x48; MSVC RTTI too. Offline reader `tools/idTech/typeinfo.py`. Fields:
  `idGameLocal.playerList` 0xa5d48, `.playerControllers` 0x4a0000, `.gamestate` 0x29a1cc, `.gamePausedTime` 0x49c4c0; `idPlayer.physicsObj` 0x14e58 (`idPhysics_Player`,
  embedded); `idPhysics_Player.current` 0x1aac (`playerPState_t`), `.viewAngles` 0x2040 (3 floats); `playerPState_t.deltaViewAngles` 0x70; `idPlayerController.ucmdTracker1`
  0x32c8 (`usercmd` 0x3c, `viewAngles` 0xe8c); `usercmd_t.angles` 0x10 (`short[3]`). `idUsercmdGenLocal` isn't in the type info. Debug cvars: `g_drawPlayerViewAngles`,
  `pm_minviewpitch/maxviewpitch`; stick cvars `joy_*`. `gameLocal` is a **heap** object (RTTI vtable `DOOMx64vk.exe+0x201E3D8`), the local `idPlayer` (vtable `+0x22A1298`,
  embedded `idPhysics_Player` vtable `+0x28589B0`) is found in the heap by vtable (~110 ms per scan once write-combined and uncached pages are skipped; Vulkan maps ~7 GB
  of GPU memory, scanning it took ~35 s). `viewAngles` follows the camera; `deltaViewAngles` stays 0 (mouse). Senses: yaw positive left, pitch negative up (id convention).
- 2026-10-19 **DOOM (2016): native gyro works in game** (user-confirmed; DualSense). Direct write as in Unreal: `apply()` adds GyroLib's turn to
  `idPlayer.physicsObj.current.deltaViewAngles` (`-pitch`, `-yaw`), pitch kept within +-89 from `viewAngles`, yaw delta wrapped +-180; `view_angles()` reads `viewAngles`.
  The game keeps the delta; gyro frame ~60 us. Right after spawning the game undid 8 writes (intro animation), which started the core stick fallback; it gave up 3 s
  later. The open-loop stick mode (`Engine::stick_speed`) was removed.
- 2026-10-19 **DOOM (2016): the gyro's gate (pause, map, glory kills).** `gameLocal` is recreated with each level: re-confirmed (its `playerList` holds the player)
  whenever the player changes (the first heap candidate at start is another object whose fields stay 0). On the confirmed one: `frameParms` (+0x2bb398) `.isMenuActive`
  (+0x14) = 1 in the pause menu, `.isDialogActive` (+0x12) flickers on its quit dialog, `.advanceTime` (+0x18) = 1 in play; `gamePausedTime` (+0x49c4c0) = -1 while running;
  `gamestate` = 3 in play. On the player: `inhibitFlags` (+0x14c4c) = 0x1f for ~1 s per glory kill and while the map is open, 0x18 for 3-7 s (upgrade screen); the Windows
  cursor shows only in the front end and loading. Gate: paused = isMenuActive or gamePausedTime != -1; no gameplay view = inhibitFlags != 0, deathCamera, or `advanceTime` 0
  (loading, where the game undid writes); flags only from the gameLocal whose playerList holds the player, checked every frame (after a checkpoint load the old one read
  `gamePausedTime` 0, ~4 s without gyro); cursor = shouldConsumeNonGuiInput; `sees_menus()` true again.
- 2026-10-19 **Settings panel (F10) in Vulkan games: works in DOOM (2016)** (3840x2160, 2-image swapchain, format 44; also across swapchain recreation). `src/core/panel_vk.cpp`
  import-hooks `vulkan-1.dll` (`vkCreateInstance`, `vkCreateDevice`, `vkGetDeviceQueue`, `vkCreateWin32SurfaceKHR`, `vkCreate/DestroySwapchainKHR`, `vkQueuePresentKHR`,
  `vkDestroyDevice`, `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`); gyro4all.dll (`src/panel/panel_vk.cpp`, panel ABI 2) draws with ImGui's Vulkan backend into the presented
  image (render pass load/store, PRESENT_SRC both ways), submits on the game's present queue waiting on its semaphores, and the present waits on the panel's. Headers:
  Khronos Vulkan-Headers v1.4.309 in `third_party/` (build only). **Dead ends inside the game's `vkQueuePresentKHR`:** (1) ImGui's texture upload calls `vkQueueWaitIdle` on
  the queue it was given and froze DOOM: the panel records ImGui's texture requests into its own command buffer (`ImDrawData::Textures = nullptr`); (2) loading ImGui's
  functions through `vkGetDeviceProcAddr` first gave it a broken `vkGetPhysicalDeviceMemoryProperties` (vertex buffers got an unmappable memory type, null mapping write
  -> AV): everything now comes from `vkGetInstanceProcAddr`, and a start-up map test disables the panel instead of crashing.
- 2026-10-19 **Build: Gyro4All 1.4.0** (user's version), `Gyro4All-v1.4.0.zip` (same file list as 1.3.0). New: the id Tech module with native gyro in DOOM (2016); the Vulkan
  panel (panel ABI 2); core: `PeekMessageA` frame hook, launcher hands the session to the exe it starts, `Engine::adopt_pump_thread`, `Engine::sees_menus`. Tested: DOOM
  (2016) Vulkan with a DualSense (gyro, gate, F10 panel, checkpoint loads); Cronos (no regression).
- 2026-10-19 **Settings panel (F10) in OpenGL games: works in DOOM (2016)** (`DOOMx64.exe`, `r_renderAPI "0"`; user-confirmed, also Vulkan re-tested). winmm.dll
  (`src/core/panel_gl.cpp`) import-hooks `gdi32.dll!SwapBuffers` (DOOM imports no `wglSwapBuffers`) and `opengl32.dll!wglSwapBuffers`; `WindowFromDC` gives the window.
  gyro4all.dll (`src/panel/panel_gl.cpp`, panel ABI 3: `g4a_api::gl_present(hdc)`) draws with ImGui's OpenGL 3 backend (own imgl3w loader) in the game's current context
  before the swap, setting/restoring draw framebuffer 0, `GL_BACK`, unpack buffer/skips, colour mask, `GL_FRAMEBUFFER_SRGB`, rasterizer discard, logic op, alpha-to-coverage,
  clip distances; every GL call under SEH (a fault turns the panel off); a context change shuts the backend down in the old context (or a throwaway one). Two input bugs (also
  in Vulkan): (1) clicks did nothing (DOOM reads the mouse through DirectInput; its window gets no `WM_*BUTTON*`): the panel polls the buttons (`GetAsyncKeyState`) every
  frame while open and the game window is foreground; (2) the panel's clicks/motion reached the game behind it: `dinputpad.cpp` now tracks the game's DirectInput mouse and
  keyboard, and while the panel captures them `GetDeviceState` returns zeros and `GetDeviceData` keeps only releases. No sRGB handling (as Vulkan).
- 2026-10-19 **DOOM Eternal (id Tech 7): native gyro through the idTech module** (Steam build 25216728, `DOOMEternalx64vk.exe`; no packer/Denuvo strings). Type info parses
  the same layout (6927 classes); `idPlayer.physicsObjHavok` 0x8a50 (`idHavokPhysics_Player`: `viewAngles` 0x3f10, `current` 0x3f28, `currentGameTime` 0x4298 int64),
  `playerPState_t.deltaViewAngles` 0x80, `idPlayer.inhibitFlags` 0x87fc. No `idGameLocal` class (`idGameSystemLocal` `state` 0x44, `mapInstance` 0x50); the DllMain thread pumps,
  no launcher. The gyro added to `current.deltaViewAngles` turns the camera and holds (user-confirmed). Gate: paused while the player's `currentGameTime` hasn't changed for 60 ms
  (a 60 Hz tick changes it every 16.7 ms; `view_angles` reports nothing after 20 ms without a tick; both on the performance counter since GetTickCount64 moves in ~15.6 ms
  steps); no gameplay view while `inhibitFlags` != 0 (0x1f87f at spawn) and in a new player's first 2 s of advancing game time (the game undid writes 0.05-1.2 s after its
  time started). The stick fallback had started as the pause/map opened with the earlier 150 ms threshold. Tested 5 min: 9 pauses/maps and 3 spawns, gyro on 99% of frames, ~84 us
  per frame, one 0.3 s fallback after leaving a pause. The Vulkan F10 panel works (format 64).
- 2026-10-19 **DOOM: The Dark Ages (id Tech 8): native gyro works** (Steam 3017860, build 25755602, `DOOMTheDarkAges.exe` ~130 MB, `.bind` = SteamStub; ReShade as `dxgi.dll`;
  user-confirmed: DualSense, Steam Input off, frame generation on and off). Type info: `classVariableInfo_t` stride 0x58, `classTypeInfo_t {name, superName, hash, size, 0,
  variables, ...}` 0x58 apart: 9969 classes (`typeinfo.py` and the module try both layouts and keep the one finding more). `idPlayer : idActorPlayer` (0x3a1a0); physics behind a
  pointer `idPlayer.idPlayerPhysicsInfo` 0x2df8 -> (0xa68): `viewAngles` 0x1f0, own `deltaViewAngles` 0x1fc, `current` 0x208 (`playerPState_t` 0xf8, `deltaViewAngles` 0xa4),
  `currentGameTime` 0x5f8, `command` 0x160 (`idUserCmd.angles` 0x1c `short[3]`); no `inhibitFlags` on the player. Detection marker `base\common.resources`. View model:
  `viewAngles = command.angles + current.deltaViewAngles` (mod 360) as in 6/7. `idTechLauncher.exe` (id's Electron mod launcher in the folder) loaded the proxy first and
  kept the session from the game: new `Engine::is_launcher(exe)`. The game reads the DualSense itself (HID.DLL): no XInput. **Limitation:** during some animations (some
  weapons' reloads, spawning) the game recomputes `current.deltaViewAngles` every tick and the gyro's writes are lost there (the mouse still works via the command). **Dead
  end:** holding the gyro 0.5 s whenever the delta changed (98 holds in a row), then whenever a write was undone; kept: no hold (gyro simply has no effect during such an
  animation). Not tried: the gyro as mouse input, or `idUsercmdGenLocal`. Gate: `currentGameTime` frozen = paused, first 2 s of a new player. **F10 panel not working yet:** the
  game presents through Streamline's `streamline\production\sl.interposer.dll` (229 Vulkan exports): no present reaches the import hooks (`renderer not ready`); next: hook the
  interposer's Vulkan exports when it loads (as RE Requiem's DXGI factory).
- 2026-10-19 **DOOM: The Dark Ages: F10 panel works through Streamline's Vulkan interposer** (user-confirmed with frame generation on and off, toggled in game
  without freezes). `src/core/panel_vk.cpp` hooks two paths with the same code (templated on the path): the exe's `vulkan-1.dll` imports and `sl.interposer.dll`'s
  exports inline (MinHook) as soon as it loads (`hooks::watch_dll`, the loader notification `overlay.cpp` also uses for Streamline's DXGI factory): `vkCreateInstance`,
  `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`/`2`, `vkCreateWin32SurfaceKHR`, `vkCreate`/`DestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkQueuePresentKHR`,
  `vkGetInstance`/`DeviceProcAddr`. Each hook calls its own path's original; a hooked call made inside another on the same thread is passed through (`thread_local` depth),
  so the panel draws once per present. Log findings: instance, device, first swapchain and its first presents go through `vulkan-1.dll`; ~1 s later the game makes a new
  swapchain through the interposer and presents only there (why only the first presents reached the import hooks before); the interposer's `vkGetInstanceProcAddr(
  "vkCreateWin32SurfaceKHR")` returns its own function so the surface isn't seen (window from the main-window fallback, right). gyro4all.dll: no device-wide wait inside
  a present any more (own fences only; frames rebuilt when the swapchain's images change under the same handle).
  - **Open bug (input), not reproduced:** with the cursor hidden (gameplay, game raw-input mouse flags 0x230 `RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE`) neither mouse nor d-pad
    navigated the panel in one session; a diagnostic build in a second session worked everywhere (14 openings). gyro4all.dll now logs the panel's input state 1 s and 3 s into
    each opening (`input +1000ms: ...`). Not retested: DOOM 2016/Eternal (Vulkan without Streamline), RE Requiem.
- 2026-10-19 **Wolfenstein: The New Order (id Tech 5, PC Game Pass 1.14.1.0 and GOG): works.** `GameLaunchHelper.exe` (the Xbox app's generic launcher in every PC Game Pass
  folder) started `WolfNewOrder_x64.exe` and kept the session (no `CreateProcess` hook saw it): it never takes the session now (`dllmain.cpp attach`, before the mutex).
  `WolfNewOrder_x64.exe` can't be read from disk (Game Pass encryption). The game pumps `PeekMessageA` on two non-DllMain threads (`+0x33117B`, `+0x3310C7`); the idTech
  module detects id Tech 5 (`base\master.index` + `base\chunk0.resources`) for the pump-thread adoption, and its scan thread stops after the type info/RTTI log when the
  player's view fields aren't found. id Tech 5 has id's type info (3328 classes, id Tech 6/7 table layout) but none of DOOM's field names; RTTI vtables `idGameLocal` +0xF2FEB0,
  `idPlayer` +0xF512B0, `idPhysics_Player` +0xCEA108. OpenGL panel ready, DualSense shown as XInput (not a Steam game). F10 is only read while the game's process owns the
  foreground window (explorer's XAML window was in front for a while).
  - Live memory: physics via `idPlayer+0x310` (separate allocation); view angles physics+0x488 (copies +0x3584, +0x3590); writing them is undone in 50-100 ms. **WriteWatch**
    showed physics+0x48c written ~62/s by `exe+0x4119CB` in `exe+0x411930` = `idPhysics_Player::SetPlayerInput(cmd, oldCmd, viewAngles)`, and physics+0x3588 by
    `exe+0x60D1A8` in `exe+0x60D130` = `SetViewAngles(angles)` (`delta[i] = angles[i] - SHORT2ANGLE(cmd.angles[i])` at +0x13c, `viewAngles` +0x148, command angles as shorts at +0x60,
    scale 360/65536). **View = SHORT2ANGLE(cmd.angles) + deltaViewAngles**, delta at **physics+0x3578**. A +20 write held and turned the view on foot (~20 degrees, screenshots)
    but not in seated/scripted scenes (the plane cockpit); the delta is set per scene.
  - **Plane turret: the gun follows the input angles, not viewAngles.** Writes to deltaViewAngles held in memory but the screen didn't turn; no game code reads the player's view
    there. The input generator's accumulated floats at **exe+0x1c1cb40** (pitch, yaw, roll; id senses) turn into the command's shorts every tick; the gun turns ~0.4x the command's
    change and stops at its limits, while viewAngles goes 1:1. Writes to the input angles at the turret turned the scene (-60 -> ~24 degrees right). On foot the view follows the
    input angles 1:1. **Change (idTech, id Tech 5): the gyro is added to the input angles**, `view_angles` reports them, the build check compares them with the command (log `id Tech 5
    check, view yaw -102.507 = command 47.763 + delta -150.270: ok`). The stick-only mode, its INI settings, a Turret class check and a "hold the view near the camera" attempt were
    tried and reverted. **User-confirmed on foot and at the plane turret.** Automated tests that focus the game while nobody plays got the player killed at the turret (user-driven
    runs only).
  - **GOG build** (unencrypted, Galaxy64.dll; idPlayer vtable exe+0x10297D8, idPhysics_Player exe+0xE87B58): same layout, but not the input angles at exe+0x1c1cb40 (a value
    search found copies only). In the Game Pass dump the input angles are **idUsercmdGenLocal+0x10** (the one static object with that RTTI vtable, exe+0x1c1cb30, vtable
    exe+0xc90538), same as Wolfenstein II: the id Tech 5 path finds the generator that way (any build or store), falling back to the Game Pass address, then the value search.
    **Works in the GOG build** (user-confirmed; `idUsercmdGenLocal WolfNewOrder_x64.exe+0x1CD5110: input angles at +0x1CD5120`).

- 2026-10-19 **Stick camera (`StickCamera=1`, core): the right stick turns the camera in degrees, past the game's capped stick speed, without mouse emulation.** Some games cap
  the right stick's turn rate; Steam Input's "joystick as mouse" gets past it but the game then switches prompts/UI. Options weighed: mouse emulation (rejected: UI switch),
  rescaling the stick (can't pass 100%), raising the game's own limit (per game), additive boost (idea); **full replacement chosen**: the game reads a centred right stick and Gyro4All
  writes the stick's turn with the gyro's (`Engine::apply`).
  - `src/core/stick_camera.hpp/.cpp` (`StickCamera`, no game memory, unit-tested): radial deadzone, the rest rescaled to 0..1 and raised to `StickCurve`, direction kept; edge
    acceleration from `StickSpeed` to `StickEdgeSpeed` over `StickEdgeRamp` ms while past `StickEdge`; frame time from `SDL_GetTicksNS` capped at 0.1 s. Settings: `StickCamera=0`,
    `StickSpeed=360`, `StickSpeedPitch=200`, `StickDeadzone=10`, `StickCurve=2.0`, `StickEdge=90`, `StickEdgeSpeed=720`, `StickEdgeRamp=400`, `StickInvertY=0` (the `Stick*` values reload
    on save; `StickCamera` at start). Hiding the stick: `xinput::frame(..., suppress_right)` zeroes `sThumbRX/RY` of the shown controller; `nativepad::frame(pad, suppress)` centres the
    stick bytes in the game's own PlayStation reports once their layout was checked against SDL (armed by `NativeFlickStick || StickCamera`). R3 still reaches the game. Steam Input and
    real Xbox controllers: not hidden, so off there (`stick camera: unavailable: ...`). Gate: gameplay view, no cursor, focus, no pause, no panel, no `hold_gyro`; off while GyroLib's
    flick stick uses the right stick (flick keeps priority) and while the stick fallback runs. Lost by design: the game's aim assist, slowdown and invert-Y (`StickInvertY`); a right-stick
    weapon wheel needs `StickCamera=0` or `StickCameraPassButtons` (comma-separated buttons held over 200 ms give the stick back to the game; user request for TNO's R1 weapon wheel).
    Note: `applied pitch` reached ~-395 deg in TNO (its input angles have no pitch limit, so pushing past the view's limit stores turn to undo; affects the gyro too).
  - **First test, TNO:** worked (no UI change), but after ~2 min the stick fallback started because it judged the stick camera's writes (~40 deg/s pitch against the pitch limit) and
    gave that turn back through the game's stick: the camera ran away ~10 s. Fixed: the fallback gets only the gyro's turn; the stick camera's turn counts as the player's stick.
    A `StickFallback=0` switch was added then removed (user: the fallback has to follow the gyro in every game).
- 2026-10-19 **Stick fallback: five closed-loop TNO tries (all dead ends), then an open-loop learner.**
  - Closed loop (`stick_fallback.cpp` rewritten to learn the game's response per axis at 9 deflection points, kept monotonic, steady deflections only, with a sense check). Failures in order:
    (1) learnt the minimum speed (id Tech's stick accelerates while held) and overshot; (2) `sense flipped` spurious on both axes because TNO's stick reaches the camera late and smoothed;
    (3) a pitch limit taught a slow curve and the owed turn pushed back; an inverted vertical axis can't be caught by "held deflection"; (4) the "turns nothing" give-up fired at the pitch
    limit (fix: skip blocked/limit pitch frames); (5) lagged-correlation sense check read lags longer than its window, or the loop's own oscillation, as an inverted axis. Replaced by a
    runaway check (>4 deg owed, deflection >0.3 toward it, camera moving away for 30 frames >8 deg flips the sense). **Dead end: any sense check that correlates deflection and turn inside
    the closed loop.** Also dead: subtracting the turn still "in flight" (Smith-predictor style) made every case worse. Final closed-loop parameters (catch-up 0.35 s, at most +90 deg/s, owed
    leak 0.99) were never confirmed in game.
  - **Open loop (current, `stick_fallback.cpp` + new `stick_response.cpp`):** the deflection comes from the gyro's speed alone (a still gyro is a centred stick). The game's stick model is
    identified online from what the camera does with the deflection given: speed = top speed per axis (signed: inverted look) x shape, `lag` frames late; shapes on a grid (axial/radial x
    dead zone 0..0.35 step 0.025 x curve 1..3 step 0.25), lags 0..10 frames; per candidate a least-squares fit of the camera's turn over 8-frame windows (cumulative sums), weighted by relative
    error and a steady deflection; sums fade with a 20 s half-life; smallest residual wins (3% margin to switch). Used once there are 90 windows of clear deflection and the model explains
    >= 50% of the camera's motion; an axis with too little data keeps the prior's speed scaled like the other. Left out: the player's stick (and 12 frames after), frames with a direct write,
    jumps over 30 degrees, an axis stopped at a limit (found by position). Before learning: the model saved for the game (`StickFallbackLearned=` in the profile, written when it changes, at most
    every 10 s and when the fallback stops), else radial dead zone 0.2, curve 1.5, 160/110 deg/s. Turns under the smallest deflection past the dead zone go out as pulses; at most 2 degrees of
    the gyro's turn wait. Probe verdict: the stick's own turn in that frame is taken as last frame's. Removed: the `StickFallbackYawSpeed/...` settings of a fixed open loop (tested in TNO: its
    defaults, linear 180 deg/s, were far from TNO's curved ~85 deg/s stick). Simulations (`tests/profile_tests.cpp`): camera/gyro speed ratio 0.96-1.11 across linear, UE-like, radial with lag and
    smoothing, noise and recoil kicks, id Tech-like acceleration, uneven 144 FPS; inverted pitch learnt as such; a still gyro centres the stick. 503 checks; ~9 us per frame. `MotionTrace=1` with
    `StickFallbackTest=1` adds `K,x,y,yaw,pitch,used` lines.
  - **TNO test of the learner: the game didn't take the fallback's stick changes.** Camera ran away at TNO's full-stick speed (~120 deg/s pitch) with our stick at 0, and stood still with our
    stick at +-0.7/+-1. Cause: our XInput slot's `dwPacketNumber` only changed with the physical pad's state and id Tech 5 skips a state whose packet number didn't change (it kept the last
    deflection it had taken); the earlier closed-loop tests had the same hidden problem. Fix (`xinput.cpp`): a changed fallback deflection bumps the packet number (like the long-press masks,
    `g_mask_changes`, now also added to our slot's packets). The learner had learnt from that bad data and saved `radial,0.350,1.00,7.0,-5.0,0`, which drove the next session's runaway (a wrong
    sense makes the player tilt against the camera). Guards: a top speed under 20 deg/s is not a stick (model rejected); a model is saved only when it explains >= 70% of the camera's motion
    from 300 windows of clear deflection. 505 checks.
- 2026-10-19 **Wolfenstein II: The New Colossus (id Tech 6, MachineGames; Game Pass 1.5.0.0): native gyro works.** `NewColossus_x64vk.exe` (Vulkan only) can't be read from disk; no anti-cheat;
  the Switch port has native gyro, PC has none. Saves backed up. `engine: idTech (detected)` (by `gameresources.resources`); no scene at first: type info parses (5895 classes) but as in TNO only
  helper structs carry fields; `idPlayer`, `idGameLocal` and the player physics have no class entry. RTTI: `idPlayer` exe+0x29F7858, `idGameLocal` exe+0x2A227B8, **`idHavokPhysics_Player`
  exe+0x2704268** (no `idPhysics_Player`; Havok as in id Tech 7), `idUsercmdGenLocal` exe+0x27837F8 (one static instance at **exe+0x3DB7980**). No gyro strings; input cvars `in_joystick`,
  `in_invertLook`, `in_anglespeedkey`. The Havok physics is embedded in the player at +0xeb68. Turn diffs found player+0xd6ec as a yaw candidate; the `mg6` layout wrote there and every write
  was undone; **WriteWatch** showed it is one element of a world transform of something attached to the player (4x4 matrix multiply at `exe+0x126AEA0`): dead end. A 15 s 4 Hz recording
  found the source as in TNO: **`idUsercmdGenLocal`+0x10 = pitch, +0x14 = yaw** (accumulated, unwrapped), and the view in the Havok physics at **physics+0xff8** (= input angles + a constant
  delta). Live write test: input yaw +30 -> view turned ~30 degrees left and held; pitch +15 looked down. **`mg6`: the gyro is added to the input angles at exe+0x3DB7990** (checked at
  start: the qword at exe+0x3DB7980 must be idUsercmdGenLocal's vtable); `view_angles` reads them. **User on foot: native gyro, no stick fallback, applied yaw up to a full turn in 10 s;** a
  player change (load) was picked up 2 s later. Pause menu/cutscenes/wheels not gated (only focus). New WriteWatch name `player`. Saves backed up (`wolf2-saves-pre-gyro4all`).
  - **Tools** (`tools/idTech/`, research only; README section 2 has the route): `dumpimage.py` (read-only dump of the running decrypted exe), `livemem.py` (RTTI vtables, heap/static
    instances, `turn` snapshots; shared reader), `turndiff.py`, `record.py` (4 Hz spans), `analyse.py` (numpy), `series.py`, `disasm.py` (capstone, from `.pdata`), `poke.py` (the live write test;
    the only one that writes memory).
- 2026-10-20 **Build: Gyro4All 1.5.0** (user's version), `Gyro4All-v1.5.0.zip` (505 checks). Since 1.4.0: the stick camera and reworked open-loop stick fallback (XInput packet-number fix), the
  idTech module's MachineGames id Tech 6 path (Wolfenstein II), the `player` WriteWatch name, `Gyro4All.ini` template spacing.
- 2026-10-20 **Indiana Jones and the Great Circle (MachineGames' id Tech 7 fork; PC Game Pass 1.16.1.0, build `20260527-113405-151882`): native gyro works.** `TheGreatCircle.exe` (Vulkan through
  Streamline) can't be read from disk; no anti-cheat/Denuvo; community route only Lyall's GreatCircleFix. Saves backed up. First run as `winmm.dll`: loads, game thread adopted (`PeekMessageW` from
  `exe+0xE6DD39`), Vulkan swapchain captured; the game also reads the DualSense itself over HID. **`engine: none detected`**: resources are `base\gameresources_pc.resources` (+ `_patch1/2`),
  `meta_pc.resources`, `*.streamdb`; this is now a third idTech marker. Type info from a read-only image dump: id Tech 6/7 layout, 8951 classes but an entity-component game: **no `idPlayer`,
  `idGameLocal`, `idHavokPhysics_Player`, `idPhysics_Player`, `playerPState_t`, `usercmd_t`** in type info or RTTI; RTTI has **`idUsercmdGenLocal`**/`idUsercmdGen` and player jobs. Components with fields:
  `idUserViewAnglesComponent` (0x50; `minViewAngles` 0x0, `maxViewAngles` 0xc, `forcedViewAngles` 0x28, `viewAngles` 0x40), `idActorControllerComponent` (`lookAngles` 0x450),
  `idLocalPlayerComponent` (singleton 0x90), `idPlayingFPSCameraComponent`, `idRigEntityComponent` (`viewAnglesDelta` 0xab4); instances have no vtable (pools). The PC exe keeps the consoles'
  motion aim (`g_setting_motion_aim`, `joy_sixAxisSensitivity`, `joy_sixAxisAimMultiplier`, `joy_sixAxisInvertX/Y`, Switch 2 Joy-Con `joy_mouseSensor*`); untested whether the PC input path feeds
  it from the DualSense.
  - **View source found, as in Wolfenstein II:** `idUsercmdGenLocal` vtable exe+0x35FF948, **one static instance at exe+0x64A9A20**; **+0x9a8 = pitch, +0x9ac = yaw** (accumulated, unwrapped;
    +0x9b4/+0x9b8 per-frame stick rates). Live write (`poke.py exe+64aa3c8 1:30 0:-15`): yaw +30 / pitch -15 held; user-confirmed. Module `Layout::mg7`: chosen when the type info has no
    idPlayer, the RTTI has neither `idPlayer` nor `idHavokPhysics_Player` and has `idUsercmdGenLocal`; the generator is the single qword equal to its vtable in `.data`; the gyro is added at +0x9a8
    (pitch down, yaw left positive); the generator stands in for the player (`find_player`, target `idUsercmdGenLocal`; WriteWatch `player`). **User on foot: works ("perfecto").** Cutscenes: the game
    lets the player look around a little through the same input angles, so no cutscene gate (dead ends looking for one: generator +0x1570 is a sequence counter; a `.data` diff left 166k
    differing dwords; `idCutscenePlaybackComponent.isPlayingCutscene` +0xac never found). Pause menu, journal and map fine with only the focus gate. Other singletons for a later gate:
    `idPauseGameComponent` (`requestPauseCount` +0x10), `idUIMidnightInfoComponent.isRunning`. GAMES.md row added.
- 2026-10-20 **idTech: no store-specific exe addresses** (the three MachineGames/id Tech 5 games were found in Game Pass builds). Detection (marker files, type info, RTTI) was
  store-independent; exe offsets were not. **Wolfenstein II (`mg6`)**: input generator = the single qword in `.data` equal to `idUsercmdGenLocal`'s RTTI vtable, angles at +0x10; the Havok
  physics offset (+0xeb68) is tried first, else searched in the player's first 0x20000 bytes (`idHavokPhysics_Player at idPlayer+0x... (not +0xeb68)`); the gyro no longer needs it to write.
  **TNO (id Tech 5)**: Game Pass input angles exe+0x1C1CB40 tried first; when the check finds the player's structure right (view = command + delta) but the input angles wrong, the scan searches
  `.data` for the single float triple equal to the command's angles (SHORT2ANGLE, within 0.01, command yaw away from 0); the player offsets (idPlayer+0x310, physics +0x488/+0x43c/+0x343c,
  state +0x13c/+0x60) are still Game Pass's: a different build logs MISMATCH and the gyro stays off. **Indiana Jones (`mg7`)**: by vtable (`single_static`, shared lookup). **Sections**:
  `Image::load` took `.text/.rdata/.data` by name only; a DRM wrapper with renamed sections (Denuvo, as in Dead Space) left no scene: a missing section is now the largest of its kind.
  Other engines reviewed (RE, Frostbite, Unreal): no exe addresses in code. **Profile names were store-specific:** Game Pass builds run `<Project>-WinGDK-Shipping.exe` and never found the
  `-Win64-` profiles: new `Engine::profile_aliases` (Unreal: -Win64- <-> -WinGDK-) and `settings::use_profile_alias` (log `settings: no profiles\<stem>.ini; using <alias>.ini`); the probe still
  writes under the exe's own stem; games renamed per store (Epic's `IndianaEpicGameStore-Win64-Shipping`) need their own profile. 505 checks; not tested in game.
- 2026-10-19 **TNO with the packet-number fix: works, user-verified "acceptable"** (DualSense as XInput, `StickFallbackTest=1`, default model): learnt within 3 s; over two forced minutes it settled
  on `axial, dead zone 0.00, curve 2.75-3.00, yaw ~160-255, pitch ~95-110 deg/s, 0 frames late`, explaining 77-95% of the camera's motion (yaw creeps up: id Tech's acceleration while held). No drift.
- 2026-10-21 **Battlefield 4 (EA app build 179547, game 1.8.2.48475): first loading test.** Frostbite 3, DX11 (or Mantle), `bf4.exe` encrypted on disk (EA/Origin OOA: entropy 8.00, `.ooa` section; only
  static import `Core/Activation64.dll`), no Denuvo. **PunkBuster** installed: campaign only, remove the proxy before multiplayer. Loads as `winmm.dll` even with RivaTuner running (unlike Dead Space):
  `engine: Frostbite (detected)`, all inline hooks `MH_OK`. Input: the game opens the DualSense (054C:0CE6) over HID through dinput8 and reads it with DirectInput8 `GetDeviceState` (DIJOYSTATE2); the
  first read comes from **`IGO64.dll+0x42E6A`** (EA app in-game overlay). **Freeze with GyroLib's panel** (and later with the XInput watcher polling real XInput from its own thread): ~8-10 threads spin on
  one lock at `IGO64.dll+0x64054` (Present thread +0x1F5E4, IGO's own thread +0x18EB2), including Gyro4All's frame thread (through SDL's message pump) and the watcher (through `xinput1_4`). With the panel
  off in `gyrolib.ini` the game runs; **with the overlay turned off in the EA app the game and the panel run.**
  - **Frostbite 2013 type layout** (`fb_types.cpp`; verified: `DataContainer TypeInfo at bf4.exe+0x2BFDFA0`, 10210 types in ~50 ms): TypeInfo +0 info data, +8 next TypeInfo (one single-linked list), +0x10 u16 runtime
    id + u16 flags; classes +0x18 parent, +0x20 default instance, +0x28 class id, +0x30 first derived, +0x38 next sibling. Info data +0 name, +8 u16 flags (kind = flags>>4 & 0x1F), +0xA u16 size, +0x10 module
    record, +0x18 alignment, +0x1A field count; classes +0x20 parent, +0x28 create function, +0x30 fields; structs +0x28 fields; enums +0x20 values; arrays +0x20 element. Fields 0x18 bytes: name, u16 flags,
    u16 offset, u32 pad, type. The old reader failed because array types have an **empty name** (shared "\0") and the list's head (`WebBrowserSettings` exe+0x2C92D20) is newer than DataContainer (only 3765 of
    10210 types follow it). Reader: names may be empty; the layout is picked on DataContainer (2013 first by shape, then 2021); newer types are found backwards in one pass (every image qword pointing into the
    image, sorted; a TypeInfo whose +8 is known is added). Arrays named `<element>-Array`. Only data classes carry fields; runtime classes (`Soldier::ClientSoldierEntity`, 0x750) only size and parent.
    Camera-related types: `SoldierAimingSimulationData`, `SoldierAimAssistData`, `AimingComponentData`, `ClientAimingConstraintsEntity`. Dead Space not retested.
  - **XInput on protected exes** (`xinput.cpp`): translation used to be off for every inline-hook exe. Now `xinput9_1_0.dll` (loaded first) and `xinput1_3.dll` exports are hooked inline; `xinput1_4.dll` was left
    to GyroLib's SDL (changed 2026-10-22). BF4 imports `XInputGetState/GetCapabilities` from `XINPUT9_1_0.dll` (slots exe+0x1B413E0/+0x1B413D8). `WM_DEVICECHANGE` posted to 0 windows on Frostbite (the frame runs on
    Gyro4All's own thread): now this thread's windows, else every visible top-level window of the process. `profiles/bf4.ini`: `XInput=1`.
  - **Why the game never polled the translated pad:** BF4 creates its XInput devices from **DirectInput enumeration**: the enum callback exe+0x946690 calls exe+0x94A090(&guidProduct) = Microsoft's WMI
    `IsXInputDevice` sample (`Win32_PNPEntity`, "IG_" + VID/PID vs Data1); true -> an XInput device object (ctor exe+0x943BD0, vtable exe+0x1D413E0; update exe+0x94C2B0: `XInputGetCapabilities` while `[this+0x14]`
    is 0, then `XInputGetState`); false -> a DirectInput joystick. **Fix, user-verified** (`fb_xinput.cpp`, `dinputpad::on_create`, `xinput::listed_as_xinput`): from the game's first `DirectInput8Create`,
    the XInputGetCapabilities import slot -> its `jmp` thunk -> the 3 functions calling it -> their direct callers -> in the enum callback `call <check>` followed within 16 bytes by `test al,al; je` before the
    constructor's call: `IsXInputDevice at bf4.exe+0x94A090 hooked` in ~150 ms, answering true for Sony and Nintendo controllers (Data1 = MAKELONG(VID,PID)). The constructor asks `XInputGetCapabilities` once and
    never retries, so `listed_as_xinput` waits up to 1 s for the frame thread to show the controller and a listed controller skips auto mode's native signs. Moving, looking and buttons work in menus and game
    (Xbox prompts in the main menu, keyboard prompts in game: the game's own PC behaviour).
  - **Looking for the camera:** external read-only scans (`tools/frostbite/fbmem.py` takes `FB_EXE=`): the game keeps (yaw, pitch) pairs in radians copied into ~20 object kinds; every copy was rewritten within a
    frame except the yaw at +0x130 of an object with vtable exe+0x1E60780 (not reflected), which turned the camera 20 deg once but is **not stored state** (an outside write only counts when it lands in the
    frame's window, 1 in ~50). Pitch +0x134 isn't a source. **WriteWatch** on the per-frame yaw found the **soldier's aim update `exe+0xFBA340(aim, out_delta, input)`**: input `[r8+0xC]`/`[r8+0x10]` = look
    input (clamped to a unit circle) times a speed and frame time; current angles **`aim+0x14` (yaw) / `aim+0x18` (pitch)** radians (yaw wrapped by `exe+0xFAFFD0`, pitch clamped by `exe+0xFAFF40`); writes the
    target `aim+0x60/+0x64` (time +0x68) and **returns the frame's turn as a delta** at `[rdx]`/`[rdx+4]`. Called directly from `exe+0xFA6BFF` in `exe+0xFA69C0` with `aim = [rbx+0x10]`. Outside writes to aim+0x14
    turned the camera ~1 in 6 (race with the update); the gyro must go in from inside the update.
  - **Real gyro, user-verified "now it is good"** (`bf4_camera.cpp`, `sync_gyro_frame` in `fb_engine.cpp`, `xinput::right_stick_pushed`): inline hook on the aim update, found by its first 29 bytes and the target
    stores at +0x1E9; the player's record is the one whose look input moves while the XInput right stick is pushed (the gyro starts after one stick move). Where the turn goes (three tries): (1) into stored angles
    before the update: a quarter of the turn, judder (the 30 Hz tick `exe+0xFB9610` copies the target over the stored angles at exe+0xFB9943 when the +0x188 flag is set); (2) into both pairs: full turn but unstable
    (the update returns new - stored angles as the view offset, both moving cancelled in it); (3) **after the update, into the returned delta and the target**, stored angles left to the tick (flag off: into the
    stored angles before): smooth. Signs: the gyro's own go straight in (the opposite was inverted on both axes, although the camera transform had suggested otherwise; not resolved). Timing: the frame ran after
    each Present on its own thread but the aim update on a job thread; `sync_gyro_frame`: the player's aim update wakes the frame thread and waits (at most 4 ms) for a frame started after the request (0 misses in
    play, ~290 frames/s). Panel back on with the EA overlay off. Vehicles not tested.
- 2026-10-21 **Titanfall 2: Source module** (EA app v2.0.11.0, binaries 2017-12-05, last PC patch; plan in `games/Titanfall2/`; user-verified). `Titanfall2.exe` is an EA/Origin OOA stub loading
  `bin\x64_retail\launcher.dll`; the game DLLs are plain (`engine.dll` imports `PeekMessageW`, `WINMM.dll`; `inputsystem.dll` loads `XInput1_3.dll` at run time); no anti-cheat files; multiplayer out of scope.
  `src/engines/source/`: detection `r2\gameinfo.txt` + `bin\x64_retail\engine.dll`, inline hooks, `sees_menus` false. **The proxy must go in `bin\x64_retail\`, not next to the exe** (`launcher.dll` loads the engine
  DLLs by full path with `LoadLibraryExA(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH)`, so their imports are searched there then System32, never in the exe's folder); Gyro4All's folder is `bin\x64_retail\Gyro4All\`;
  `deploy-gyro4all.sh` takes `EXE=` (`EXE=Titanfall2.exe bash tools/deploy-gyro4all.sh "<game>/bin/x64_retail"`). Core: inline mode preloads `xinput1_3.dll`.
  - `CreateInterface("VEngineClient013")` -> `engine.dll+0x763B78`, vtable engine+0x5C9DB8: slot 63 = **GetViewAngles** (engine+0x510F0), slot 64 = **SetViewAngles** (engine+0x51150: rejects NaN/inf, normalizes);
    local client via engine+0x78210(slot) (-1 = active; clients at engine+0x7A64C0, stride 0x1B638); angles floats in degrees, Source senses, pitch -50..+75, roll 0. **Pause**: client+0x160 (byte) 0 in play, 1 in
    the pause menu (the only flag that followed 10+ user pauses). `engine.dll`'s `.data` is 0x145279C0 bytes in memory (stream, don't dump).
  - **SetViewAngles from the frame doesn't stay** (world angles are rebuilt every frame from the player's local angles; only the mouse step feeds a change back): diagnostic hook callers per frame: client.dll
    +0x250224, +0x2553AC (CreateMove), +0x256205, +0x256F13 and **+0x24F90C** (client+0x24F7A6: GetViewAngles, world-to-local client+0x2CBBC0, + the mouse's delta, local-to-world 0x2CB9A0, then SetViewAngles).
    Fix: SetViewAngles hooked (MinHook); `apply` accumulates the turn and the hook adds it to the angles the mouse step's call (return address client+0x24F90C, checked by `FF 90 00 02 00 00`) sets; dropped after 100 ms
    without a step call (the core's frame runs several times per game frame); `view_angles` includes the pending turn; gameplay view requires a mouse step in the last 100 ms. **User-verified: "gira entero y las
    direcciones son correctas"** (DualSense shown as XInput; mouse step took the gyro on 1438 of 1439 calls in 10 s at ~144 fps, frame cost ~45 us). Not checked: titan cockpit, wallrunning, cutscenes.
  - **Core: auto XInput no longer hides a DualSense the game closed unread, whatever opened it:** TF2's dinput8.dll enumeration opens the pad 8 times and never reads; the last of `xinput::frame`'s three hiding
    branches (the game opening the HID device) didn't skip `dualsense_ignored()`/`rejected`; now it does (shown ~5 s after start). `profiles/Titanfall2.ini` no longer sets `XInput=2`; RE7/RE8 keep theirs. User-tested
    with `XInput=1`: shown 4.7 s later, the game reads its sticks, the gyro turns the view.
- 2026-10-21 **Build: Gyro4All 1.6.0** (user's version), `Gyro4All-v1.6.0.zip`. Since 1.5.0: the Source module (Titanfall 2), `profiles/Titanfall2.ini`, inline mode preloads `xinput1_3.dll`, auto XInput fix for a
  DualSense closed unread after a DirectInput enumeration, `deploy-gyro4all.sh` `EXE=`.
- 2026-10-21 **Battlefield Hardline (EA app, Game Pass; build 612926): the BF4 route, real gyro on foot** (user-verified, DualSense). Origin OOA protection like BF4 (inline hooks); settings in `Documents\BFH`. Type
  reader 2013 layout unchanged (DataContainer `bfh.exe+0x2C38760`, 12326 types). The same soldier aim update at `bfh.exe+0x4388620`, same offsets; BF4's pattern missed it twice (`mov r11,rsp` is `49 89 E3`, target
  stores use other registers and store pitch first at +0x265): `bf4_camera.cpp` (log name "Battlefield") takes either and checks the stores by shape (+0x60/+0x64 in either order, then +0x68, one base register, first 0x300
  bytes). In a car cutscene the update doesn't run and the gyro is off; after a cutscene the soldier gets a new aim record (the right stick has to move once). **Race:** the camera model's pattern scan and the XInput
  `"IG_"` scan (`scan_image` shared one static buffer) ran together and found 0 functions: scans now serialised by a mutex. **No XInput import at all** (resolved at run time): new fallback route in `fb_xinput.cpp`: wide
  `"IG_"` string -> `find_lea_refs` (fb_types) -> its function = the check (`bfh.exe+0x3CB9140`), hooked 250 ms after the types; the game lists 054C:0CE6 as XInput and polls. Buttons and gyro work in the campaign.
- 2026-10-22 **Ghostrunner** (UE 4.26.2) and **Ghostwire: Tokyo** (UE 4.27.2): user reports both work with the generic view. **Kena: Bridge of Spirits** (UE4, also ReShade + RenoDX and `KenaCore.dll`): no panel, no
  gyro: the first process exits ~70 ms after start while a second `Kena-Win64-Shipping.exe` (the real game) starts, found the mutex taken and stayed a plain proxy; the relaunch comes from `CODEX64.dll`'s own
  `CreateProcessW` import and the hooks were import-table hooks on game modules only. Fix: `dllmain.cpp` hooks `KERNELBASE!CreateProcessA/W` inline (MinHook), so a relaunch from any DLL hands the session over.
  Gyro works (DX12, generic view). **Ghostrunner 2** (UE 4.25+ FField, D3D11, Epic): generic view works; **false "camera ignores the gyro" notice at start** because the main menu possesses `BP_MenuPawn_C` (not a
  DefaultPawn) whose camera is fixed, and AutoProbe started in the menu. Fix (`ue_engine.cpp base_view`, `ue::is_a_like`): a pawn whose class chain has a name containing "MenuPawn" is "no gameplay view" (also Valor
  Mortis). GAMES.md updated.
- 2026-10-22 **Builds:** Gyro4All 1.6.1 (user's version): CreateProcessA/W hooked inline in KERNELBASE (Kena). Gyro4All 1.6.2 (user's version): "MenuPawn" menus count as no gameplay view (Ghostrunner 2, Valor Mortis).
- 2026-10-22 **Mass Effect Legendary Edition (ME1): crash loop at start, `.ooa` = protected exe, UE3 detection** (Steam, `MassEffect1.exe` 2.0.0.48602; plan in `games/MassEffectLE/`). The user also has LEBinkProxy
  (`bink2w64.dll`), `ASI\` with AutoTOCLE, ReShade (`dxgi.dll`) with the Luma addon. The exe is BioWare's UE3 fork packed by EA's activation wrapper (`.ooa`; imports only `Core/Activation64.dll`, `PhysXLoader64.dll`,
  `d3d11.dll`, `dxgi.dll`, with name tables); cooked packages in `BioGame\CookedPCConsole`; `um scan` says "Unknown native engine".
  - **The game hung at start** (`Gyro4All.log` stops after `engine: none detected`, a WER loop `ntdll.dll+0x1a12d`, 0xc0000005, keeping the session mutex): the `.ooa` stub's unpacked code calls `LoadLibraryExW` ->
    our `DllMain` -> `hooks::init` -> `patch_slots` -> **`strcmp` access violation**: import tables walked while the stub is still filling them, a name RVA outside the image; the fault inside DllMain then recurses
    (`__CxxFrameHandler4` -> `__vcrt_getptd` fails -> `abort`). Why import-table mode: the stub's small import table has name tables so `scan_imports` didn't see a protected exe (BF4/Hardline/TF2 only worked because
    their modules force `inline_hooks()`). Fix (`hooks.cpp`): an exe with an `.ooa` section is a protected exe (inline hooks, import tables untouched; log `imports: exe has an .ooa section`); every import walk checks
    each RVA against `SizeOfImage` (`in_image`).
  - New engine module **`Unreal3`** (`src/engines/unrealEngine3/`): detection only at first: a `<root>\<Game>\CookedPC*` folder two levels above the exe's folder, after Unreal in the detection order. The fix
    works: the game starts, `engine: Unreal3 (detected)`, PeekMessageW frame on the main thread (first call from `gameoverlayrenderer64.dll+0xB1E26`), D3D11 panel via `gyro4all.dll`, motion ~460 samples/s.
  - **XInput translation didn't reach the game:** the DualSense appears in DirectInput (054C:0CE6, 6 axes, 15 buttons), XInput auto showed it after 4 s, but the game never polled. The exe imports `XINPUT1_4.dll`
    (resolved table by the protector, not in the PE headers): `xinput1_4!XInputGetState` at **`MassEffect1.exe+0xF9DE58`**, `XInputSetState` `+0xF9DE60`, thunks `+0xDAE85A`/`+0xDAE860`; no `XInputGetCapabilities`. First
    fix: in inline mode xinput1_4's `GetState/SetState/GetCapabilities/Enable` hooked too (`hk14_*`, own originals; calls from gyrolib.dll/SDL, Gyro4All, gyro4all.dll or xinput9_1_0 pass through; nativepad's
    diagnostic hook on the same export removed since MinHook can't hook a function twice; also benefits BF4/Hardline/TF2). Still no poll: UE3's controller list (globals `+0x1B9B250` array of 0x1F4-byte entries,
    `+0x1B9B258` count; entry +0 IDirectInputDevice8, +8 type (3 = XInput), +0xC slot, +0x1E0 instance GUID, **+0x1F0 "connected"**; start-up `+0x98CF60` adds 4 XInput entries, the DirectInput enum callback
    `+0x989F40` adds an XInput entry for devices `+0x98E280` (WMI "IG_") calls XInput, other pads only with whitelisted caps, which the DualSense isn't) is polled by `FWindowsViewport` (vtable `+0x120E7B8`) slot 23
    `+0x9943D0` ProcessInput, gated on `GetFocus() == [this+0xC4]`, then `XInputGetState(slot)`; a failure clears the flag for good. After playing all four flags were 0: XInputGetState was called and failed for every
    slot, yet Gyro4All's hook never ran: the export's first `jmp` goes to a relay page whose target is **`gameoverlayrenderer64.dll`** (Steam hooked the export after Gyro4All and answers itself without calling the
    original). Import-table mode never has this problem. **Fix: `hooks::redirect_slots`** (one sweep of the exe's non-executable sections, readable pages only, for pointer slots holding given exports, repointed to Gyro4All
    and kept in `g_patched`/verify()); `xinput.cpp` in inline mode points the protector's xinput1_4 slots at `hk_imp14_*` (up to 5 passes; log `hooks: xinput1_4.dll!XInputGetState slot at MassEffect1.exe+0x... (the
    protector's import table) now points to Gyro4All`); real function = the export itself; a `thread_local` guard (`Inside`) lets nested XInput calls pass straight on. 661 checks; not tested at this point.
- 2026-10-22 **Mass Effect LE (ME1): XInput translation works** (user; DualSense; Steam overlay on): `hooks: xinput1_4.dll!XInputGetState slot at MassEffect1.exe+0xf9de58 (the protector's import
  table) now points to Gyro4All` (and SetState `+0xf9de60`), `the game polls XInputGetState` 7 s later; sticks and buttons work. The Steam overlay's hook answers XInputGetState without calling
  through even with Steam Input off for the game.
- 2026-10-22 **ME1: UE3 reflection and the camera** (read-only external reads, level BIOA_STA00, on foot). **GObjects** `MassEffect1.exe+0x1770670` (TArray<UObject*>, 183762 objects), by shape; matches
  ME3Tweaks' LE1-SDK, which also gives **GBioNamePools** `+0x16A2090` (array of chunk bases; chunk 0 `+0x1338FA0` static, chunk 1 heap; a copy of chunk 0's base at `+0x169A080` is a decoy). **FName** (LE1-SDK):
  `Offset:29` (bytes into the chunk), `Chunk:3`, then int32 Number; name entry `u32 FlagsAndMeta`, packed 8-byte `HashNext`, text at +12. Dead ends: a flat pool base, a GNames-style TArray<FNameEntry*>.
  **UObject** +0x08 index, +0x40 Outer, +0x48 Name, +0x50 Class, +0x58 ObjectArchetype. **UProperty** +0x70 ArrayDim, +0x74 ElementSize, **+0x94 Offset** (found from FMatrix's planes at 0/0x10/0x20/0x30);
  UBoolProperty bitmask at **+0xD0**; UStruct.SuperField **+0x60**. By reflection: `Engine.Engine.GamePlayers` +0x498, `Player.Actor` +0x68, `Actor.Rotation` +0x114, `Actor.Location` +0x108,
  `Controller.Pawn` +0x3DC, `Actor.WorldInfo` +0x1AC, `WorldInfo.Pauser` +0x644, `PlayerController.PlayerCamera` +0x6B0, `Camera.CameraCache` +0x45C (POV rotation +0xC), `Camera.ViewTarget` +0x3EC; no
  `bShowMouseCursor`. Scene: `Transient.GameEngine_0` -> `LocalPlayer_0` -> `...PersistentLevel.BioPlayerController_1` (pawn `SFXPawn_Player_1`, camera `SFXPlayerCamera_1`). The controller Rotation
  (65536 units/turn; pitch stored 0..65535, unwrapped yaw) is followed by the pawn and the camera cache. **Live write test:** yaw +45 deg stayed and the camera followed 1:1; pitch writes stayed and
  the camera followed; -80 was clamped to -60 by the game. Other pitch copies are effects (`BioPawn.m_vAimDirection` +0x760, ViewTarget POV +0x3F8, CameraCache POV +0x468,
  `SFXPlayerCamera.CameraDesiredPosition` +0x5A4).
  - `Unreal3` module rewritten: all found at run time (GObjects by shape with the index check, the chunk array by the class Class's name plus sample names from every chunk in use, the property offset from
    FMatrix, members and GameEngine by name; retried every 2 s, the stick fallback after 60 s). Gameplay view = controller has a pawn and `Pauser` empty; apply adds the gyro to Rotation (fractions kept);
    `view_angles` reads it. **User: the direct gyro works "perfectly"**, but it kept turning behind the pause menu (`WorldInfo.Pauser` stays empty there; game time doesn't stop). Snapshot diff menu closed vs
    open: new `BioSFPanel`/`GFxMovie` objects under `SFXGameViewportClient_0.MassEffectGuiManager_0` and **`WorldInfo.bPlayersOnly`** (+0x790, mask 0x80). Paused = `Pauser` set or `bPlayersOnly`; no gameplay view
    while `WorldInfo.bIsMenuLevel` (the main menu `entrymenu`; its controller also has a pawn). Both bools by reflection (offset + mask checked as a power of two). **Verified in game** ("funciona"):
    `ue3: GObjects at MassEffect1.exe+0x1770670 (122796 objects), names at +0x16A2090, property offset field +0x94, 21 ms`; menu reads `world is a menu level`, save `a game level`, pauses log `frozen
    (bPlayersOnly: a menu)`. Open: dialogue/cutscenes (the gyro still turns the controller), the Mako, other full-screen menus, ME3.
- 2026-10-22 **`tools/unrealEngine3/ue3tool.py`** (new, README): `find` (GObjects by shape; Outer/Name/Class as three consecutive pointers, Class = the field where the class Class points to itself; names as
  BioWare SFXName chunks or UE3 GNames; UProperty offsets from FMatrix; UBoolProperty mask; SuperField), `objects`, `props`, `scene` (WorldInfo bPlayersOnly/bIsMenuLevel/Pauser), `snap`/`diff`, `write-test`
  (the only writer); layout cached in %TEMP%. Tested on ME1 (`find` 2.7 s).
- 2026-10-22 **ME1: ReShade stopped loading with Gyro4All (our fault).** On protected exes (inline mode) the overlay's `hook_system_factory` loaded `System32\dxgi.dll` by full path right after DllMain, before
  the game's own dxgi import; the loader then gives every "dxgi.dll" request that module, so the wrapper in the game folder never loads. Fix (`overlay.cpp`): if no dxgi.dll is loaded and one sits next to
  the exe, load it first (`overlay: the game's own dxgi.dll (a wrapper, e.g. ReShade) loaded first`). Possibly the same with BF4/Hardline/TF2 with a dxgi wrapper (not checked).
- 2026-10-22 **Build: Gyro4All 1.6.3** (user's version), `Gyro4All-v1.6.3.zip`: Mass Effect LE (ME1) with the new `Unreal3` module (direct gyro through the controller's Rotation, off in pause/main menus);
  `.ooa` exes treated as protected and bounds-checked import walks; XInput on protected exes through xinput1_4 and the protector's own import slots (ahead of the Steam overlay's hook); a dxgi.dll wrapper loaded
  before System32's on protected exes; `ue3tool.py`.
- 2026-10-23 **ME2: no camera, two causes** (Steam; `Game\ME2\Binaries\Win64`; log `ue3: property Engine.GamePlayers not found`). **Decoy name array:** `MassEffect2.exe+0x16609F8` has chunk 0's base right and
  junk after it; the real chunk array is **`+0x1668A10`**. Both module and tool sampled names only from the first objects (all chunk 0), so the decoy passed; fix: samples spread over all of GObjects (module ~20000,
  tool ~4000, 24 per chunk; the class Class still searched in every one of the first 20000 objects). **The engine is `Transient.SFXEngine_0`** (a GameEngine subclass): the first non-default object whose class
  chain (SuperField) reaches `GameEngine`. ME2 layout: GObjects `+0x173CC48` (153373 objects), Offset +0x94, mask +0xD0; `GamePlayers` +0x498, `Player.Actor` +0x68, `Actor.Rotation` **+0x124**, `Controller.Pawn`
  **+0x294**, `Actor.WorldInfo` +0x1B0, `WorldInfo.Pauser` +0x684, bPlayersOnly +0x7D8/0x80, bIsMenuLevel +0x7D8/0x4000. Scene `SFXPlayerController_3`, pawn `SFXPawn_PlayerSoldier_1`, camera `SFXPlayerCamera_3`.
  **Verified in game** ("fixed", DualSense). **Build: Gyro4All 1.6.4** (user's version): ME2 support; GAMES.md ME2 row.
- 2026-10-23 **Metro 2033 Redux (4A Engine) recon and slices** (plan in `games/Metro2033Redux/`; Steam 286690 buildid 26162904 "Next-Gen Update", `metro.exe` 1.0.0.4, **SteamStub** `.bind`, `.text` entropy 8.0).
  Imports `USER32!PeekMessageA`, `WINMM.dll`, `XINPUT1_3.dll` by ordinal (#2, #3), `DINPUT8!DirectInput8Create`, `HID.DLL` + `SETUPAPI.dll` (own DualSense path, adaptive triggers), `amd_fidelityfx_dx12.dll`;
  window class `_uengine_`; no anti-cheat; pad cvars `joy_sens_x`, `joy_sens_y_scale`, `joy_sens_aiming_x`, `aim_assist`, `invert_y_axis`, renderer `r_api`. Slice 0: the proxy loads under SteamStub, `PeekMessageA`
  hooked via the import table (frame on the main thread from `metro.exe+0x656273`), the game opens the DualSense over HID itself (64-byte USB reports 0x01, overlapped reads): XInput translation off, `nativepad` follows
  its reports; the Unreal fallback's heap scans (~3 GB, ~0.5 s each, every 30 s) found nothing and wasted work.
  - **4A module** (`src/engines/4aEngine/`, namespace `foura`, `Engine=4A`): detection `content.vfx` + `content*.vfs0` next to the exe; window class `_uengine_`; first `no_camera` (blind stick fallback; user confirmed
    it turns the view); menus = mouse cursor shown (`GetCursorInfo`); cursor gate works (Steam overlay focus stops the gyro). **No panel:** `metro.exe` imports no dxgi/d3d11/d3d12 (loads them itself), so the
    import-table `CreateDXGIFactory*` hooks never fire; `OverlayFactory=system` brought the panel up, then a crash before the main menu (below). Later made automatic: `overlay.cpp loads_graphics_api_itself` +
    `hooks::modular()`: when no game module imports dxgi/d3d9/d3d10/d3d10_1/d3d11/d3d12/opengl32/vulkan-1, the build isn't modular, the exe isn't protected and no `OverlayFactory=` is set, the System32 factory's vtable
    is hooked (`the game imports no graphics API (it loads DXGI itself)`); `profiles/metro.ini` removed (only metro.exe matches among the installed exes).
  - **Crash with the system factory: stack overflow `0xC00000FD`** ~12 s in, when the game makes a second swapchain (same window): ~19,900 cycles of `hk_present` -> `gameoverlayrenderer64.dll+0x9b72d` (Steam's
    Present hook at `+0x9b700`; for `DXGI_PRESENT_TEST` it calls its saved original, which is our hook). Steam overlay off in the game's properties changes nothing (the DLL is still injected). Offline test
    (plain D3D11 + flip swapchain): the swapchain vtable is in `dxgi.dll` and file values of slots 8/13/22/39 equal the live ones. **Conclusion: Steam inline-detours the function the swapchain's Present slot points to,
    each time the game creates a swapchain, keeping one saved original.** At swapchain 1 that was dxgi's Present; at swapchain 2 the slot points to our `hk_present`, so Steam detours our hook and its original becomes our
    trampoline also for its dxgi detour: a loop. Other games make one swapchain per session (ME LE runs the same path and works); any D3D10/11/12 game recreating its swapchain under Steam can hit it (a latent core bug).
    Dead ends (reverted): loop check switching the slot's original to its file value (dxgi's real Present was the very function in flight); same with the first saved original as fallback. **Fix
    (`overlay.cpp`, `com::unhook_slot/rehook_slot`):** every patched swapchain slot is tracked (`patch_swapchain_slot`); around the game's CreateSwapChain, CreateSwapChainForHwnd,
    D3D10/D3D11CreateDeviceAndSwapChain (nested count once) they hold their saved originals, so a hooker at creation sees the same functions; afterwards our hooks go back (a value written meanwhile becomes our
    original). Log `overlay: N swapchain hook(s) set aside while the game creates a swapchain`. **Verified in game** (no crash, panel works, 5 hooks set aside at the second swapchain).
  - **ReShade's open overlay blocks our panel's mouse and keyboard:** ReShade (`source/input_windows.cpp`) blocks input process-wide while its overlay is open (message hooks turn mouse/keyboard messages into WM_NULL,
    GetCursorPos returns the last position, GetAsyncKeyState/GetKeyState return 0, raw input and Windows hooks filtered). Not fixed (user's call); TROUBLESHOOTING: use the controller or close ReShade's overlay. Possible fix:
    GetCursorInfo for the cursor and `win32u!NtUserGetAsyncKeyState` for buttons while the panel is open.
  - **Camera angles found.** Dead ends: Cheat Engine scans found only copies (the yaw in the NVIDIA driver's constant buffers written by `nvwgf2umx.dll+0x5ddc62`, direction-vector components, a static renderer object
    `metro.exe+0xF4C430` whose view x projection matrices include an element following the yaw: writing any of them reverts). Method that worked: snapshot all writable memory (4.4 GB, 64 MB batches), turn with
    SendInput relative mouse (+D, -2D, +D, idle), keep values whose change is proportional and returns; then turn ~99 deg and keep only values whose rate per count is unchanged (an angle). 6465 -> 26 candidates.
    **Master view angles** (heap): `float pitch, yaw, roll(0)`, radians, 0.000823 per mouse count; yaw grows turning right, doesn't wrap; pitch grows looking down. Writing it plus a mouse nudge moves every copy; copies alone
    revert. A 2-level pointer scan gave 291 chains; after a checkpoint reload only **`[[metro.exe+0xFBD820]+0x30]+0x650`** survived (the global's object vtable `metro.exe+0xB22D20`, its `+0x30` object with vtable
    `metro.exe+0xA81F08`; yaw repeated at +0x654, +0x660, +0x66C; no RTTI).
  - **4A module: direct camera** (`foura_engine.cpp`): reads `[[global]+0x30]+0x650` (finite, |pitch| <= 1.6, roll 0) and turns it (yaw += gyro yaw, pitch -= gyro pitch, radians); view 1 while readable and no cursor.
    The global from the code signature `48 8B 1D ?? ?? ?? ?? 48 8B 5B 30 48 85 DB 74` (`metro.exe+0x1F4A4B`, unique; the global is used all over), else this build's fixed offset when both objects carry this build's vtables.
    **Verified in game** ("yaw and pitch ok"; 120-165 updates/s, 98-99% turned; the view follows writes without any mouse input). Issues and fixes: pushing the pitch to the limit started the stick fallback for a
    second (the module's 85 deg clamp stopped the turn, judged "the game undid the writes"; the game's own limit is 90 deg) and its learning saved nonsense models into the installed ini -> new
    `Engine::limit_turn(pitch, yaw)` (host trims the turn before `apply`; the fallback judges the gyro's share of the trimmed turn; 4A: +-pi/2). Pause: scan with Esc sent as a DirectInput scan code found game clocks
    (float seconds) that stop while paused: `metro.exe+0xF58420` (real rate) and `+0xF58428` (half rate) inside the static renderer object `metro.exe+0xF4C430` (+0xBFF0, +0xBFF8); object found by `48 8D 0D ?? ?? ?? ?? 48 FF A0 90 02`
    (`metro.exe+0xEE1A7`, unique), else the fixed offset when its vtable is `metro.exe+0xE53C88`; paused = the clock stood still 200 ms while frames go on. Verified in game.
- 2026-10-23 **Build: Gyro4All 1.7.0** (user's version), `Gyro4All-v1.7.0.zip`: Metro 2033 Redux with the new `4A` module (direct gyro, code signatures, pitch limit, pause by the game clock, cursor gate);
  `Engine::limit_turn`; automatic system DXGI factory; swapchain hooks set aside during creation; GAMES.md and TROUBLESHOOTING (the panel and ReShade's open overlay).
- 2026-10-24 **Metro: Last Light Redux (Epic) with 1.7.0.** User report "no panel, no gyro, no stick fallback": the 4A module works unchanged (same code signatures, same offsets as 2033's exe: global `+0xFBD820`,
  clock `+0xF58420`; Epic's `EOSOVH-Win64-Shipping.dll` rewrites some imports, harmless). **Cause:** the game window never had the foreground (`focus: lost to brave.exe` for ~5 min until the user clicked it; Windows'
  foreground lock behind the fullscreen game; the game reads the pad over HID, which works without focus), and our focus gate keeps the gyro, fallback and panel key off. Workaround: click the game once; not fixed in code.
  - **Crashes at start (not Gyro4All):** 4 runs, `0xC0000005` at `metro.exe+0x9420F8` on a game worker thread ~0.3 s after the second swapchain: a file lookup returns null and the code reads the fd for `_lseek`;
    cause: the intro videos renamed to skip them (`intro.ogv-off`, `legal.ogv-off`); the build doesn't tolerate missing startup videos (names restored). GAMES.md: don't rename the intro videos.
  - **The gyro turned the main menu's camera** (a live scene with readable global/angles, running clock, no cursor; same classes as a level). Snapshot diff (main menu -> level -> level alt-tabbed -> main menu):
    **`metro.exe+0xF4B838` dword, 1 in the main menu, 0 in a level** (also `+0xF49424..0xF49434` -1/0; the global's `+0x2E4` and `.data +0xF44FCC` did not survive a second menu). Set to 1 at `metro.exe+0x73C1AA`
    (`mov dword ptr [rip+flag],1; mov word ptr [rip+..],di; test rsi,rsi`, unique), to 0 at `+0x73C0EB`; read at `+0x591538`, `+0x5930C5`, ... `kMenuCode` finds it on the first frame (`4a: main-menu flag at ...`);
    while 1 the module reports no gameplay view (`4a: main menu` / `left the main menu`). **Verified in game** ("todo cuadra"). **Build: Gyro4All 1.7.1** (user's version): Last Light Redux support, main-menu flag.
- 2026-10-24 **Metro Exodus Enhanced Edition (Steam), 4A module: stick fallback first, then the camera found.** `MetroExodus.exe` (`.bind`, `.text` plain on disk, entropy 6.5), DX12 (system DXGI factory path, panel
  works). None of the three Redux signatures match, and the module took 2033's fixed offset (wrong objects in Exodus): no view, and since a global "existed" the blind stick fallback never ran. Fix (`exe_is_metro()`): fixed
  offsets only for `metro.exe`; otherwise `no camera hook (gyro as right stick)`: **stick fallback verified in game** (DualSense USB).
  - Camera search, rounds 1-2 (dead ends): the "4x4 camera ring" in 4 MB blocks is the player's **body**; the 4 MB "frame blocks" are **worker-thread stacks** (a hardware write breakpoint hit inside `__chkstk`): every
    matrix copy found there was a local variable, why whole-memory searches kept landing on copies. No float/double holds the view yaw as an angle (whole-memory deltas over two turns, 76 and 137 deg, left random
    matches; pitch limit ~60 deg down); the only full rotation was the NVIDIA driver's copy. A cdb hardware write breakpoint attach without `-pd` killed the game when cdb was killed (with `-pd` attach/detach is harmless).
  - **Offline disassembly (`.text` plain), with a ref-scanner bug fixed first** (`insn_rva = i + text_rva`, not the file offset; earlier ref addresses were off by 0x400 and unreliable): the debug print `"Camera. Position: {%f;%f;%f},
    Angles: {%f;%f;0}"` is in `exe+0x585050`; the camera state is a 4x4 float matrix in `.data` at `0x17033d0..0x170340f` (position `0x17033d0`, orientation `0x17033e0`), written by 5 `movaps` in `0x62ccxx..0x62e7xx`;
    update function `0x62cac0` (camera object in rcx, spins a lock, calls `0x62ce30` which builds an orthonormal matrix from position + two basis vectors as doubles) writes the result to the `.data` matrix and
    `[camera_obj+0x18..0x58]` (double matrix, what the renderer uses). Its only caller `0x81f790` derives them from `[actor+0x9d0]` (a 3x4 float matrix at +0x50), so the `.data` matrix is recomputed every frame.
  - **View angles found and write-tested in game (2026-10-25):** breakpoint chain (cdb `-pd`, `ba w4`, one hit then `.detach`) from the static view matrix `exe+0x30021e0` (built at `exe+0xE66369` by `cross(fwd,up)`) via
    `exe+0x30049b0` and the camera globals `exe+0x17033d0` to `0x62cac0`/`0x81f790` and the camera object `[player+0x9d0]` (+0x50 pos, +0x60 fwd, +0x70 up; also `pitch, yaw, roll` at +0x14/+0x18/+0x1c, undone within 30 ms).
    **The player (`[[exe+0x1696ce0]+0x30]`) holds `float pitch, yaw, roll` at `+0x9e0`** (radians; pitch grows looking down, yaw = heading, roll 0), same layout as Redux's `+0x650`; copies of the yaw at +0x9f0, +0xa08,
    +0x3d6c follow. Write test (yaw +0.35, pitch +0.2) stuck; every copy followed within a frame. Global signature (Exodus only, no match in Redux): `48 8B 05 ?? ?? ?? ?? 48 8B DA 48 8B 48 30 48 85 C9` (3 matches, all
    `exe+0x1696ce0`). Module: Redux signature first, then Exodus's with angles at +0x9e0; the rest is the 2033 path. In game: `gyro on (target view angles)`; the player object moves on loads (handled per frame);
    Exodus's yaw wraps at +-180. The gyro stayed suspended by GyroLib's activation until the user changed settings. **User verdict: works** (directions right, smooth, no trouble at the limits). Still open: its own
    pitch limits (~60 deg down measured vs the code's +-90), pause (no clock signature; the game pauses when not in front), the main menu and pause screens without a mouse cursor.
  - The method is now a tool, `tools/camtrace/` (README with the steps), and AGENTS.md makes it the next step when an engine's own route doesn't find the camera. Checked live on Exodus: `dump` with the pointer path,
    `snap` + `vectors` (after a 68 deg turn the view's group ranks first: 32 copies incl. `exe+0x17033e0`), `write-test` on the yaw (kept), `find-vec` only matches stack copies while the game runs (meant for the paused game),
    offline `refs` and `sig` reproduce the findings, `bp exe+0x3002200` hit the same writer (`exe+0xe66369`) and detached cleanly. **Build: Gyro4All 1.7.2** (user's version): Metro Exodus EE, the camtrace tool,
    README "Main Features", GAMES.md fixes.
- 2026-10-26 **Mass Effect: Andromeda (EA app, Frostbite, 1.7.2, DualSense, Steam Input not running): first run.** Detected as Frostbite; `.ooa` (inline hooks); xinput1_4 slot found in the protector's table (`exe+0x431e510`),
  `IsXInputDevice` found from its `IG_` string and hooked, the game polls XInputGetState. **Bug: the XInput translation flipped every second** ("shown" / "not shown: a real XInput controller is connected"): this system's
  `xinput1_3.dll` is GameInput's `xinputforwarder` (imports no driver calls, loads `xinput1_4.dll`); the watcher's "real" XInputGetState is xinput1_3's trampoline, which then calls the hooked xinput1_4 export from
  inside xinput1_3, not passed through, so it got the translated controller and counted it as real. Fix: `passes_through` also lets xinput1_3's calls through (674 checks; not run in game). Types: `DataContainer TypeInfo at
  exe+0x48492B0`, "info data layout unknown", only 312 types (Dead Space ~23,885): the reader doesn't understand this Frostbite's TypeInfo yet; no camera model fits; the blind stick fallback runs. `TypeDump` was `*` by
  default (every Frostbite game wrote `Gyro4All-types.txt` on every start): now unset (dump only when no camera model fits; `TypeDump=<filter>`/`*` always, `0` never; INIs created by 1.7.x keep `*`).
- 2026-10-26 **Build: Gyro4All 1.7.3** (user's version), `Gyro4All-v1.7.3.zip`. Since 1.7.2: the XInput translation no longer flips every second where `xinput1_3.dll` is GameInput's
  forwarder to xinput1_4 (protected exes: Mass Effect: Andromeda); Frostbite's `Gyro4All-types.txt` only written when no camera model fits (or `TypeDump=`).
  - **Andromeda (cont.): the exe is encrypted on disk** (`.ooa`/`.ooadk`; `typeinfo`/`fieldinf` random bytes), TypeInfos readable only in the running game. **2017 info data layout** (from the 312 raw info
    data): the 2013 one with the array type's TypeInfo inserted at +0x18: name +0x00, flags/size +0x08 (kind = flags>>4 & 0x1F: class 0x35, array 0x41, enum 0xC089, struct 0x29), module +0x10, array type
    +0x18, alignment/field count +0x20, then classes: parent +0x28, fields +0x30, then functions; arrays: element +0x28 (info data 0x38 bytes); enums: values +0x28; structs: fields from the first of +0x40,
    +0x38, +0x30, +0x28 whose records read. Field record = 2013's (0x18 bytes). `fb_types.cpp` layout 2017 checked on DataContainer after 2013 (`shaped_2017`). **In game: 23,345 types in ~0.1 s with fields.**
  - Camera system: `ContactCamera::` (Cinebot controllers: `InputController` with `InputControllerAxesStrategy`, `SplineAxesController`, `RotationController`, `FreeController`, `RecenterController`,
    `CollisionController`; `GameClient::ClientPlayerCameraEntity`); runtime classes have no reflected fields, only their `*Data`. Input goes through **InputTree** (`Intent_LookPitch = 4`, `Intent_LookYaw = 5`,
    `Input_LookYaw = 35`, `Input_LookPitch = 36`): the counterpart of Dead Space's EIAYaw/EIAPitch. **Dead ends:** a value search for the look levels (812k -> 485 floats; none persistent; the levels don't persist between
    ticks or aren't floats in [-1,1]); `snap --heap` + `vectors` after an ~80 deg turn: 957k turned vectors dominated by bones; a live search for world transforms (`camxform.py`, scratch: every 16-byte aligned
    LinearTransform in writable memory, turn ~100 deg then tilt ~30 deg) found dozens of copies, two in the exe's static data (`exe+0x4315820`, `exe+0x42ea630`), all rewritten every frame (`write-test` undone in 30 ms);
    angle search for a plain yaw float: none.
  - **Breakpoints chain (cdb `-pd`, one hit each, game kept running):** `exe+0x4315840` written by the function at `exe+0x16e7890` (copies 4 rows from a heap source with FOV 1.0472 and aspect 1.7778); the source's
    forward row written by `exe+0x35f7d0` (render view `setTransform`, builds frustum planes) called by `exe+0x15173aa`; `view+0x40`'s real writer `exe+0x1516944` (in `exe+0x15168c0`), called from `exe+0x14fb552`; ends on
    render job threads' stacks (view requests copied per frame). Vtables/live instances (`vtables.py`, scratch: getType = `lea rax,[rip+type]; ret`; vtable = the `.rdata` qword pointing to it; instances = heap
    qwords equal to the vtable): `InputControllerAxesStrategy` getType `exe+0xbc3cf0`, vtables `exe+0x2e19b50`/`exe+0x2e514d0`, 4 hits; `InputController` `exe+0xbaa340`/`exe+0x2e4b7d8`, 2; no instance of
    `SplineAxesController`/`RotationController`/`FreeController`/`ClientPlayerCameraEntity`. Live controllers come in pairs (`InputController`, `TrackableController`, `SpringController`, `CollisionController`, `ShakeController`, ...).
  - **The camera's angles: `InputControllerAxesStrategy` `+0xC0` (yaw) / `+0xC4` (pitch), float degrees** (`objdiff.py`): the camera's absolute yaw relative to the character, wrapping 360 -> 0 (constant offset plus a spring lag
    ~6 deg), pitch the same (up positive; idle a few seconds and the camera recenters behind Ryder, pair back to 0). A direct write is rewritten every frame (output). Written by `exe+0xbc458b` (`mov [r12],rax`, 8 bytes)
    in the strategy's per-frame input read `exe+0xbc44e0` (input wrapper `[strategy+0xa0]`, `call [rax+30h]`), and **a second time per frame by the odometer update `exe+0x5e7f60`** (vtable `exe+0x2d73a18` slot 3 at
    `exe+0x2d73a30`; object `[strategy+0xb8]`; args: odometer, Vec2 axes in rdx, Vec2 speeds in r8, dt in xmm3, `float* pair` 5th): `pair.yaw += rate.x * [odo+0x10] * scale * dt`, `pair.pitch += rate.y * [odo+0x14] * scale * dt`.
    Each frame: `exe+0xbc44e0` loads the camera's current angles into the pair, the odometer adds the stick's turn, the camera follows the pair.
  - **Camera model `mea_camera.cpp`**: the odometer update found by its prologue (`4C 89 44 24 18 48 89 54 24 10 57 48 83 EC 50 48 8B 05`, once in `.text`) checked against its accumulation at +0xF9/+0x120; the vtable
    slots holding it swapped for a hook that calls the game's and then adds the gyro's turn since the pair's last call (yaw subtracted, pitch added), after `sync_gyro_frame`; gate: an odometer update in the last
    100 ms. **User: works on foot** (`camera model Andromeda resolved (32 ms)`, slot `exe+0x2D73A30` hooked, one odometer/pair per camera mode, ~95 updates/s, off in menus). **Except the scanner (d-pad down): the gyro's
    pitch is inverted there** (the stick isn't; the scanner is a camera mode of its own with its own strategy/odometer/pair sharing the same data and wrappers; FOV 60 -> 45).
  - **Fix: the pitch sign learned from the view.** First learning from the stick failed a gyro-only player and depended on order; replaced by `find_view()`: the static render view copy found by the code that fills it
    (`0F 10 01 0F 29 05 rel32` / +10 `0F 10 49 10 0F 29 0D rel32` / +21 `0F 10 41 20 0F 29 05 rel32`, also required: the 10 bytes before `48 8B 0A 48 8D 97` because the pattern matched 4 times; `exe+0x4315820`), whose
    third row is the view's back axis (view pitch up = -asin(y)). Each pair's pitch input per update (smoothed x0.8) is correlated with the view's tilt since the pair's last update (score x0.98, tilts over 10 deg ignored as
    cuts); at +-2 deg^2 the pair's relation is set (logged) and a pair that tilts the view down gets the gyro's pitch inverted. Absolute, so camera order and the invert-Y option don't matter. **User, gyro only: works**, the
    scanner's pitch is right (1293 of 4564 updates inverted). Pair addresses can come back as another camera (strategies freed per mode) and a blend can briefly read wrong; it settled each time.
  - **Incident (tooling):** a conditional execution breakpoint that never hit in 120 s; the script's subprocess timeout killed cdb while attached with the hardware breakpoint set, and the game was gone. Never kill an
    attached cdb: let the script `.detach` on its own.

- 2026-10-27 **Warhammer 40,000: Space Marine 2 (Steam, "Swarm" engine; 1.7.3; DualSense, the game's own HID reads; started without EAC): recon, a new `swarm` module, native gyro works.**
  - Install: `client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe` plus `... - Retail.dll` (same code as an in-process server for solo/hosted games). Window title `... CLIENT 15.2.PATCH...`,
    **window class `Magnus`**. Data in zip `.pak`s under `client_pc\root\paks\client\default\` (`default_ssl.pak`: compiled SSL scripts; `default_other.pak`: `.cls`/`.sso`). The "Swarm" strings are the zombie-swarm ECS
    (`EcsComponentSwarm*`), not the engine's name. The game reads the DualSense itself (Sony pad library via HID.DLL, 64-byte reports), DirectInput mouse/keyboard, D3D12 through Streamline; Epic's overlay
    (`EOSOVH-Win64-Shipping.dll`) rewrites `LoadLibrary*`, `DirectInput8Create`, `GetCursorPos` imports after us. Ships `msvcp140.dll` 14.34 (the user renamed it).
  - **Why F10 did nothing:** the DllMain thread pumps only the splash screen's window, runs frames ~10 s, then stops for good while the `Magnus` window is pumped by another thread. Fix (core): `Engine::pump_window_class()`:
    the game thread is 0 until the first message for a top-level window of that class, then that thread (`dllmain.cpp adopt_game_thread`). Module `src/engines/swarm/`: detection by `..\..\paks\client\default\default_ssl.pak`,
    window class `Magnus`. The blind stick fallback turned nothing (the deflection goes into the game's HID reports but the camera didn't move).
  - **Camera (camtrace)**, from the editor's type descriptions (`client_pc\root\tools\meta_descs\meta_desc_ssl.ps`, shipped with the game: `GameCameraSystem.gameCamera` -> `GameCameraPackage`: `tpsController`
    (`TpsCameraController`), `cameraInputController`, `inputSenseFov*`, `rotationSpeed`; designer data, not runtime offsets). Every step of the first run was a copy (write-test undone): the render view block `exe+0x4e8ec40`
    (camera-to-world rows at `exe+0x4e8ec7c`, ~22 copies) written by `memcpy` from `exe+0x1d76e90` via the render-view setup `exe+0x1803710`; the render camera via `exe+0x1350ee0`
    (`world = exe+0x1dd4fe0(byte)`; `bp exe+0x18037a6 --exec`); set by `exe+0x22220a0` called by the camera component update `exe+0x1373d50` (`[rbx+0x70]` = the render camera); the event from the scene node's
    transform-changed dispatch `exe+0x1726170` (world matrix at node+0x98; node world computed in `exe+0x1726480`). A write breakpoint on the node's world matrix ended with the game gone. Second run: rig = camera node <- attachment
    link (vtable exe+0x3643390) <- **arm node** (local +0x58) <- link <- **root node** (character yaw only); write-tests on the root's world undone. A value search on the camera's heading, user turning between scans, left the
    **third-person camera controller** (vtable exe+0x362B450, one instance, +0x08 = the arm node, a `main_camera` string at +0x1C8): **`float pitch` at +0x130 (degrees, grows looking down) and `float yaw` at +0x134 (degrees,
    the view's heading atan2(x, z), Y up, (-180,180]; shrinks turning right)**. Write-tests kept and the view followed (yaw +20 twice, pitch +5). Findable every run by the constructor's signature (`C7 41 18 01 00 00 00 48 8D 05 ??
    ?? ?? ?? 33 DB 48 89 01 48 89 59 08 48 8B F9 C7 41 10 03 00 00 00`, disp at +10; unique) -> exe+0x362B450, and a heap scan for the vtable qword (3 s from Python; in-module: background scan, again 5 s after it goes
    away). `camtrace` gained `bp --exec` (`ba e1`) and `--cond`; execution breakpoints never took the game down.
  - Module writes `pitch -= gyro pitch`, `yaw = wrap180(yaw - gyro yaw)`. **User: the gyro turns the camera, both axes right.** Log: `camera controller at ... (1 found; 6.75 GB scanned in 469 ms)` 4 min after start; `gone` and found
    again 45 s later. **Bug (user): pitch limits not respected:** the game clamps the controller's pitch to the camera mode's limits and the core took the clamped writes for a camera it can't write (stick fallback, which
    turns nothing here). Limits are per camera mode in the presets (`CamBase.angleSettings`: `maxConstrainRotationY` = down, `minConstrainRotationY` = up; `ssl/camera/cam_modes/mgs/*.sso` in `default_other.pak`): idle 75 down /
    70 up, aim_minizoom 89/70, sprint 50/50, melee 75/70, last_stand 30/30, target_lock up 89. Fix: no `view_angles` for this engine (no stick fallback); `limit_turn`/`apply` clamp the pitch to -70..75 (cam_idle), widened to
    wherever the game's stick takes it (aiming to 89). **Bug (user): with GyroLib's panel open the d-pad also reached the game**: the exe imports `CreateFileA` as well as `CreateFileW` and nativepad only hooked W, so a DualSense
    handle opened through A was never followed; added `hk_create_file_a` (`nativepad: the game opens HID devices through CreateFileA too; followed`). **Both fixed in game** (user): the panel idles the reports, no stick fallback lines,
    the gyro stops at the pitch limits.
- 2026-10-27 **Build: Gyro4All 1.8.0** (user's version), `Gyro4All-v1.8.0.zip`. Since 1.7.3: the Swarm module (Space Marine 2), `Engine::pump_window_class`, nativepad follows `CreateFileA` HID opens, camtrace `bp --exec`/`--cond`;
  GAMES.md and ADVANCED.md (Space Marine 2).

## Status summary (2026-10-08)
Early status (superseded by the entries above and `docs/GAMES.md`): Pacific Drive done (on foot + car profile), Returnal generic view, Cronos: The New Dawn with a profile (raw double rotators in the pawn's `View`
component), Life is Strange Remastered with the probe-found profile (+OnGyro for the walking camera; first DX11 game and first pre-4.25 property system), Gears of War: E-Day with the generic view (needs inline hooks,
the System32 DXGI factory vtable and the heap scan for GUObjectArray; `PROXY=version.dll`).

## Ideas for later
- XInput translation extras (alpha with the default mapping): split touchpad (left half View, right half Menu, via `SDL_GetGamepadTouchpadFinger`), extra buttons (Edge paddles, mic, Capture) copying a standard button
  from the ini. Not keyboard keys: UE would switch to keyboard/mouse prompts.
- **Auto-probe with closed-loop check** (parked 2026-10-09): find the camera's rotation state without a manual probe by writing a small delta to each candidate and keeping the one that turns the camera (largely done since,
  as the live test in the UE probe).
- Heap scan as the universal GUObjectArray finder: works for any build, but the data-section scan stays first (instant in normal games); the heap scan remains the fallback.

- 2026-10-11 **GyroLib 1.3.1: upstream burst delivery replaces GyroUE's pacer.** GyroLib commit 9addfd9 answers our report (issue #1; also fixes issue #2: INI `gyro.acceleration=0` with stale fast sensitivities stays Off).
  SHA256SUMS verified; ABI 1, INI schema 0.2.0 unchanged. `MotionDelivery` (`src/detail/delivery.hpp`, per endpoint, inside `gl_update` after camera/cursor angles, before trackball/flick): a batch is a burst when
  consecutive samples are >= 2 ms apart in sensor time but arrive < 1 ms apart; three consecutive bursts separated by updates with no samples latch pacing until the source restarts. Each update's integrated angle is spread
  linearly over a window (peak-hold of the gap between batches, x0.98 per batch, 15-60 ms), starting at the next update; pending angle dropped on gates, calibration, view/target/profile/modifier changes and stalls > 100 ms.
  SDL source only (not Steam), no INI setting, flick stick and trackball not buffered. Synthetic replay of a Switch Pro BT stream (Luto trace): both pacers cut empty frames from 42-75% to 0.3-2.6% and speed CV from 1.0-1.9 to
  0.31-0.36 (GyroUE) / 0.38-0.44 (GyroLib), both adding ~25 ms. Risks: the latch never releases (a DualSense over BT whose reports bunch could get a permanent >= 15 ms window), Steam-provider motion isn't paced. GyroUE's
  Pacer, `Pacing`, `PacingMaxMs` removed (old keys ignored); `MotionTrace` stays; new status line `motion N samples/s in M arrivals/s | camera updates U/s, T% turned, P% of those without a new sample (GyroLib burst
  delivery)` (P must stay ~0 for a DualSense). **Luto test (user), Switch Pro over Bluetooth: works well.** Still to test: Switch Pro USB, DualSense USB and BT (P%), Steam Input on, a `MotionTrace=1` capture.

- 2026-10-13 **Jedi: Fallen Order (Steam, UE 4.21, `SwGame-Win64-Shipping.exe`, UE4SS also installed), GyroUE 0.8.1: engine not readable, then solved.** GUObjectArray found (exe+0x3875CE0, stride 0x18, ~26.8k objects) but no
  `FNamePool` (it exists from UE 4.23; 4.21/4.22 keep names in **`TNameEntryArray` (GNames)**: `Chunks[128]` of `FNameEntry*[16384]`, then `NumElements`, `NumChunks`; `FNameEntry` = `Index` (name index << 1 | wide), `HashNext`,
  characters at +0x10). Fix (`ue.cpp find_legacy_names`/`legacy_entry_string`): when the pool isn't found but the object array is, look in the exe's writable sections for GNames (pointer to the table, or the table);
  entries 0/1 must read `None` and `ByteProperty` at the same text offset (tried 0x10, 0x0C, 0x08, 0x14, 0x18), NumElements/NumChunks consistent (`ue: no FNamePool; GNames ...`). Layout: `UProperty objects, UE <= 4.24`
  SuperStruct=0x30 ChildProperties=0x38 Field.Class=0x10 Next=0x28 Name=0x18 ElementSize=0x34 Offset_Internal=0x44 sizeof(Property)=0x70; PC `BP_SwPlayerController_C` (ControlRotation +0x3C8), pawn `BP_Hero_C`, camera manager
  **`RsCameraManager`**. The gyro wrote ControlRotation and the camera didn't move. **Reflection of `RsCameraManager`:** `ActiveCameraMode` +0x2600, `ModeInputs` +0x2610 (ControllerInput +0x2704, ...), **`ModeOutputs`
  +0x2740** (Location, **Rotation +0x274C**, LocationOffsetCS (arm, ~-450 X), RotationOffsetWS +0x278C; POV = ModeOutputs.Rotation + this). A memory hunt of PC/pawn/camera manager (0x2000 bytes of the manager, which missed
  ModeOutputs) found only exact copies of the POV. **Write proof (`WriteProcessMemory`):** +30 yaw at ModeOutputs.Rotation: camera +30.00 and stayed, position moved 227 units (expected chord for a 450 arm: 233, so it orbits);
  +10 pitch +10.00. The PC+0x6D0 native struct: no effect.
  - **Dotted `Property=` and nested probe candidates:** `find_prop` resolves `Struct.Member` (any depth) through StructProperty types, offset = sum, so views, live test, `read_view_rotation` and `apply` take nested rotators and a profile can
    name one by reflection (user's choice over raw `Offsets=`, safer for other games). The live test no longer rejects nested properties; reflected candidates on the camera manager are a passive "camera output" only inside the engine's
    `PlayerCameraManager` part (`CameraCachePrivate.POV.Rotation` stays an output, `ModeOutputs.Rotation` doesn't). Profile `profiles/SwGame-Win64-Shipping.ini`: `Object=Camera`, `Property=ModeOutputs.Rotation` (keeps
    `DualSenseAsDualShock4=1`). **Probe test (user, DualSense BT, profile set aside, Probe=1, 3 captures): the probe verified `Camera.ModeOutputs.Rotation` live and wrote it as the game's profile** (POV.Rotation and ControlRotation
    rejected as overwritten, TransformComponent0 no effect). **Gyro test: works** (`camera: confirmed ... (ModeOutputs.Rotation, view 'Camera')`; off in menus/loads). Not checked: combat, cutscenes, spline camera modes.
  - **Probe UX:** a restart with Probe=1 still set silently kept the gyro paused (`applied yaw=0.0` all session). Now after a verified live result (`finish_tests`) `disable_probe()` writes `Probe=0` to GyroUE.ini (and to the game's
    profile if it sets Probe itself), logs `probe: Probe=0 written ...`, and the notice says the camera was found, to RESTART THE GAME, and that the probe is off.
- 2026-10-14 **Valor Mortis (Xbox/Game Pass GDK build, UE 5.7, `ValorMortisMSG-Win64-Shipping.exe`), GyroUE 0.8.1: engine not readable, then solved.** FNamePool found (exe+0xADF4ED0) but no GUObjectArray (data
  sections, XOR globals and three heap scans of 7.4 GB found nothing; one candidate at exe+0xAEC2F90 whose items read null). The exe can't be read from disk. Read-only look: the candidate is the real `ObjObjects` (Max
  0x210000, Num 70868, MaxChunks 0x21, NumChunks 2); **each 0x18-byte `FUObjectItem` holds the Object pointer at +0x8** (UE 5.7 puts a 64-bit field before it). Fix (`ue.cpp`): `object_array_stride` also tries the Object
  pointer at +8 (`g_item_object`; log `object at +0x8`). **Tested (user, DualSense, Probe=1): works**; layout unchanged (SuperStruct=0x40 ChildProperties=0x50 Next=0x18 Name=0x20 Offset_Internal=0x44); PC
  `BP_PlayerController_C`, pawn `BP_PlayerCharacter_C` (first person; menu pawn `BP_MenuPawn_C`); the probe verified `PC.ControlRotation` (generic view, no profile needed). Probe=0 written by itself. PlayStation prompts: the game's
  own "Controller platform" option, absent from the Game Pass build; not pursued.
  - **Valor Mortis crashes in the main menu (2 sessions, not resolved).** The game reports through Sentry (crashpad): envelopes in `%LOCALAPPDATA%\ValorMortisMSG\.sentry-native\external\` hold the minidump and are deleted once sent
    (copy them before restarting). Dump (UE 5.7.3, GyroUE 0.8.2): AV reading 0xFFFFFFFFFFFFFFFF at `exe+0x2EED0C0` on `RenderThread 0` (no GyroUE/GyroLib in that stack); DLSS frame generation on, RTSS hooks loaded, our DX12 panel hooks
    active. Both crashes <0.5 s after `gyro off: mouse cursor shown` in the main menu, both with NativeFlickStick=1. Not isolated (the proposed A/B tests were declined).
- 2026-10-14 **Build: GyroUE 0.8.2** (user's version), `GyroUE-v0.8.2.zip`. Since 0.8.1: UE 4.22-and-older GNames, UE 5.7 object array items, `Property=Struct.Member` by reflection and nested rotators tested live by the probe, the probe
  sets `Probe=0` after a verified camera, Jedi profile. Tested: Jedi (probe + gyro, DualSense BT), Valor Mortis (probe + gyro).
- 2026-10-14 **Bug (user): GyroLib's panel navigated with the d-pad also drove the game behind it.** Only the controller shown as XInput was neutral while the panel was open. Valor Mortis and Jedi read the DualSense themselves
  (Sony's library via `nativepad.cpp`), which passed through untouched; a real XInput pad did too. Fix: `host.cpp panel_has_pad` (true while the panel is open and, after it closes, until every SDL button is released and both
  triggers are up) passed to `xinput::set_neutral` (our slot neutral; real slots connected with zeroed `Gamepad`) and `nativepad::set_neutral`: every Sony report with a known layout and verified CRC is made idle (`idle_report`:
  sticks 0x80, triggers 0, buttons cleared with the hat at 8, touch "no finger"; motion, counters and battery kept; BT CRC rewritten; DualSense USB 0x01/BT 0x31, DualShock 4 0x01/0x11, basic BT 0x01, DualSense shown as DS4). DualSense
  handles are always followed (real DualShock 4 only with `NativeFlickStick=1`). Log `nativepad: the game's controller reports made idle (GyroLib's panel has the controller)` / `passed through again`. Not covered: Raw Input/DirectInput
  games (later `dinputpad`) and the press that opens the panel with the pad shortcut.
- 2026-10-15 **Ported from the GyroUE repository (commit 5bc258b): GyroLib long-press blocking and the camera recenter button** (builds; not tested in game then).
  - **Long-press blocking** (GyroLib 1.3.1 `ADVANCED_MOTION.md`): with `context.N.gyro.activation` Hold (3) or Hold to disable (5), a single `context.N.activation.button` and `activation.block_long_press=1`, `gl_filter_event` defers
    that button's press: release < 200 ms -> `GL_EMIT_TAP` (the host gives the game a short press), hold >= 200 ms -> blocked. Needs `GL_HOST_LONG_PRESS_BLOCKING`. `core/longpress.cpp` (game thread, after `gl_update`): every SDL
    button of the selected pad through `gl_filter_event`; result per button `hide` (the activation button is held back from its first report while blocking is on, because the game reads the pad on its own threads) and `tap` (80 ms
    press after `GL_EMIT_TAP`). Focus loss, the panel taking the pad or a controller change finish held buttons with `native_hold=1`. Applied in `xinput.cpp read_pad` (our slot; SDL ordinals before the Nintendo swap), in real XInput
    slots when the selected pad is itself an XInput pad (packet number offset by a change counter), and in `nativepad.cpp` reports of the selected Sony controller (button bytes from b+7 DualSense / b+4 DualShock 4; hat re-encoded;
    BT CRC rewritten) only after those bytes agree with SDL (>= 10 compared reads with a button down and >= 4x the misses). Log `long-press blocking available (...)` / `unavailable: ...`, `longpress: button N tapped/held`. Not
    covered: Raw Input/DirectInput games, Steam Input keyboard bindings.
  - **Camera recenter** (GyroLib `gl_set_recenter_step_callback`; `context.N.camera.recenter_button`, `recenter_duration_ms` 0..1000): the callback gets the fraction of the remaining pitch to remove; GyroLib drops requests
    unfocused/paused/cursor/menus. Core (`host.cpp`): registered in `start_gyro`, fractions combined `1-(1-a)(1-b)` and applied after the gyro delta, not counted by the follow check; support asked from the engine when the view changes
    (`Engine::recenter_support` Yes/NotYet/No; NotYet retried each frame; No withdraws it); applied through `Engine::recenter(fraction)`. Unreal: supported while the view's rotation can be read; applied as `apply(view, delta, 0)`
    with `delta = wrap180(RecenterPitch - pitch) * fraction`; per-view profile keys `RecenterPitch=` (default 0), `Recenter=0`. Unverified per profile: Pacific Drive car, Cronos, Jedi, Hogwarts, Life is Strange.
- 2026-10-15 **Ported from GyroUE (e75bd96, 84ab27b): sensitivity profiles ADS / ALT_ADS / OTHER, default view labelled "Exploration"** (not tested in game then). GyroLib's panel can't bind a button to a view, so the buttons live in
  `Gyro4All.ini` (profile overrides; legacy `[GyroUE]` read too): `<Key>ProfileButton=` and `<Key>ProfileMode=Hold|Toggle` for Key = Ads, AltAds, Other; all off by default (a profile needs both keys). Button names: Xbox or
  PlayStation (face buttons by position), d-pad, Back/Start, Touchpad, Misc, paddles, LT/RT (trigger > 8000). Engine-neutral in `core/host.cpp`: GyroLib views 1001 (ADS), 1002 (ALT_ADS), 1003 (OTHER) registered after the engine's views
  (`register_sensitivity_profiles`); disabled ones get the label "<label> - disabled"; parents: ADS and OTHER inherit view 1, ALT_ADS inherits ADS; an engine view with one of these Ids turns that profile off. Only in the default
  view (id 1): `frame()` passes the profile's id to `g_input->update`; other views (vehicles) keep their tab. Latest activated profile wins; Toggle turns off when gameplay is interrupted. No in-game notice for disabled profiles
  (GyroLib 1.3.1 has no API for the panel's selected tab). Unreal: the default view's label is "Exploration" (was "Camera"); a user profile with `Label=Camera` keeps that name.
- 2026-10-16 **Ported from GyroUE (b3dac74): case-preserving FName pools (`WITH_CASE_PRESERVING_NAME` builds)** (found on PAYDAY 3, Steam, UE 5.x: GUObjectArray found but no FNamePool). Each pool entry is `ComparisonId` (uint32), then a
  header with bIsWide in bit 0 and Len in bits 1..15, then the characters, 4-byte aligned (offset in block = (index & 0xFFFF) * 4; block 0: `None` header at +4, chars at +6; `ByteProperty` chars at +0x12). FName is 12 bytes, so
  UObject::Outer is at +0x28 (Name 0x18; Class 0x10, InternalIndex 0x0C unchanged). Reflection: SuperStruct 0x48, ChildProperties 0x58, FField Name 0x20, ElementSize 0x34, Offset_Internal 0x44. `ue.cpp find_name_pool` accepts that block 0 (header
  0x0008), sets `g_name_case_preserving` and `g_obj_outer = 0x28`; every Outer read uses it; log `ue: FNamePool.Blocks at ... (header len case-preserving entries, Outer +0x28)`. Tested in GyroUE 0.9.1 (PAYDAY 3, DualSense, generic
  view). PAYDAY 3 is always-online co-op, outside the offline-only ground rule (user's request in GyroUE); README/GAMES rows carry the online warning. Not tested with Gyro4All.
- 2026-10-16 **Ported from GyroUE (589c1b1): `Match=Camera` switching fixes and in-game camera learning** (378 checks; not tested in game with Gyro4All). The case (PAYDAY 3): the gyro stopped after some in-game actions; the follow check
  caught it (`camera: stopped following the gyro (ControlRotation, view 'Exploration'; ...) - target CH_Star_C, camera manager BP_PlayerCameraManager_C`); the probe found **`Camera.@SBZClampCameraRotationModifier` raw +0x68 (double rotator)**:
  during those actions this modifier turns the camera and `ControlRotation` becomes an output. The view target doesn't change, so `ViewTargetClass` can't tell the cameras apart. Tested in GyroUE (switching both ways, AutoProbe end to
  end, fast switching with carry-over, no freeze at pitch limits). Dead end there: "stopped following" after 2 windows gave a false AutoProbe start; back to 3.
  - Mapping: `core/camera_match.hpp` (header-only, engine-neutral): the choice between the base view and its `Match=Camera` views: fast path (3 writes in a row put back by the game, judged on yaw, ~30-50 ms), slow path (camera ignores
    >= 2 deg of yaw over >= 0.2 s), missing object (at once), carry-over of the lost gyro (max 45 yaw / 10 pitch), all ignored -> the view followed last, 2 s rest. `ue_profile.cpp` (new): `View`, `match_camera`, `load_views(path)`,
    `has_views`, `add_view` (appends `[View.Auto<id>]` with `Match=Camera`, `Parent=1`; writes `[Gyro4All] Views=`; ids skip the sensitivity ids), `same_rotation`. `ue_engine.cpp`: `base_view` + `active_view`; rotation read before and after
    the gyro write for the fast path; `after_frame` runs `camera_match_frame` and the follow check; profile hot reload (`check_profile_file`, every 1 s; views only). Follow check keeps running after its verdict, judges yaw only, logs
    `stopped following the gyro` after 3 ignored 2 s windows and `follows the gyro again`; a first "ignores it" verdict or a "stopped following" starts AutoProbe. `ue_probe.cpp`: `ProbeKey=` (default `Ctrl+F7`, an empty value = no key) starts
    the probe during play or cancels any probe; `AutoProbe=1` (max 3 per session, 30 s apart; keeps the gyro on until its live test, gives up after 45 s without a capture or when the camera follows again); a verified camera of a probe
    started during play, or of any probe in a game with a profile, is added with `add_view` and read again at once; only `Probe=1` in a game without a profile writes `[View.Default]`. Pitch wraps (UE5 0..360). Core: mouse look counts like
    the right stick (`host::mouse_moving`); `Engine::views_changed`; `EngineFrame::base_view_id`; transition lines (`focus: ...`, `gyro: GyroLib enables/suspends the gyro`, `motion data available/lost`, `motion source N`, ...);
    `util.hpp split` trims tabs. Tests: `tests/profile_tests.cpp`, target `gyro4all_tests`, run by `tools/test-gyro4all.sh`. `profiles/PAYDAY3-Win64-Shipping.ini`: default view + `[View.Clamp]` Id=2, `Match=Camera`,
    `Camera.@SBZClampCameraRotationModifier` +0x68, RotatorSize=24. The RE Engine module gets none of this.

## Dead ends

(none yet; dead ends are recorded in each entry)

## 2026-10-25 Dead Island 2: camera splits yaw and pitch

- Dead Island 2 (UE4, `DeadIsland-Win64-Shipping`, Epic), first person: with the default view the gyro turns pitch only; the game rewrites `PC.ControlRotation`'s yaw ("overwritten (output)"). The probe's Match=Camera view
  `Pawn.CollisionCylinder` raw `0x200,0x220` turned yaw only. New profile key `PitchView=<id>` (yaw-only view, its pitch goes into that view's rotation; the probe prefers rotations that turned both axes). In game the capsule view
  is NOT the stick's camera: it turns the body past the game's limits and shows the character model.
- camtrace (cdb hardware breakpoint on `PC.ControlRotation` yaw, PC+0x2C4; new `bp --skip base:size` ignores Gyro4All's own writes; the hit check now needs the marker on its own line): the game's write is
  `AController::SetControlRotation` (exe+0x369F370) called from the game's `APlayerController::UpdateRotation` override at exe+0x17C78F0, which reads `RotationInput` (PC+0x458) and `GetControlRotation()`, stores RotationInput at
  pawn+0x12EC/+0x12F8, then **sets the view's yaw to the pawn's RootComponent (capsule) world yaw**; the pawn's vtable +0xB88 (exe+0x1775040, FaceRotation(new, old)) turns the whole actor with `SetActorRotation(RootComponent quat * delta)`.
  The capsule's +0x200/+0x220 are USceneComponent's rotation caches, not its transform (ComponentToWorld quat at +0x230): a write to the cache turns the view's yaw but not the actor.
- **Dead ends (memory-only routes):** `CollisionCylinder.RelativeRotation` with `WorldRotation=1` turns the view but the camera sits at the edge of the character model (children are only moved by `UpdateComponentToWorld`) and
  CharacterMovement rewrites the transform while walking; ControlRotation rewritten from the capsule; `Property=RotationInput` written from the PeekMessageW pump is wiped (the player tick at exe+0x3A42620 sets `RotationInput =
  ZeroRotator` at its start (exe+0x3A426E4), the stick adds, `UpdateRotation` reads it, zeroed again at the end (exe+0x3A42DD5)). The yaw has to go through engine code.
- **Solution: `Property=RotationInput` views** (`ue_update_rotation.cpp`): the gyro is queued and added to `PC.RotationInput` inside the controller's `UpdateRotation`, hooked with MinHook; found by shape (the one PC vtable entry whose
  function reads two different components of `[rcx + RotationInput offset]` before any call/jump/return or change of rcx, using MinHook's hde64 length decoder; `ue_update_rotation_match.hpp`, unit-tested with hand-assembled bytes). No fixed
  addresses. Queued turns not taken within 250 ms are dropped. `RotationInput` isn't reflected in UE4: `update_rotation::rotation_input` = a reflected one, else `InputYawScale` (or UE5's `_DEPRECATED`) minus ControlRotation's rotator
  size (+0x458; InputYawScale +0x464 = 2.5, InputPitchScale +0x468 = 1.75). First shape matched 4 vtable entries (UpdateRotation [344] and DI2's AddPitch/Yaw/RollInput overrides [369]-[371] at exe+0x17A4D90/0x17A52C0/0x17A4DE0): hence
  the two-component rule. **Works in game** (user, DualSense): yaw and pitch like the stick, the body follows, walking keeps yaw.
- **Cinematics:** the gyro still turned the camera in a cinematic. An `IsLookInputIgnored()` gate (vtable[231], exe+0x3687CD0, via the reflected UFunction's native thunk, `first_virtual_call`) never fired there: reverted. Dumps scene vs
  gameplay: the only difference is `Pawn.CharacterMovement.MovementMode` (ByteProperty @0x1A4): 5 (MOVE_Flying) in the scene, 1 in play; `BP_Base_Player_C.IsInActiveCutscene` (@0x1A08) false in both. Built in (Unreal module `read_scene`):
  the scene counts as paused while the pawn's `IsInActiveCutscene` is true or `CharacterMovement.MovementMode` is 5 (logged `scene:`); risk: a game whose normal play flies the character loses the gyro there. **VERIFIED in game**
  (user, Epic version): the gyro stops in the scripted scene and works after it. Profile `profiles/DeadIsland-Win64-Shipping.ini` (`[View.Default] Property=RotationInput`). GAMES.md row.
- 2026-10-25 **Bug (user): Dead Island 2, with GyroLib's panel open the mouse pointer moved behind it and the d-pad drove the game.** No `nativepad:` lines; Epic's overlay (`EOSOVH-Win64-Shipping.dll`) owns the exe's LoadLibrary imports,
  so `hk_load_*` never saw the delay-load of `libScePad.dll` and its imports were never patched. Mouse: Slate polls `GetCursorPos` every frame in menus. Fix: `hooks.cpp` watches `libScePad.dll` loads (`watch_dll`, loader
  notification) and runs `patch_all` from a new thread; `overlay.cpp` hooks `GetCursorPos` in the game modules (import-table mode): while the panel captures the mouse it returns the position from when it took it. 674 checks; not
  tested then.
- 2026-10-26 **Dishonored: Definitive Edition (Epic, UE3; 1.7.3 dev build; DualSense): XInput translation and the gyro camera work (user, in game).**
  - **The game never polled the translated controller:** the exe imports `XINPUT1_3.dll` by ordinal 2/3 (`jmp [rip+slot]` thunk at exe+0xC7817C) and calls XInputGetState in two places: the joystick enumeration (exe+0x7891E0, XInputGetState on
    slots 0..3, each connected slot not yet listed gets an entry of type 3 in a 0x1F4-byte joystick array) and the per-frame poll (only listed entries). The enumeration runs at viewport creation (ctor exe+0x78BF40), from a DirectInput setup
    function (exe+0x78FE40) and from the deferred message handler's WM_DEVICECHANGE case (switch at exe+0x78F0D8). The game's first check came 1.5 s before Gyro4All showed the pad (2 s grace), and neither our WM_DEVICECHANGE nor one posted
    by hand re-ran it (**dead end: WM_DEVICECHANGE for this build**). Fix: `Engine::controllers_changed()` (core, game thread, right after the WM_DEVICECHANGE when the controller is shown or withdrawn); `Unreal3` implements it in
    `ue3_xinput.cpp`: the exe's XInputGetState import slot (by name, or ordinal 2) -> `FF 25` thunks / `FF 15` calls -> `E8` calls to the thunks -> the call followed within 0x30 bytes by `cmp r32,4` and a backward `jl`, in a function
    starting at most 0x100 bytes before it; called with a null `this` under SEH. Log `ue3: controller list: enumeration at Dishonored.exe+0x7891E0 (found from 2 calls of XInputGetState, 16 ms)`; slot 0 polled ~1300 times per 10 s.
  - **Reflection** (`ue3tool.py find` after letting it try 4-aligned UObject fields): GObjects exe+0x17F4630 (Index +0x8), UObject packed to 4 bytes: **Outer +0x3C, Name +0x44, Class +0x4C**; names are UE3's **GNames** (exe+0x17F4270,
    TArray<FNameEntry*>, text at entry +0x14, entries at any byte); UProperty Offset +0x88, ElementSize +0x68, ArrayDim +0x64; UBoolProperty mask +0xA4; SuperField +0x74. `Engine.GamePlayers` +0x67C, `Player.Actor` +0x64, `Actor.Rotation`
    +0x100, `Controller.Pawn` +0x2BC, `Actor.WorldInfo` +0x1A0. Scene: DishonoredEngine -> DisLocalPlayer -> DishonoredPlayerController, DishonoredPlayerPawn, DishonoredPlayerCamera. **No `WorldInfo.Pauser`**; the pause menu sets
    `WorldInfo.Paused` (+0x36C mask 0x100); `bIsMenuLevel` (+0x36C mask 0x20000) stays on in gameplay, so profile key `[Unreal3] MenuLevel=0` ignores it (`profiles/Dishonored.ini`). **The camera follows `DishonoredPlayerController.Rotation`**
    (write test with screenshots: pitch +20 up, yaw +30 right; in the scripted intro scene it is rewritten every frame). `Unreal3` generalised (no per-game offsets): the Class field found in 4-byte steps, Name/Outer the two 8-byte fields
    before it, names through BioWare chunks or GNames (text at +0x10/0x14/0x18/0x0C, entry pointers at any byte), SuperField and bool mask found, Pauser optional, paused also while `WorldInfo.Paused`. **User verdict: works**, gyro off in
    the pause menu. Open: the main menu has a controller and pawn, so the gyro is "on" there (writes undone, stick fallback "turns nothing"); a few moments in play had writes undone (stick fallback learnt axial, curve 2.25-2.5, yaw ~100-150).
    GAMES.md: Dishonored supported.
- 2026-10-26 **Bug (user): ME1 lost its camera hook after the Unreal3 generalisation.** `ue3: name table not found (... 16 sample names)`: 3 of 16 samples were named `<uninitialized>` and every sample had to be an identifier (which
  objects exist depends on when the search runs). Fix: the name table counts when at least 3/4 of the samples are identifiers (module and tool); the bool-mask search only needs distinct bits among bools sharing an offset (at least two
  must share one). **User: ME1 and Dishonored work** (ME1: `name chunks at MassEffect1.exe+0x16A2090 (text +0xc), property offset field +0x94, SuperField +0x60, bool mask +0xd0`, `WorldInfo.Pauser +0x644`, no `Paused`). ME2/ME3 not retested.
- 2026-10-27 **Crysis (2007, EA app, 1.2.1 build 6156, CryEngine 2, 64-bit): new `CryEngine2` module (`src/engines/cryEngine2/`); the gyro works (user: "funciona perfecto"; DualSense shown as XInput).** Install: `Bin64\Crysis64.exe`
  (x64 launcher, loads `CrySystem.dll`; the EA app starts `Bin64\Crysis.exe`, a 32-bit wrapper that runs `crysis64.exe`). PunkBuster (`Pb\`) multiplayer only. No ASLR (CryGame 0x39000000, CryAction 0x30500000, CrySystem 0x36500000,
  CryInput 0x33500000). `Crysis64.exe` imports neither winmm nor version, but `CrySystem.dll` imports both, so `Bin64\winmm.dll` loads with it on the main thread. Pump `PeekMessageA` (module prefix `cry`). `CryInput.dll` imports
  `xinput1_3` by ordinal 2/3/4 and `dinput8`. `Editor.exe` and `CrysisDedicatedServer.exe` are `is_launcher`.
  - **MSVC 2005 RTTI** (Complete Object Locator signature 0). Vtables in CryGame.dll: `CPlayer` +0x360A98 (186 slots), `CPlayerInput` +0x3614A8 (13: 1 PreUpdate +0x66530, 4 OnAction, 7 Reset, 8 DisableXI),
    `CPlayerMovementController` +0x3620E8 (slot 0 RequestMovement +0x793B0), `CGame` +0x3668C8; CryInput.dll `CXInputDevice` +0xEF78, `CMouse` +0xE838. `CPlayerInput`: +0x28 CPlayer*, +0x40 deltaMovement, **+0x64 mouse
    deltaRotation** (Ang3; PreUpdate multiplies by sensitivity, `CPlayer+0x978` and zoom), **+0x70 XInput deltaRotation**, +0x84 DisableXI. PreUpdate sums both into a `CMovementRequest` on its stack (flags, bit 0x40 = delta rotation
    at +0x60, Ang3 radians) and calls `[CPlayer+0x490]->RequestMovement(request)` (return address CryGame+0x66F29); the bit stays clear when `CPlayer+0x82D` is set.
  - Module: RequestMovement hooked inline (MinHook); calls from PreUpdate's return address get the gyro's pending turn added to the request's delta rotation (radians; **pitch +x up, yaw -z right**, confirmed in game), after the game's
    sensitivity and zoom. Gameplay view while PreUpdate requested within 100 ms (menus, pause, loading otherwise). Vtable slot, first bytes and the call before the return address are checked; another build falls to the blind stick
    fallback. Log `cryengine2: RequestMovement at CryGame.dll+0x793B0 (hooked)`, ~1400-1600 requests per 10 s with >98% carrying the gyro. No `view_angles`/recenter yet.
  - **Bug: the F10 panel never opened.** Crysis's renderers get their device entry points at run time (`CryRenderD3D10.dll` and CrySystem look up `D3D10CreateDevice`/`CreateDXGIFactory` with GetProcAddress; `CryRenderD3D9.dll`
    `Direct3DCreate9`); `loads_graphics_api_itself` doesn't apply (modular build, an API DLL is imported). Fix (core `overlay.cpp`): on the import-hook path the game modules' `GetProcAddress` is hooked; a lookup of one of the
    device/factory creation exports (dxgi, d3d11, d3d10, d3d10_1, d3d9 via `panel_host::runtime_export`) in the loaded DLL of that name returns the same hook the import tables get (`overlay: d3d10.dll!D3D10CreateDevice looked up at
    run time by ...: hooked`). 674 checks; not tested then.
- 2026-10-27 **Releases 1.8.1-1.8.3.** 1.8.1: new CryEngine 2 module (Crysis 2007, 64-bit, EA app 1.2.1 build 6156) and the panel's GetProcAddress hook for games that look up their D3D/DXGI creation functions at run time.
  1.8.2: Crysis Remastered and Kingdom Come: Deliverance in the CryEngine module (renamed from cryEngine2), vtable function pointers read without 8-byte alignment, the static panel follows the swapchain the game presents to (panel ABI 4
  `set_window`). 1.8.3 (2026-10-28): Crysis 2 Remastered (user: rename `MSVCP140.dll` in Bin64).
- 2026-10-27 **Crysis Remastered (`Bin64\CrysisRemastered.exe`, 2022-02-03): CryEngine module renamed `CryEngine2` -> `CryEngine` (`src/engines/cryEngine/`); the gyro works (user; DualSense shown as XInput).** Monolithic exe (no
  CrySystem/CryGame DLLs), base 0x140000000, no ASLR, not encrypted, **no RTTI**. Imports PeekMessageA, xinput9_1_0, dinput8, winmm, version, d3d10 (`D3D10CreateBlob` only), steam_api64. **CPlayerInput** found from the string "CPlayerInput::SetState
  called: should never happen" (exe+0x1C284F0) -> slot 5 of vtable **exe+0x1A56200** (1 **PreUpdate exe+0x39E4E0**, size 0xD8, same code as Crysis's). `exe+0x39F150` is AddDeltaRotation(request, Ang3): bit **0x40** of the request's 64-bit
  flags, delta rotation at **request+0x88**; the request goes to `[CPlayer+0x2A0]->vtable[1]` (RequestMovement), return address **exe+0x39EBA7**. Module: one `Build` per game; Remastered hooks PreUpdate (slot and first bytes checked), whose first calls
  read `CPlayerInput+0x28 -> CPlayer+0x2A0 -> vtable[1]` and hook it (`RequestMovement at CrysisRemastered.exe+0x162980`; ~1200 requests/10 s, >99% with the gyro). Detection: exe `crysisremastered.exe` and `..\Game\gamedata.pak`; prefix "". Senses as Crysis. **In
  vehicles the gyro is off** (PreUpdate stops requesting; the user is fine with it). The F10 panel works through the new GetProcAddress hook (`dxgi.dll!CreateDXGIFactory1` and `d3d11.dll!D3D11CreateDevice` looked up at run time).
- 2026-10-27 **Kingdom Come: Deliverance 1.9.7 (Epic): works (user; `version.dll`, DualSense shown as XInput).** `bin\Win64MasterMasterEpicPGO\KingdomCome.exe` (small launcher) loads `bin\Win64Shared\*` and **WHGame.dll** (engine and game in one,
  ASLR, MSVC RTTI ~5170 type descriptors); imports winmm, version, PeekMessageA/W, DINPUT8, xinput9_1_0; DX11; no anti-cheat. RTTI: `IMovementController` vtable WHGame+0x21C42E0; **`wh::entitymodule::C_ActorMovementController` vtable WHGame+0x21C4330,
  slot 1 RequestMovement WHGame+0x36CED4**; its request copy tests bit 0x40 and passes request+0x88 to **WHGame+0x21D158 = CMovementRequest::AddDeltaRotation**. The player's input update (WHGame+0x21D7A8, from C_Player's vtable slot 193 -> +0xA7C580) builds
  the request at rsp+0x70 (skipped when `[game+0xC8]->[+0xC0]->[+0x788]->vtable[2]` returns true) and sends it via the actor's movement controller, return address **WHGame+0x21DD6A**; a cold path (+0xB8D0A2 AddDeltaRotation) returns to WHGame+0xB8D0D9. Action
  names: `rotateyaw`/`rotatepitch`, `xi_rotateyaw`/`xi_rotatepitch`, the horse's `horse_xi_*`; C_PlayerInput OnAction WHGame+0x1FF950; combat lock-on cvars `wh_cs_PlayerInput*` may fight the gyro. Module: `kKcd` build, static RequestMovement (vtable slot
  and first bytes checked), both return addresses checked; detection exe `kingdomcome.exe` next to `WHGame.dll`; prefix "whgame". Bugs found: `mem::rdp` rejects pointers not aligned to 8 and RequestMovement is only 4-aligned (vtable reads now align 1);
  the first D3D11 swapchain is the launcher's splash window ("One time render"), the game's on the real window came 1 s later, and F10 requires the panel's window to be the foreground root: panel ABI 4 `set_window` (`panel_host::follow_window`, window subclass
  moved with `unsubclass`); then the panel still never got a Present because the game presents to the **first** swapchain it creates and the second (another window) never presents: when the static panel's target hasn't presented for 500 ms and the game
  presents to another D3D10/D3D11 swapchain with a window, the panel moves to it (`the game presents to ... not the panel's (... idle 4172 ms): the panel moves to it`). EOS overlay rewrites WHGame's imports after start.
- 2026-10-27 **Crysis 2 Remastered (`Bin64\Crysis2Remastered.exe`, 2022-08-04; data `GameCrysis2\`): works (release 1.8.3).** Detection by a Remastered table (exe, data pak relative to Bin64, Build; Crysis 2's `..\GameCrysis2\gamedata_chk0.pak`); a detected game
  without a Build falls to the stick fallback. Monolithic exe, no ASLR, no RTTI; ships `libScePad.dll` (not imported statically). CPlayerInput from the same SetState message (exe+0x1C30AB0, slot 5 of vtable **exe+0x1C30DD0**): 1 **PreUpdate exe+0x12F0D70**,
  size 0x420; **CPlayer* at +0x310** (per-build `input_player`; 0x28 elsewhere); PreUpdate builds the request at rbp-0x70, adds the delta rotation inline at **request+0x78** after testing bit 0x40, calls `[CPlayer+0xBB0]->vtable[1](request)`, return address
  **exe+0x12F1737**.
- 2026-10-28 **Release 1.9.0** (`Gyro4All-v1.9.0.zip`, 674 checks): new Glacier 2 module for HITMAN 3 (below); core `MotionTrace=<seconds>` (values over 1 = trace length). No profile for the game.
- 2026-10-28 **Hitman 3 (Glacier 2): new module `src/engines/glacier/`, camera turned directly, working.** Epic build, `HITMAN3.exe` 3.280.0.0 (Build ID 10545744); DualSense USB, Steam Input off; the F10 DX12 panel (through Streamline's `sl.interposer.dll`) works. `Retail\winmm.dll`
  loads (the exe imports `timeBeginPeriod`/`timeGetTime`); frame from `PeekMessageA` on the main thread; no anti-cheat (EOS SDK only; game has online modes: offline use only; ZHMModSDK uses `dinput8.dll`, no clash). The game reads the DualSense over HID
  through Sony's `libScePad.dll` (report 0x01; no motion on PC). Detection: `thumbs.dat` next to the exe and `..\Runtime\packagedefinition.txt`.
  - Globals (signatures from ZHMModSDK, https://github.com/OrfeasZ/ZHMModSDK, unique here): player registry exe+0x3130720, ZGameTimeManager exe+0x315D5C0 (`m_bPaused` +0x70, works: pause menu), CameraManager exe+0x3946CE0, InputManager exe+0x312B6E0,
    GetCurrentCamera's global exe+0x3194BA0. The registry's `TArray<SNetPlayerData>` +0x390 has end = 0: read the inline `m_aPlayerData[4]` at +0x50 (0xD0 each; Hitman ZEntityRef +0xA8); interfaces by name through the entity type (+0x20 list of {STypeID*,
    offset}): `ZHitman5` at ref+0x3A8; `m_rMainCamera` at ZHitman5+0x1348/+0x1350 (`ZHM5MainCamera`). Dead end: snapshots of ZHM5MainCamera and ZHitman5 turning a full circle: only matrices and quaternions.
  - camtrace (3 breakpoints): the camera's world matrix (ZHM5MainCamera+0x20) written by a generic set-transform (exe+0x3F182) from local quat (+0x50) and position (+0x60) at exe+0xF26D90; the local quaternion is written at exe+0xF26748 from the
    **third-person camera controller's update exe+0x724240**. The controller: vtable **exe+0x1E1BA10** (constructor exe+0x722945, unique), +0x10 the ZHM5MainCamera interface, +0x18/+0x20 the Hitman ref/ZHitman5, **yaw +0x2C, pitch +0x30** (float radians; copies
    +0x3E4/+0x3E0). Yaw grows turning right (kept in [0, 2pi)), pitch grows looking down; the game's pitch limits are live: +0x8B4/+0x8B8 (= the update's struct +0x5C4/+0x5C8): 1.222/-0.785 (70 down/45 up) on foot, +-1.396 aimed, interpolated during raise/lower.
    The controller is found with a heap scan for the vtable (+0x20 = the local ZHitman5) on its own thread (a pointer search missed it). The gyro stops at the live limits (widened by the game's own stick, at most 1.5).
  - **Aiming:** the same controller and angles; only the FOV changes (+0x200: 40 on foot, 25 aiming). Stick fallback switched off for Glacier (the module reports no `view_angles` while the controller is found; the blind fallback stays when it isn't).
  - **The aim freeze was the look-at blend, after chasing the message pump and the input delta.** (1) At every weapon raise/lower the game thread pumps no messages for 0.14-0.2 s while it keeps rendering, so the PeekMessageA frame didn't run
    (`MotionTrace=300`, new); a Present-driven frame thread was tried (and later removed). (2) The update's input delta `[ZHitman5+0xBD0]+0x50` (float yaw, pitch in degrees, scaled at exe+0x1F92698) was tried as the injection (`GlacierInputDelta`): on foot
    consumed, aimed cleared every frame without applying; judging consumed/dropped flapped 30-40 times a second: **dead end**. The right stick never goes through it (only the mouse's path); on foot the stick's turn rate is the controller's +0x2F4. (3) **The look-at blend**
    (exe+0x727E05..727EEC): while the byte at the update's struct +0x6C4 (controller +0x9B4) is set, each update pulls the pending angles toward a fixed world point (+0x6A0, controller +0x990) by the progress +0x6C8 over +0x570 (0.54 s) then clears it;
    started at a gyro aim-in, never at stick aim-ins. Fix: angles written directly (`GlacierInputDelta` default 0) and, while the gyro turns, the blend byte cleared (`look-at blend stopped for the gyro`). Aim in/out with the gyro works. A conditional
    hardware breakpoint on a value written every frame hung the GPU (`D3D12: Removing Device`) and killed the game: **don't repeat**. A build with the Present frame crashed twice (~1 min in, the game's own `int 3` at exe+0x11DFCEF, Gyro4All's frame
    thread idle in both dumps); with pending writes off no crash but the freeze remained. After the look-at fix the game behaves the same with `GlacierPresentFrame` 0 and 1, so the Present frame thread, its setting and the D3D12-wait were **removed**: the gyro frame
    runs on the PeekMessageA hook on the game thread. The F10 panel had been lost with the Present frame (the first D3D11 startup-window swapchain attached the static panel first; fixed by waiting for the D3D12 swapchain, then removed with the thread).
  - The stick fallback started now and then (9 times in minutes) and saved `StickFallbackLearned=radial,0.000,1.75,76.2,35.7,3` to `profiles\hitman3.ini` (later switched off for Glacier). No per-game profile (decided with the user).
- 2026-10-28 **Bug (user): ME LE 3 fell to the blind stick fallback (1.8.3, 1.9.0).** `ue3: engine not found (GObjects found, names found, property offset field missing)`. The accepted name table was a **decoy array** whose chunk 0 and chunk 1 slots hold the same
  pointer (an address inside the exe; the real chunk 1 is on the heap); chunk 1 names read from inside other entries come out as tails ("nlineEventSet"), which are still identifiers and passed the 3/4 rule of 2026-10-26, and `Matrix`/`Vector`/`Rotator` (class
  `ScriptStruct`, chunk 1) never matched. Real table `exe+0x17B33D0` (text at entry +0xC); ME3: GObjects `exe+0x1887E40` (153259 objects), Offset +0x94, mask +0xD0, SuperField +0x60, `GamePlayers` +0x498, `Player.Actor` +0x68, `Actor.Rotation` +0x114,
  `Controller.Pawn` +0x3BC, `Actor.WorldInfo` +0x198, `WorldInfo.Pauser` +0x670. Fix: reject a chunk-array candidate when two chunks the sampled names use are the same pointer or null (module `find_names` and `ue3tool.py`). Not verified in game then.
- 2026-10-29 **Hitman 3: small hitches turning with the gyro (stick smooth) at a 100 FPS cap: uneven sensor time per frame.** `MotionTrace=120` (1.9.0, DualSense USB, gyro sweeps at a wall): Gyro4All's frames regular (3795/4394 at 10.0 ms); ~485 samples/s, sensor intervals
  1.3-3.8 ms; SDL reads at `gl_update`, so every sample's arrival is the frame time and GyroLib's burst delivery never engages. The sensor time inside each 10 ms frame varies 8-12 ms and the turn per frame follows it (sd 0.19, correlation 0.93 with the
  frame's sensor span); the stick turns by frame time, so it stays smooth. **Implemented: sensor-time pacing in the core** (`src/core/sensor_pacer.hpp/.cpp`, `host.cpp`): `SensorPacing=1` (default) / `SensorPacingDelayUs=-1` (automatic), re-read on save. GyroLib's samples
  (observer) give sensor and arrival times; host time = sensor + the lowest arrival-minus-sensor offset of the last 2 s. Each update's GyroLib turn becomes a segment of a timeline; each frame gets the turn up to (frame - delay), the last rate carried forward past the
  last report (at most 20 ms after the fix below). Automatic delay = median lag of the last report behind the frame. Per axis the frame's turn is clamped to 0.5-1.5x of (rate at the target x frame time) with the sensor motion's sign (never a step back; the 0.5 floor
  stays: allowing zero left 10-16 frames stalled mid-sweep). A GyroLib turn in an update without new reports (its burst delivery, BT pads) flushes what is owed and leaves GyroLib's turn unpaced for 1 s. Gate closing, a 100 ms stall or a sensor clock restart drop what
  wasn't played back. Status line `sensor pacing: delay, frames paced, % extrapolated, frames left to GyroLib's burst delivery, S stalls`. Replay of the trace: jitter 0.238 -> 0.090 (automatic delay) / 0.094 (delay 0). **In game (DualSense USB, ~160 FPS, sweeps):** raw
  0.308-0.321 -> paced 0.082 (automatic) / 0.117 (delay 0); totals kept; no frame against GyroLib's sign; the automatic delay is ~7% smoother than 0 and follows the motion more closely, 0 is ~2 ms earlier. **Hard-stop test (~30 fast wrist turns of 300-1800 deg/s, each stopped and
  held):** two faults fixed: a 143 ms game stall (62 reports in a frame; the 1.5x clamp trickled 7.2 deg out and left a ~0.25 deg offset) -> a stall (frame or report gap over 100 ms) now sends what is owed plus the update's turn at once and restarts the timeline; and reports
  stopping for 12.6 ms mid-turn at ~950 deg/s -> extrapolation cap 10 -> 20 ms. After: paced minus GyroLib cumulative p50 0.25 -> 0.008 deg, 300 ms after a stop mean 0.14 -> 0.03 deg. Game stalls of 144-189 ms were the game's (Gyro4All's frame max 0.61 ms).
- 2026-10-29 **007 First Light (Steam, Glacier, `Retail\007FirstLight.exe`): the frame hook found, then the camera.**
  - **Frame hook.** 1.9.0: `engine: Glacier (detected)`, then no gyro frame, so GyroLib never starts. The exe imports `GetMessageW`/`DispatchMessageW` only (a pump thread of its own blocked in `GetMessageW`; the game loop runs on the main thread, as RE7). Unpacked, MSVC RTTI
    (`ZGameLoopManager` vtable exe+0x2D0B628, ctor exe+0x7AC3A0, static object exe+0x5CF8280). In this build it is a registry of named **game-loop events** (`Start`, `Stop`, `Paused`, `Resumed`, `GameplayStart`, `GameplayStop`, `IntroCutStart`, `LoadScreenGone`, `SavegameRestored`, ...; exe+0x7AD0C0
    register, +0x7AD320 unregister, +0x7ACFA0 raise). Main loop from the CRT entry: exe+0x2A8E3B0 -> WinMain 0x46430 -> ... -> 0x3E150 (the loop) -> **0x49C10 (one frame; false = quit)** -> 0x4A300 (the frame's work: frame limiter, `ZGameLoopManager` +0x18/+0x20 times, +0x98 loop state 3 = running).
    **Hook point, data only:** 0x49C10 (at exe+0x4A218) loads a 32-byte delegate from **`ZGameLoopManager` +0xB0** (q0 member fn, q1 low dword this adjustment, q2 static fn, q3 object) and calls the frame through it when set; live all zero. The module (`frame_source`, when the exe imports neither
    `PeekMessageA` nor `W`) finds the wrapper by its code (`kWrapperCode`, exe+0x4A218 -> exe+0x5CF8330), waits for `ZGameLoopManager`'s vtable at wrapper-0xB0 (up to 5 min), and sets only the delegate's static fn (+0x10, one locked 8-byte exchange from 0) to `loop_frame`, which runs the gyro frame and then the
    frame's own delegate; checked every second (put back if cleared, left alone if something else fills it). Log `glacier: the exe never calls PeekMessage: the gyro frame runs in the game loop`. **Tested:** wrapper set 187 ms after start, GyroLib starts on the first loop frame, F10 panel works (DX12), ~79 gyro frames/s vs ~157
    presents/s (frame generation on), frame avg ~110-130 us.
  - **Camera search (read-only searches, write tests, two one-hit cdb breakpoints, in-mod WriteWatch).** Nothing of HITMAN 3's camera carries over: RTTI shows "Knt" kinetic cameras and game camera entities (`ZSphericalOrbitGameCameraEntity` (69 live), `ZKntHumanoidCamera`, `ZCylindricalOrbitGameCameraEntity`,
    `ZStateDrivenGameCameraEntity`, `ZPlayerCameraOrientationLimiter`); the first sampled are configuration, not the live view. Glacier is **Z up** (camtrace `vectors` assumes Y up: heading atan2(y, x), pitch asin(z) in the scratch variant). The view has 48 copies (10 in static data, e.g. exe+0x5E7B240); yaw copies
    (-(heading) - 90 deg) in `ZAudioManagerWwise` (exe+0x4A234C0) are written each frame from a `ZKntAudioListener`'s transform: copies. `ZCameraManagerMain` static at exe+0x5D1B200 (vtable exe+0x2D897E0; vfunc +0x38 returns the active camera entity). The camera and player live in the **Gameplay World ("Gpw")**, a
    data-oriented simulation fed through input queues (`Gameplay::TInputPool<SGpwInput_...>`: `PlayerCameraSetSphericalCoordinates`, `SetPlayerCameraWorldHeading`, `AddPlayerCameraOffsetInput`, `BlockPlayerCameraInput`) in a **2.7 GB private region** (camtrace skips regions over 2 GB; two 7.4 GB snapshots ran it out of memory). A yaw/pitch pair in
    radians at `<component> + 0x10C/+0x110` is written every frame from the camera entity's quaternion: a copy.
  - **The right stick followed from the pad to the camera:** `scePadReadState` in `ZDeviceGamepad_DualSense` vfunc 9 (exe+0x993C60; right stick at device+0x144/+0x148, device in the 2.7 GB region); in the Gpw (`[exe+0x64528E0]+8` is a table of component arrays {base, stride}; the player is entity 0) a per-frame input record is copied
    into the **player input component** (+0x558/+0x560; +0/+4 right stick, +0x20/+0x24 left). **Dead end that took the character, not the camera:** the "look input pipeline" exe+0x17CBE20 reads +0x20/+0x24, so it is the **move** pipeline; the first injection (a rate added to the control component +0x1210 +0/+4/+8) hooked the character's locomotion (the gyro jittered
    the movement). The "derived view" yaw (W1 +0x88 +0x10C/+0x110) is atan2 of a target position minus the character's position (a look-at/aim heading), not the camera's state. **Dead end that crashed the game:** killing an attached cdb leaves a hardware breakpoint in the game threads' debug registers (007 pauses when it loses focus, so runs started in
    this app's window never hit); camtrace `bp` got `--timeout` (breaks in with DebugBreakProcess, `bc *`, detach). Never wrap `bp` in `timeout`. In-process traces replaced cdb (core `WriteWatch`; `glacier_look.cpp` provides the name `input`): `r:input` (+0) was only written by the copier exe+0x1914931 and read by the idle check exe+0x1A39681 (the camera doesn't read the stick from this
    component); the queued record only by exe+0x14FD4D8; the stick reaches the camera by another path. Steam updated the game at 13:28 on 2026-10-29: code moved ~+0xFE0 (Gameplay World global to exe+0x6452CA0); code patterns found everything again.
  - **The camera's orientation is a quaternion** (paused-view comparisons: every unit float4/float3 and double in the big RW regions kept when it rotated by the view's angle; no persistent float angle exists). Write tests while paused are not conclusive (pausing may re-sync the camera); live spin tests (a few degrees every 30 ms for 3 s) on the copies had no effect (rewritten every frame).
    **Following the writers back with in-mod `WriteWatch=<addr>`:** derived view (exe+0x17CDE80) <- per-entity record table `[exe+0x6452BE8]`+0xB30 (the Gpw's input queue) <- a queued record (exe+0xC23507, pushed by exe+0x18A6B90 from `[[exe+0x3E9AE20]+0x10]`) <- the **active camera**'s transform (exe+0x540539; handle at `[exe+0x3E9ADE8]+4`) <- the camera entity's transform
    {quat, pos, prev quat, prev pos} in `[[exe+0x3E9ADA0]+8]` +0x30/+0x38 (exe+0x4FE5B8, dirty entities only; quaternion from exe+0x538560 (a look-at when bitset +0x720 is set) else exe+0x564DD0 from component +0x1E8 = {pivot, offset, shoulder offsets, up}) <- the offset (exe+0x545687 clears it, exe+0x52E0A5 fills it) <- **the orbit update** (loop at exe+0x52DEB0).
  - **The orbit camera's state (persistent, integrated by the game):** camera world `[exe+0x3E9ADA0]` (+8 component table), per camera entity: **+0x500 (stride +0x508): radius, yaw, pitch (radians), radius/yaw/pitch rates (+0xC/+0x10/+0x14, set by the right stick), yaw/pitch offsets (+0x18/+0x1C)**; each frame `angle += dt * rate`, clamped by **+0x558 (stride +0x560): radius, yaw min/max,
    pitch min/max** (exe+0x51D0F0), wrapped to +-pi, then turned into the offset around the pivot (exe+0x5640A0). Live: radius 2.0, yaw 141.35, pitch -4.15 (up negative), limits yaw +-180, pitch -60..75. Spin test on the yaw (+0x504): 62 writes of 2 deg turned the view +122 deg and the game kept it.
  - **Implemented, tested in game (DualSense): works.** `glacier_look.cpp`: code patterns find the camera world (the orbit update loop, which also pins the +0x500/+0x508/+0x558/+0x560 offsets), the active camera global, the Gameplay World and the player update (its hook only tells when gameplay runs). Each gyro frame (game thread, before the frame's update) adds the turn to the active orbit camera's yaw
    (`kYawSense +1`) and pitch (`kPitchSense -1`), pitch clamped to the camera's limits, yaw wrapped; no turn when the active camera has no orbit state. Log `glacier: orbit camera <handle> (state ..., limits pitch ..)` once per camera (16 kept). In 7 minutes it followed four orbit cameras (13020057 limits -60..75, 13020086, 1302006F, 13020067 limits -75..75). No recenter.
  - **Indoors and the pause menu:** the player's locomotion update stops indoors, so the gate is an inline hook on the orbit update (exe+0x52DDE0, the function around the loop, found from the unwind table with `RtlLookupFunctionEntry`, chained fragments followed); the orbit update keeps running behind the pause menu, so also `ZGameLoopManager` (wrapper - 0xB0) +0x98: 3 = running, 2 = pause menu (ZGameTimeManager +0x90 = 1 when paused would also do).
  - **Look-at cameras (L2).** Some orbit cameras look at a point (bitset `[[world]+0x38]+0x720`, point at component +0x1A50 stride +0x1A58, exe+0x538560): the **aim camera (1302006B, limits -80..80) ignores its orbit angles and follows the point**; the normal look-at cameras (13020067 with a weapon out) keep the point but follow their angles. The gyro turns both: the point about the camera's position
    (heading -= yaw, elevation += pitch within limits) and the angles. **Aim freezes, three causes:** (1) when the aim ray hits nothing the point is ~1e8 m along the view: the 1e4 m sanity limit skipped it (limit now 1e12 m); (2) ruled out: the game moving the written values after the switch, or a different rendered camera; (3) **L2's zoom (0.4-0.5 s, shorter with a weapon) runs on a transition camera without an orbit state**
    (entity 0x11E with a new generation each time, e.g. 1302011E, 1303011E, between 13020067 and 1302006B both ways); the orbit update integrates every orbit camera in its list (argument +8/+0x10: begin/end of 16-bit entities), active or not. Fix: **while the active camera has no orbit state, the gyro turns every orbit camera in that list** (points included); the game re-aligns 13020067 to the aim camera's view when it switches
    back. Tested (user): the gyro turns the view during the zoom with and without a weapon (with L2 activation and always-on; transition entity 0x105 in that game, so it is told by having no orbit state, not by its index). Not checked: cover cameras (13020061, 1302008E, 1302009E) after a cut that turned them, cutscenes.
  - **Log cleanup:** removed the 2 s calibration line (~1800 lines/hour); the camera line once per camera; the transition line (`glacier: transition camera <handle> for N ms: N gyro frames (N turned cameras), asked yaw .. pitch ..`) stays for the first 12; the core's status block (4 lines) now every 30 s instead of 10 s (`kStatusMs` in `host.cpp`).
- 2026-10-29 **007 First Light: a Switch Pro controller (DirectInput, no Steam Input) shown as XInput but never read; fixed with an engine-neutral WMI answer, user-verified.** Log: `shown as XInput controller 0`, then `game polls per slot 0/0/0/0`.
  The input manager's DirectInput device callback exe+0x994820 (guidProduct at +0x14) first matches the Sony product GUIDs, then calls exe+0x994F40 (rcx ?, rdx = &guidProduct, BOOL): Microsoft's WMI `IsXInputDevice` sample; true -> an XInput device (0x1B0 bytes,
  `ZDeviceGamepad_XInput_Windows`) whose slot is the first from the manager's next-slot counter (+0x48) up to 3 where `XInputGetCapabilities(slot, 0)` succeeds with SubType 1, asked once (none: slot -1, never polled); update exe+0x99BF90 calls XInputGetState([this+0x188]).
  Same scheme as Battlefield 4.
  - Engine hook first (`glacier_xinput.cpp`): from the game's first DirectInput8Create, functions that `lea` the wide "IG_" string whose callers test the result (exactly one: exe+0x994F40), hooked with MinHook, answers 1 for Nintendo controllers when the translation is on; core `xinput.cpp
    listed_as_xinput(vendor, product, wait)`: Glacier doesn't wait, so a listed controller not shown yet reads as a connected gamepad in the first free slot until the frame decides (`g_listed_pending`); BF4 keeps its 1 s wait.
  - **Engine-neutral fix on top** (`src/core/xinput_wmi.cpp`): the check's WMI query is answered differently. `CoCreateInstance` (game modules' imports; combase.dll's export inline on protected exes): when the game creates CLSID_WbemLocator, an in-process class object of our own (CLSID_WbemClassObject) gives
    fastprox.dll's `CWbemObject::Get` (IWbemClassObject slot 4), hooked inline with MinHook (all 235 Win32_PNPEntity instances share that Get). A `DeviceID` starting `HID\` without `IG_`, whose VID/PID (USB `VID_xxxx`, Bluetooth `_VID&0002xxxx`) is a present HID game controller (SetupAPI + hid.dll: usage page 1, usage 4/5/8; Sony skipped
    with XInput=1 when the game has Sony's pad library) reads as `HID\VID_xxxx&PID_xxxx&IG_00\<instance>` and `listed_as_xinput(.., false)` is called; calls from Gyro4All, gyrolib.dll and SDL3.dll pass untouched; the controller list is re-read when older than 2 s. Covers any DirectInput pad (8BitDo in DInput mode, generic).
    `VtableHook` moved to `src/core/vtable_hook.hpp`. **Dead end (crashed 007 at start, stack overflow):** the first version patched WMI vtable slots (IWbemLocator::ConnectServer 3, IWbemServices::CreateInstanceEnum 18 / ExecQuery 20, IEnumWbemClassObject::Next 4) in place, and the Steam overlay hooks the same ConnectServer and
    CreateInstanceEnum, so the two hooks called each other. **Verified in game** (inline `Get` version; Steam Input off): `the game creates a WbemLocator` at 10 s, `IWbemClassObject::Get hooked` 70 ms later, `057E:2009's DeviceID: shown as an XInput one` 5 s before the game's first DirectInput8Create, `the game polls XInputGetState`,
    300-1500 polls/10 s on slot 0; no crash with the Steam overlay; the DualSense also works (game's own support, 64-byte USB reports). Not covered: other forms of the check (IG_ in Raw Input/SetupAPI names) and controllers DirectInput doesn't list. Not tested: BF4 with this build, 8BitDo/generic pad, BT Pro controller.
- 2026-10-29 **Control (Steam 870780, Ultimate Edition, March 2025 build): Northlight engine module (`src/engines/northlight/`, plan in `games/Control/`); the gyro works (user: "feels right").**
  - **Recon.** Northlight (Remedy), native x64, no packer, no anti-cheat. `Control.exe` (90 KB, .NET) is the launcher for `Control_DX12.exe` (timestamp 0x67e06bf2) or `Control_DX11.exe` (0x67e06e49). Modular: engine in `<module>_rmdwin10_f.dll` (DX12) / `_rmdwin7_f.dll` (DX11): `rl_`, `app_`, `input_`, `coregame_`, ... with **decorated C++ exports** and RTTI. Camera
    classes: `CameraComponentState@coregame` (exports `getRotation`, `getMatrix`, `getInterpolatedTransform`, `getView` (`PerspectiveView`), `getInterpolatedFOVInRad`), `PlayerCameraComponent(State)`, `TailCameraComponent`, `GamePlayerCameraMovementComponent(State)`, `FPSCameraComponent`, `FreeCameraComponent`, `PhotoModeCameraComponent`,
    `CameraManComponent`. `PeekMessageW/GetMessageW/GetRawInputData` imported by `app_rmdwin10_f.dll`; `rl_rmdwin10_f.dll` wraps XInput (`rmdXInputGetState`) and loads it at run time. No libScePad, no HID. `winmm.dll` imported by `bink2w64.dll`, `navigation_rmdwin10_f.dll`, `wtf.dll`. Module: `detect` = `coregame_rmdwin10_f.dll`/`_rmdwin7_f.dll` next to the exe; `is_launcher` =
    `Control.exe`; window class `AppWindowClass`.
  - **Frame:** the DllMain thread never pumps; the game window belongs to another thread (PeekMessageW on it ~580 calls/s from `app_rmdwin10_f.dll+0x548F`): `pump_window_class()` = `AppWindowClass`.
  - **XInput:** inline hooks on all XInput exports were MH_OK but 0 game polls: **the Steam overlay hooks `xinput1_4` in front of ours** (`xinput1_4!XInputGetState/SetState` jump to `gameoverlayrenderer64.dll+0xd9c00/+0xd9d90`, no call-through even with Steam Input off), and `rl_rmdwin10_f.dll` tries xinput1_4 first (GetProcAddress results at `rl_rmdwin10_f.dll+0x3a28a0/+0x3a2898`;
    `snd_rmdwin10_f.dll` has xinput9_1_0 GetCapabilities/SetState). Fix: `hooks::redirect_slots` searches the non-executable sections of every game module, not only the exe; Northlight's Control prefix `rl_rmdwin` (the inline PeekMessageW hook catches app_'s pump). Log `hooks: xinput1_4.dll!XInputGetState slot at rl_rmdwin10_f.dll+0x3A28A0 (an import table of the game's
    own) now points to Gyro4All`. **User-verified:** 650-740 polls/s answered with the translated controller; sticks and buttons work. Cursor gate: arrow cursor in menus (`flags 1, cursor 0x10003`), `flags 0, cursor 0` in gameplay.
  - **Camera from exports and RTTI (read-only):** `CameraComponentState::getView` = `this+0xC0`; vtables (2025 DX12 build): `CameraComponentState` coregame+0x5d6988; in the exe `PlayerCameraComponentState` +0xe45100, `TailCameraComponentState` +0xe56ad0, **`GamePlayerCameraMovementComponentState` +0xe2b068**, `FPSCameraComponentState` +0xe1db50, `FreeCameraComponentState` +0xe1e158,
    `PhotoModeCameraComponentState` +0xde54b8, `CameraManComponentState` +0xdfe7c8. In game: 59 `CameraComponentState`, one of each special camera, two `GamePlayerCameraMovementComponentState`. World +Y up; camera looks along local +Z, no roll; every camera state's `+0x40` is the view quaternion, `+0x50` the position. `GPCMovement+0x40` (copies +0x60, +0xB8) is the unit look direction;
    `+0x2E0/+0x540/+0x820` a world point ~120 units along it. No yaw/pitch angles. `write-test +0x40`: undone (copy). `camtrace bp` (one-hit, cdb `-pd`, user saved first): `+0x40` written at `exe+0x3e4165` in the component's per-frame update: `+0x40` = normalize(`+0xB8`), stored only if changed; the stick input scaled by `[r10+1E0h]` goes to `exe+0x3e4630(this, &input, xmm2, xmm3, dt)` then
    `exe+0x3e4aa0`, `exe+0x3e4d20` (the turn of `+0xB8`). **`write-test +0xB8 --yaw 20`: kept** (+0x40 and the player camera's quaternion followed; the user saw the view jump 20 deg). **The source: rotate `+0xB8` every frame** (yaw about +Y, pitch about the camera's right axis).
  - **Module:** heap scan for the vtable (4 objects: two on a thread stack, the live one, and an unused second heap instance whose `+0x40` isn't its `+0xB8`); the live one is picked once gameplay begins (~45 s in). The per-frame check only verifies the vtable and the look vector's length (the scan's full test fails for a frame after a gyro write or while turning fast and dropped the component 8 times in 30 s); rescan every 1 s (was 5 s) while there is
    no component (a respawn replaces the camera entity; the new one picked ~1 s after it is filled).
  - **Smoothness (job system):** the camera update (`exe+0x3e3be0`, via `r::experimental::JobSystem::executeNextJob` -> `r::FlowUpdateGraph::update` -> exe+0x383d96 -> exe+0x517853) runs on a **worker thread**, not the pump thread that writes the gyro (~2 per frame; 248 camera updates/s at ~123 presents/s). Direct writes: 0-3 gyro steps per update and writes between the update's read and write lost (step jitter
    0.436 deg median with 274/1079 steps under half; stick 0.124-0.152 even). Fix in stages: (1) hook the update (found by its unique `+0xB8` -> `+0x40` copy code `48 8D 57 40 0F 28 C4 0F C2 02 00 0F 50 C0 83 F8 0F 74 ?? 0F 11 22` at exe+0x3e4152, and its unwind entry; one argument rcx = the component; `apply` queues the turn, the hook applies it right before the game's own update; a turn untaken for 100 ms is dropped): "much smoother, similar to the stick" but
    uneven; (2) running totals with QueryPerformanceCounter times, the hook reads the totals at its own time; (3) the hook reads at a frame clock of its own (advanced by the average update interval EMA 0.1, pulled 10% per update toward real time minus 6 ms, never backwards, reset after 100 ms without updates or 50 ms off), totals interpolated between the gyro frames: step variation 0.038 (stick 0.024; previous 0.204). Costs ~6 ms latency.
    **Bug: the "uncontrolled" gyro after the hook was the core stick fallback:** with the hook `apply` only queues the turn so `view_angles` right after showed the camera unmoved, taken as undone writes (fallback every 1-3 s, saved wrong `StickFallbackLearned=` lines into `profiles\Control_DX12.ini`, removed with the user's OK). Fix: `view_angles` returns false while the update is hooked (the hook's writes are part of the game's own update). **User: gyro aiming "feels right"** (DualSense Bluetooth, Steam Input off, activation on L2/L1 press). Not covered: photo mode, free camera, cutscenes (only the mouse cursor gates), DX11 exe, USB.
- 2026-10-29 **CONTROL Resonant (Steam 3669870, Northlight): recon, detected by the Northlight module** (not run with Gyro4All). One exe `CONTROLResonant.exe` (99 MB, timestamp 0x6ac4318a), DX12 only, unpacked, no Denuvo/anti-cheat, single player (`nakama-sdk.dll`, `steam_api64.dll`). Data `data_pack2\generic|pc\*.rmdtoc/*.rmdblob`. Imports `PeekMessageW/GetMessageW` (prefix ""), `WINMM.dll`, `VERSION.dll`,
  `XINPUT1_4.dll` (XInputGetState), `HID.DLL`, `SETUPAPI.dll`; strings `libScePad_static` (Sony's pad library linked in), `GameInput`, Wwise 2024.1. Camera classes RTTI: `PlayerCameraComponent`, `TailCameraComponent`, `FPSCameraComponent`, `FreeCameraComponent`, `PhotoModeCameraComponent`, `CameraManComponent`, `CameraInputComponent`, `CameraLockOnComponent`, `CameraSetComponent`, ... (ECS, `heron::*`).
  **The game has a gyro system:** `coregame::gyro_input::gravity_vector::system(...)`, global `sg_gyroInput` (only a gravity vector named); check the options menu: if it offers gyro aiming, Gyro4All must stay off or replace it. The user's folder has ReShade (RenoDX addons) and SCSKiller (`d3d12.dll`). Detection `data_pack2\pc\*.rmdtoc`; `is_launcher` only `Control.exe` in a Control folder.
- 2026-10-29 **Ghostrunner: a player's DualSense buttons didn't reach the game; not reproduced.** Retest on Epic (UE 4.26, 1.9.0, DualSense Bluetooth, no Steam Input): no Sony library and no HID native sign; the game polls `XINPUT1_3!XInputGetState` (re-hooked after `EOSOVH` rewrote it); Gyro4All shows the DualSense as XInput 2.2 s after it starts polling; buttons and sticks work. Open: a build with a Sony library that never opens, reads or closes the DualSense would keep auto mode on "native support" forever
  (`sony_sign_holds` always true with a Sony library; `dualsense_closed_unread` needs an open and a close).
- 2026-10-29 **Bug (user): DOOM (2016) Vulkan on AMD, the F10 / pad-shortcut panel invisible in game but capturing input; in the front end shown with its top cut off** (1.8.3, `DOOMx64vk.exe`, 3200x1800 on a 3840x2160 desktop, AMD GPU; not reproduced on NVIDIA 3840x2160). One swapchain/window all session, no Vulkan error. Suspected cause (unconfirmed): gyro4all.dll's Vulkan renderer took the queue/family of the first present and submitted every later draw there; a later present from another queue (DOOM's async compute on
  AMD) means another thread's queue used without external synchronisation and drawing into an image owned by another family with no ownership transfer, undefined on AMD. Fix (`src/panel/panel_vk.cpp present`): the panel submits on the queue being presented; a new family rebuilds the frames (pools per family) and re-uploads ImGui's textures; a family without graphics skips the panel and, with no present seen, the panel closes after 250 ms instead of capturing input
  unseen. Logs `Vulkan: the game presents from queue ... (family N) now, not ...`, `the panel's last submissions not done after 100 ms`. Resolved in the AMD entries below. Also seen: `motion source 0 ... the controller was reopened` every few seconds.

## 2026-10-30 Alan Wake 2 (Epic): no gyro frame, panel never attached; camera found; steps fixed

- Detected as Northlight by `data_pack2\pc\*.rmdtoc`. One PeekMessageW only, on the window's thread (AppWindowClass), from `AlanWake2.exe+0x1859487`; the pump at `+0x1859481` is one PeekMessageW followed by a blocking GetMessageW/TranslateMessage/DispatchMessageW loop (CONTROL Resonant's exe has the same shape). Fix: the Northlight module runs the gyro frame on its own thread, woken by each Present (`overlay::on_present`, 8 ms timer without one), for every
  Northlight game but Control (`frame_source`, as Frostbite); `window_class()` is `AppWindowClass` for all. Core: the game makes a D3D11 swapchain (`dxgi.dll`) ~2.4 s before its D3D12 one and never presents to it; `overlay::attach` now waits while the D3D10/D3D11 target hasn't presented since its capture (`panel_host::target_has_presented`) or has been idle 500 ms.
  The game reads the DualSense itself through HID (Sony's library, report 0x01). Verified (Epic, DualSense): `northlight: gyro frame thread`, `DX12 panel ready`; gyro as right stick (blind fallback) on in play, off with the cursor. The user had to rename the game's own `msvcp140.dll`/`msvcp140_2.dll` (14.34.31938).
- **Recon (static):** no `GamePlayerCameraMovementComponentState` and no match for Control's update pattern; `content::GamePlayerCameraMovementComponent` is only content (tuning), reached through `bigfish::component::CameraMovementContentWrapper`. The gameplay camera is a real ECS (`ecs::World`, `DefinitionComponentEntry<T, access, ...>`, access 2 = read, 3 = write), components plain structs (no RTTI). `bigfish::tailcamera::update` reads PlayerInput etc. and writes
  `bigfish::component::TailCameraState` and `coregame::component::CameraView` (the over-the-shoulder camera's own state); `bigfish::cameramixer::*` write mixer outputs (writes likely undone); `FreeCameraState*` for photo mode/free camera. Exe plain sections, no Themida/Denuvo/anti-cheat strings.
- **Source (camtrace, Epic build):** `snap`/`vectors` (90 deg stick turn): 49 turned copies (view matrices near `exe+0x3c15d80`, forward row `exe+0x3c0d248`; undone). `find-vec` before/after a second turn: 1493/1167 copies; write tests (`--yaw 20`): one heap copy kept the write **and every copy read after it was 20 deg further: the source** (a sparse region with entity-id-like ints: an ECS component column). `bp`: written every frame by `exe+0x191a958` in
  **`exe+0x191a4b0`** (`.pdata` 0x191a4b0-0x191a99e), called from `exe+0x1919499`: with rsi = the 6th argument, a float 5th argument (dt) and 7th/8th pointers, it reads the unit direction at `rsi+0x00` (y up), converts to two angles (`exe+0x1f32980`), adds the stick's rates times dt (`rsi+0x90` yaw, `rsi+0x98` pitch) plus a timed turn driven by `rsi+0xdc` (quick turn), wraps yaw into `rsi+0x8c`, clamps pitch (two scaled globals)
  into `rsi+0x94`, rebuilds the direction (`exe+0x3a7230`) and stores it at `rsi+0x00`. So the direction is the state the game turns from each frame (a turn written before this function runs is kept, with the game's own pitch clamp), same convention as Control's `+0xB8`.
- **Implemented:** `aw2_camera.cpp` hooks the update (signature `48 8B D9 48 8B 75 6F C5 F8 10 06 C5 FB 11 44 24 40`, once in the exe, + unwind-table function) for every Northlight game but Control; the first camera state it runs on is turned; another takes over once that one hasn't run for 100 ms; gameplay while the update ran in the last 100 ms (and no cursor). Control's queue, frame clock, look-vector turn and code search moved to `look_queue.cpp` (`LookQueue`), shared.
  **Alan Wake 2's update runs on a fixed 30 Hz step** (dt 33.3 ms at p10/p50/p90) while rendering ~68 fps (updates on every 2nd or 3rd frame); occasional steps (3-5 over 1.8x the neighbours per 5 s) because LookQueue read the totals 6 ms behind real time (Control's frames ~4 ms apart) but here Gyro4All's frames come once per Present (~15 ms), so the read was often past the newest frame. Fix: the delay is at least 1.5x the average spacing of `add()` calls
  and the hook passes the game's dt to `take()`, which advances the clock by it. **Verified (user, Epic, DualSense): "Steps gone"** (0-2 doubled steps per window, was up to 5). Not covered: aiming, character switches, photo mode, cutscenes. The 5 s `camera steps` measurement was removed after the test.
- 2026-10-30 **DOOM (2016) on AMD: cause confirmed, panel ABI 5, mouse fixed.** Second AMD log (queue fix build): panel fine in startup screens (queue family 0), but from the Campaign/Multiplayer/SnapMap screen every present comes from **queue family 1** (AMD's compute family, DOOM's async
  compute; queue families `0:15 1:14 2:12 3:0 4:32`), which can't draw, so the earlier fix skipped the panel (`renderer not ready`). Fix: the panel draws on the game's graphics queue when the present's queue can't draw. winmm.dll (`src/core/panel_vk.cpp`): `vkCreateDevice` reads the queue families' flags
  (`queue families, index:flags`); the first queue the game gets from a graphics family is the draw queue, passed in `g4a_vk_present` (ABI 5: `draw_queue`, `draw_queue_family`, `concurrent`, `lock_queue`); once presents come from another queue, the game's `vkQueueSubmit`, `vkQueueSubmit2(KHR)`, `vkQueueBindSparse`,
  `vkQueueWaitIdle` on the draw queue and `vkDeviceWaitIdle` take a recursive lock (hooks on the import table and the interposer's exports) that gyro4all.dll also takes around its submission. gyro4all.dll (`panel_vk.cpp`): `pick_queue`; exclusive images get three submissions per frame (release on the present queue waiting on the game's semaphores,
  draw on the graphics queue, acquire back), concurrent images the draw alone; layout stays PRESENT_SRC_KHR; frames rebuilt when the present's family changes. Known gaps: a game submitting on the draw queue at the moment the lock is switched on; a game alternating present queues every frame. **User-confirmed on the integrated AMD GPU of their own PC:** panel opened in the front end and in game, used
  with mouse and pad; presents move family 0 -> 1 a second after the renderer starts and back/forth at screen changes (4 in 4 min); swapchain recreated at mode changes (1024x768 -> 3840x2160 -> 1920x1080).
  - **Mouse dead in game on AMD (no cursor):** DOOM's DirectInput mouse is **exclusive: cooperative level 0x5 (`DISCL_EXCLUSIVE | DISCL_FOREGROUND`)**, keyboard 0x16. Windows then neither moves the cursor nor sends mouse messages. `dinputpad.cpp` now also hooks `SetCooperativeLevel` (logged) and `Acquire`: while the panel captures an exclusive mouse, the game's first read unacquires it (`let go while the settings panel captures it`), its reads return idle and its `Acquire`
    reports success without acquiring; the first read after the panel lets go acquires again (`acquired again`). With the mouse let go the window got `WM_MOUSEMOVE` and ImGui followed. A second (likelier on NVIDIA, where the mouse had worked) cause in the panel code: ImGui's Win32 backend polls `GetCursorPos` only while it tracks no mouse area; a `WM_MOUSEMOVE` starts tracking and only `WM_MOUSELEAVE` ends it;
    gyro4all.dll passes messages only while open and clears ImGui's mouse at each opening (position -FLT_MAX), so after the mouse was used in the front end the leave arrived with the panel closed and in game ImGui never had a position again. Fix (`panel_main.cpp api_frame`): each opening passes `WM_MOUSELEAVE` and `WM_NCMOUSELEAVE` after clearing the input. Diagnostics: a second `input +..ms:` line with the window's mouse messages,
    OS cursor moves and the foreground state; the capture line lists the cooperative levels.
  - Test build for the AMD user (DOOM 2016, 6900 XT): `Gyro4All-v1.9.0.zip` repackaged at 1.9.0 (version kept by the user, so indistinguishable from the first 1.9.0 zip) with the uncommitted DOOM work (graphics-queue path, ABI 5, exclusive mouse let go, ImGui mouse tracking reset, diagnostics).
- Switch Pro Controller (shown as XInput, auto mode) in a game whose entry did not name it: works in menus and play, but the game shows a one-time "controller disconnected" alert when a save loads (data-driven `ActionPrimaryUserLoggedOutOrDisconnectedInputDevice`, trigger not found); the exe imports `XInputGetState` ordinal 2 (4-slot scan in `exe+0x20a2cf0`) and its IsXInputDevice (WMI sample with the Steam
  `28DE:11FF` exception, `exe+0x151ad29`) reads the Pro Controller as XInput; Gyro4All never hid it. Left as is (user's call).

## 2026-10-30 Alan Wake Remastered (Epic): Northlight detection and camera

- One exe `Game_f_x64_EOS.exe` (DX12 only), no `*_rmdwin*_f.dll` modules and no `data_pack2\`. Imports `XINPUT1_4.dll`, `d3d12.dll`, `dxgi.dll`, `xaudio2_9.dll`, `EOSSDK`, `fmod.dll`, `bink2w64.dll`; the pump is **PeekMessageA**; window class `AppWindowClass`. Data in the older layout `data\ep999-000.rmdp` + `.bin`. Detection (`northlight_engine.cpp is_aw_remastered`): `data\ep*.rmdp` without Control's coregame modules (Control is checked first). Frame on `frame_thread` like AW2's. **User-verified in game: "todo bien"**
  (DualSense shown as XInput controller 0).
- **Camera (camtrace, Epic build).** Plain sections, no Denuvo/Themida/anti-cheat strings; MSVC RTTI and classic camera classes (vtables exe-relative): `PlayerCamera` 0x769280, `TailCamera` 0x76e560, `AttachedCamera` 0x752928, `RailCamera` 0x76a360, `RevealCamera` 0x76b8a8, `PathCamera` 0x767ab8, `FreeCamera` 0x75c320, `CinematicCamera` 0x756b30, `DeathCamera` 0x758270, `Character` 0x754780. Statics: `exe+0xb05df0` -> PlayerCamera,
  `+0xb05e00` FreeCamera, `+0xb05e08` CinematicCamera, `+0xb05e18` DeathCamera. TailCamera: `+0x08` 3x3 rotation (rows right/up/forward, y up), position `+0x2c`; a write is kept ~0.3 s then eased back (smoothed output); copied from a stack matrix at `exe+0x2f008b` in TailCamera's update `exe+0x2ef3a0`, which builds it from a look direction (`exe+0x2f3d10` returns the global `[exe+0xb20980]`, `exe+0x15f4d0` turns it into yaw/pitch, the pitch gets
  `TailCamera+0x14c` added, `exe+0x14c4c0` builds the matrix plus a ~7 deg shoulder offset in yaw). Dead ends: `Character+0xbc` and a float yaw at `0x6f1f650+0xfc` (consumers).
- **The source: `[exe+0xb20980]`** (heap struct, no vtable): `+0x00` eye and `+0x0c` target, **`+0x18` the unit look direction** (y up, heading atan2(x, z)), `+0x24` yaw and `+0x28` pitch radians (derived by `exe+0x29b8a0`; undone). Write test `--yaw 20` on `+0x18`: kept and TailCamera followed. Written by **`exe+0x29f1b0`** (rcx = an object holding the look struct at `+0x50`, rdx = a 3x3 rotation; dir = M x dir, then `exe+0x2f5390` stores it and recomputes `+0x24/+0x28`);
  signature `48 8B DA 0F 29 74 24 30 48 8B F9 48 8D 54 24 20 48 8B 49 50 E8` (at +0xa, once); the method runs with the stick idle too (`PlayerInputReader`, vtable `exe+0x7694b8`, method at vtable+0xc8, called from `exe+0x180535`). `awr_camera.cpp` hooks the look turn at configure and turns the direction at `[reader+0x50]+0x18` before the game's own turn (LookQueue, no dt); AW2 and AWR share one `HookedCamera` table. **User-verified: "perfecto"**
  (`look turn hooked at Game_f_x64_EOS.exe+0x29F1B0`). Not checked: pitch clamp, aiming, Attached/Rail/Path cameras, cutscenes.

## 2026-10-30 Silent Hill 2 (remake, UE 5, `SHProto-Win64-Shipping`): camera probe (user, 1.9.0, DualSense)

- Same Bloober framework as Cronos (PC `SHPlayerControllerPlay_BP_C`, pawn `SHCharacterPlay_BP_C`, PCM `SHPlayerCameraManager_BP_C`) but Cronos' offsets don't carry over (`Pawn.View` +0x2A0/+0x420 read garbage). Probe run 1 (orbit radius 72-122): live test **`Pawn.SpringArm` raw +0x1E0,+0x348** and **`Pawn.View` raw +0x470,+0x4F8** both MOVE THE CAMERA; `Pawn.View` also turns the character; `Pawn.Camera` +0x1E0 turns the view in place; others outputs. Run 2 verified nothing. With the run-1 profile
  (default ControlRotation + `Match=Camera` SpringArm) the camera turned standing still but not walking and aiming moved the crosshair without turning James. **Profile with `Pawn.View` +0x470,+0x4F8 as the default view works "100%"** (standing, walking, aiming); the SpringArm view isn't needed (shipped profile is the single `Pawn.View` view; GAMES.md row).
- Why the probe learned the wrong one: `finish_tests` learned the first verified group in candidate order (SpringArm before View) and only one. **Probe fix (unreleased):** among several that pass, pick one that turns pitch too, then one that also turns the character (`ProbeGroup` carries `pitch_followed`, `turns_pawn`; `probe_groups()` sorts verified ones by that rank so `finish_tests` and `probe_suggestion` agree; `Gyro4All-probe.log` says which was picked and why). 695 checks; not re-run in game.

## 2026-10-30 Days Gone (Steam, UE 4.11 by Bend, `DaysGone.exe` in `BendGame\Binaries\Win64`): object array not found, then solved

- 1.9.0: `GUObjectArray not found yet`, 0 XOR-encoded loads, heap scans nothing; monolithic exe with SteamStub `.bind`. Read-only: **ObjObjects is the pre-4.20 flat `FFixedUObjectArray`** (`Objects*`, `MaxElements` 2097152, `NumElements` ~470k, in `.data` exe+0x4C122A0), FUObjectItem stride **0x10**, Object at +0; **GNames** global pointer (exe+0x4AA22F8) to a heap `TNameEntryArray`, FNameEntry characters at **+0x12**.
  UObject stock; SuperStruct +0x30, Children +0x38, UField::Next +0x28. Fix: `ue.cpp` accepts a flat object array (`flat_object_array_stride`, `g_obj_flat`) and name text at +0x12. In game (DualSense, no Steam Input): `layout (UProperty objects) SuperStruct=0x30 ChildProperties=0x38 Field.Class=0x10 Next=0x28 Name=0x18 ElementSize=0x34 Offset_Internal=0x50 sizeof(Property)=0x78`; PC `BendDefaultPlayerController_C`, pawn `BendDefaultPlayerPawn_C`,
  PCM `BendCamManagerBP_C`; **`ControlRotation` is ignored by the camera**. Probe captures "could not be compared" because UE 4.11's PCM property is `CameraCache` (not `CameraCachePrivate`): `probe_diff()` now accepts `.CameraCache.POV.Rotation`. Probe then found only outputs.
- Reflection walk: `BendCamManagerBP_C` (native `BendCamManager`): `m_camInstances` (+0x3600, TArray) one instance per mode (`BendPlayerCamFollow_C_0` exploration, Combat, Sprint, ManualAim, Crouch, vehicle ones, ~37). `BendPlayerCamFollow_C_0` (size 0x380): **+0x80 and +0x120 are FMatrix (forward, right, up, location)**, +0x160 a yaw-only frame (orbit pivot), no float yaw angle anywhere: the view is kept as matrices, which the probe's angle diff can't match.
  Write tests (user saved; `um backup create` `daysgone-saves`): **+0x120 forward and right turned 20 degrees about Z: KEPT, the POV yaw followed at once and stayed: the source**; +0x160 undone (output); +0x230 = -(pitch radians) undone. Pitch: forward and up of +0x120 turned about the right axis kept and applied the same frame (an earlier "delay" was the game not in front). The user saw the yaw and pitch writes on screen.
- **New view option `Matrix=1`** (with `Offsets=`): each offset is a float rotation matrix (FMatrix rows X forward, Y right, Z up, 16 bytes apart) instead of an FRotator (`ue::read_matrix_rotation`: pitch = asin(X.z), yaw = atan2(X.y, X.x), roll = atan2(-Y.z, Z.z); `add_matrix_rotation` rebuilds the rows as FRotationMatrix, roll kept, pitch clamped +-89). Documented in ADVANCED.md. First profile `Object=Camera.@BendPlayerCamFollow_C`, `Offsets=0x120`, `Matrix=1`: on foot the gyro turns the camera; aiming and the bike do nothing (other camera modes are separate objects, all `BendCamBase` with
  output matrix at +0x80 and source at +0x120; aim cameras derive from `BendCamManualAim`, per weapon; the bike's `BendVehicleCamFixedAim_C` too). `BendCamManager` +0x35C8 points at `BendPlayerCamFollow_C` even while other modes run (not the active camera). Pitch: the gyro wound the follow camera's source to -75 while the output stayed at -20.9 (the game's pitch limits are `m_fMinPitch`/`m_fMaxPitch` -0.95/1.34 on foot, -1.5/1.5 aiming).
- **Dead end: `ActiveMatrix=<offset>`** (select the camera whose matrix matches the POV): aiming added sway so the match flapped (`view: Aim`/`view: none` every ~1.9 s); dropped. **Solution: `BendCamManager` +0x35D0 is the camera stack** (native TArray<BendCamBase*>, newest first): walking `[Follow]`, aiming `[AssaultRifleAim, Follow]`, bike `[VehicleCamFollow, Follow]`; while blending entry 0 is a `BendCamTransition`. New view options **`ActiveList=<offset>`** (+ `ActiveListSkip=` classes): the view is active while the list's first entry past the skipped classes is of its classes (`Object=...@A|B|C`), views in profile order (Vehicle before Aim, because the bike's FixedAim is a BendCamManualAim);
  **`OutputMatrix=0x80`** brings back a source pitch beyond the output's (|source| > |output| + 1). Profile (all three views `Offsets=0x120 Matrix=1 ActiveList=0x35D0 ActiveListSkip=BendCamTransition OutputMatrix=0x80`; Vehicle's Activation can be set Off to keep the gyro off the bike). **In game (user): views switch cleanly On foot / Aim / Vehicle with no gaps**, `camera: confirmed ... view 'Aim'`. Saved to `profiles/DaysGone.ini`, GAMES.md row. Regression: Cronos works as before.
- 2026-10-30 **Mass Effect 3 LE: camera drift from the stick fallback (GitHub issue #2); `StickFallback=0` and an ME3 profile.** Report: drift while the gyro is active, direction set by where the right stick came to rest, none with the gyro off; DualSense wired/wireless, Steam Input on/off; the game reads the DualSense as Gyro4All's XInput controller. 1.8.3 never found the camera (decoy name table), so the blind stick fallback ran (also the cause of stutter turning with the stick); 1.9.0 (pre-release) found it and wrote
  `Actor.Rotation` directly but the fallback still started three times in 25 minutes (two ended at the first probe write; one lasted 61 s with the pad lying still, probe writes under `kMinStep` 0.11 deg give no verdict). **Cause:** `StickFallback::stick` sizes the deflection for a centred stick and `xinput.cpp`/`dinputpad.cpp`/`nativepad.cpp` add it to the raw right stick including its resting offset (RX +1156..1413, RY +1928..2185); slow turns and noise go out as pulses only `kPulse` (0.04) past the model's dead zone, less than the offset,
  so pulses toward the offset pass amplified and the rest fall inside the dead zone: zero-mean noise becomes a steady turn toward the resting offset (0.08 deg/s measured; a port of `stick()` reproduces it). Also: Unreal3 has no `limit_turn`; `nativepad.cpp` truncates the added byte (`uint8_t(...)`), a one-step bias toward left/up. Change: `StickFallback=0` (`Gyro4All.ini` or profile, read at start) keeps the fallback off (no detection, no blind mode; with no camera hook the gyro then turns nothing, logged once; also overrides `StickFallbackTest`); `profiles/MassEffect3.ini` sets it.
  Not a fix for the fallback itself.
- 2026-10-30 **ME3: `ue3tool.py` and `Unreal3::find_names` read a decoy name table.** `ue3tool.py find` stopped at `UProperty offset field not found`: it took `MassEffect3.exe+0x17AB058` (chunk 0 right, chunk 1 junk where a third of the names still read as identifiers), letting the 3/4-of-all-samples rule through; the real table is `+0x17B33D0`. Fix: 3/4 identifiers in every chunk in use (tool and module), plus upstream's repeated-chunk check. ME3 layout: GObjects `+0x1887E40`, Offset +0x94, ElementSize +0x74, ArrayDim +0x70, mask +0xD0, SuperField +0x60;
  scene `SFXEngine` -> `SFXLocalPlayer` -> `SFXPlayerController` (< `BioPlayerController`), camera `SFXPlayerCamera`, input `BioPlayerInput`. **First in-game run still no camera** (ME3Tweaks Mod Manager launch, Steam Input on): `find_names` ran 3 s after start when no sampled object used chunk 1 yet, so the per-chunk rule had nothing to reject the decoy with. Fix: when the Matrix planes don't read back (`find_members`) the table is a decoy: logged (`the name table at ... doesn't read Core.Object.Matrix's planes: a decoy`), remembered and skipped by the next `find_names`
  (cleared when nothing else fits). 698 checks.
- 2026-10-30/31 **Steam Input mouse in the gyro4all.dll panel: an experiment reverted, then our own Block/Convert (`src/core/steammouse.cpp`), user-verified.** User: the per-view `input.steam_mouse` row (Pass through / Block / Convert) shows in a DX12 game's panel but not in the static panel (GyroLib offers it only while a Windows mouse bridge is attached: `gl_overlay_win32_message` or `gl_sdl_attach_window`).
  - **Experiment (reverted by user decision):** a second `gl_overlay` as the window bridge (`WM_INPUT`/`WM_MOUSEMOVE` forwarded) plus the PeekMessage loop feed: the row appeared but did nothing. DOOM reads the mouse through exclusive DirectInput and registers no Raw Input, so the bridge never got the 3 device-less relative raw packets GyroLib needs to confirm a Steam stream (its low-level hook intercepts only after confirmation). Days Gone: `WM_INPUT 0` in gameplay with L2 held; low-level hook counts (injected 0, hardware 0) show Steam's gyro "mouse" never reaches Windows as mouse input there: the game uses the Steam Input API itself (GyroLib's Steam reader reports ok), so no mouse
    filter can help (turn the gyro off in the Steam layout). DOOM (no Steam Input API in its old steam_api64.dll): Steam's mouse *is* OS-injected (54-2833 injected motions per 10 s).
  - **Our own Block (`steammouse.cpp`)**, from GyroLib 1.3.1's source (MIT; `mouse_bridge.cpp`, `mouse_hook.hpp`, `mouse_route.hpp`: Steam's mouse detected as selected controller under Steam Input + >= 3 device-less relative `WM_INPUT` packets within 100 ms of injected motion; then its low-level hook returns 1 for injected motion while the game is in front, lease 100 ms; Convert uses the raw packets' deltas at 0.05 deg/count): a `WH_MOUSE_LL` hook on its own thread drops injected `WM_MOUSEMOVE` while the game owns the foreground and a 150 ms lease (renewed each frame) is fresh. Looser than GyroLib: any injected motion counts while Steam Input has the controller (AutoHotkey/DS4Windows would be blocked too).
    DOOM tests: (1) gameplay still turned: dinput8.dll reads motion with its own `WH_MOUSE_LL` installed on Acquire (newest hook first); re-installing after each Acquire (`dinputpad::mouse_acquires()`, `kReinstall`) changed nothing; (2) dinput8.dll resolves `RegisterRawInputDevices`/`GetRawInputData` at run time: Raw Input still carries injected motion after a low-level hook drops it; fix: inline hooks (`hooks::hook_function`) on `GetRawInputData`/`GetRawInputBuffer` that zero `lLastX/lLastY` of device-less relative packets while blocking (buttons and wheel kept):
    **Block works**. Parity with GyroLib: the setting is GyroLib's own per-view `context.<view>.input.steam_mouse` (`gl_setting_get`), so the row, texts, per-view values, inheritance and saving are GyroLib's; GyroLib shows the row only while one of its mouse bridges exists (`c->virtual_mouse && steam_input_available()`), so `overlay.cpp` again creates a `gl_overlay` next to the static panel (`attach_bridge`, `gl_overlay_process` before `gl_update`) that is never fed window messages (it only shows the row).
    Block and Convert take Steam's mouse everywhere while the game is focused, menus and the open panel included. Convert: the emptied Raw Input packets' counts are summed (atomics) and turned into the camera at 0.05 deg/count (yaw += x, pitch -= y), added only while the camera may turn; needs Raw Input packets. Double count fixed: `host_raw_input` reads each `WM_INPUT` through the same inline hook, so `hk_get_data` counts a handle once. **Mass Effect 3 LE (inline hook mode):** user32's raw input exports were already hooked by xinput.cpp/nativepad.cpp and MinHook hooks a target once: steammouse hooks win32u's `NtUserGetRawInputData`/`NtUserGetRawInputBuffer`
    (falling back to user32's). **User-verified (2026-10-31): Block and Convert work in DOOM 2016 (Vulkan, exclusive DirectInput mouse) and ME3 LE (DX11, protected exe, non-exclusive DirectInput mouse)**, with the row in the gyro4all.dll panel. Not verified: a game reading `WM_INPUT` itself (UE), DX10/DX9/OpenGL.
  - **DOOM pitch limit (found in the A/B):** the stick fallback fired only with Block when looking all the way up/down, because the id Tech module had no `limit_turn` (its `apply` clamps +-89 but DOOM's view stops short; with Pass through Steam's own mouse masked it). Fix (`idtech_engine.cpp`, deltaViewAngles paths): a view beyond 45 degrees that stays put for 40 ms (`kStuckMs`) while each turn asks for more pitch is taken as the limit (`idtech: the view stops at N degrees up/down`), per session; the first push may trip the fallback once. The `StickFallbackLearned=` line saved in DOOM's profile by those episodes (axial,0.300,1.00,1025.9,197.4,1) is still there.
- 2026-10-31 **ME3: XInput translation toggling every second (user log; not every run).** The watcher thread found "a real XInput controller" on every other pass: on that system's XInput DLLs (`xinput1_4.dll!XInputGetState` at +0x1750 and `xinput9_1_0.dll` at +0x1160, vs +0x9a60/+0x1b40 in the good run) the watcher's own call reached a Gyro4All hook through a chain `passes_through` doesn't know and was answered with the translated controller. Fix (`xinput.cpp`): the watcher thread is marked (`t_watcher`); `our_state` never answers it and the xinput1_4 inline hooks pass its calls straight on. New log line when the watcher's slot mask changes (`xinput: real XInput controllers in slots 0x..`). 700 checks; not tested.
- 2026-10-31 **`Unreal3::limit_turn`: the pitch limit learnt, so a push against it doesn't start the stick fallback** (PR 12 review: ME3 LE's fallback started while the gyro pushed against the game's pitch limit, which Unreal3 didn't trim: the game clamps `Actor.Rotation`'s pitch each frame and the core read that as undone writes). As in id Tech, the limit isn't read from the game (`Camera.ViewPitchMin/Max` aren't what every UE3 game clamps to,
  e.g. BioWare's `SFXPlayerCamera`): a pitch past 30 deg up or down (`kMinLimit`) that stays put for `kStuckMs` (40 ms) while every turn asks for more is the limit (`ue3: the view stops at N degrees up/down`), raised if the pitch goes past it, learnt again for each player controller (level loads). First ME3 run (165 FPS): limits learnt at 60.0 deg up and down but the fallback still started 5 times in 18 min, 0-7 ms before the `view stops` line (8 undone frames at 165 FPS is ~48 ms, shorter than
  `kStuckMs`). Fix: while the limit is being learnt the turn already stops wherever the pitch is stuck (from the second frame of a push), a push is judged on the untrimmed request, push threshold 0.03 deg/frame (was 0.25). Second run (~2 min): no fallback at the limits; one mid-scene start ended in 0.35 s (`the stick turns nothing`): the other cause (a scene holding the camera). PR 12 review fixes: with `StickFallback=0` and no camera hook an on-screen notice (10 s) joins the log; the decoy-table line is logged once per table.
- 2026-10-31 **Multi-engine GyroLib zoom compensation** (709 checks; user-tested in Days Gone): `ViewInfo.zoom_available` per view; `EngineFrame.zoom_fov` carries the current and unzoomed reference vertical FOV; the core reports valid values to the active GyroLib view before every update (ADS/ALT_ADS/OTHER included). Unreal reads `PlayerCameraManager CameraCachePrivate.POV.FOV` (`CameraCache` in older UE4), horizontal per `FMinimalViewInfo`, plus `POV.AspectRatio` and `ProjectionMode`;
  perspective FOV converted with `2*atan(tan(horizontal/2)/aspect)`; the reference is `DefaultFOV` or the horizontal `ZoomReferenceFOV=` of the view profile. Missing/invalid data and orthographic views bypass scaling; no extra scale in camera writes. **Unreal3** (tested): optional `PlayerController.PlayerCamera`, `Camera.CameraCache`, `TPOV.FOV`, `PlayerController.DefaultFOV`; reports `Camera.CameraCache.FOV` as the current horizontal FOV and the reference (`[Unreal3] ZoomReferenceFOV=` else `DefaultFOV`),
  converted with the game window's client aspect (largest visible top-level window). **`ZoomReferenceFOV=` generic fallback:** the most specific place wins — a `[View.*]` section (Unreal) / `[Unreal3]` — then a generic `[Gyro4All] ZoomReferenceFOV=` (profile or Gyro4All.ini), then the engine's own default property (`DefaultFOV`); `load_view_extras` reads the `[Gyro4All]` value when the view section has none; `Unreal3::configure()` falls back to `settings::get_string`. **Glacier (HITMAN 3)** (tested): the camera controller holds the game's FOV as a float at **+0x200**
  (**40 on foot -> 25 aiming**, copies at +0x2D0, treated as horizontal, to verify); `zoom_fov()` reads it; the reference is the generic setting, else the widest FOV seen with the current controller; `views()` declares `zoom_available` only for the PeekMessage path (HITMAN 3). **007 First Light has no FOV source** (its L2 zoom is a short-lived transition camera without orbit state; hypothesis: the Gameplay World's derived view (+0x88) has the FOV as a neighbouring float, +0x114).

## 2026-11-02 Zombie Army 4: Dead War (Epic, Rebellion Asura, Denuvo)

- **Recon** (plan in `games/ZombieArmy4/`): `bin\ZA4_dx12.exe` / `bin\ZA4_vulkan.exe`, Asura engine, Denuvo Anti-Tamper (no debugger, so no camtrace `bp`), no anti-cheat, EOS for co-op. Imports WINMM and VERSION; the proxy loads. No engine detected, so the Unreal module ran as the fallback.
- **Stalls (fixed):** the Unreal fallback's search of writable+executable sections (every 10 s until GUObjectArray is found) ran on the game thread; ZA4's 358 MB RWX `.xtls` made each search freeze the game ~1 s (`perf: idle max=1058536us`) and closed the Vulkan panel ("no frame presented for 1078 ms"). Fix (`ue.cpp start_section_scan`): that search runs on its own thread like the heap scan; verified (idle max 200-350 ms). Then a hitch every 1-2 s: the per-second data-section search (GUObjectArray 47 ms in a 25 MB `.xcode` RW section, plus the name pool) on the game thread:
  `start_data_scan` moves it to its own thread for good once a try takes over 8 ms ("the data-section search took N ms on the game thread: ..."); verified (first try 187 ms; `perf: idle max` under 1 ms).
- **Vulkan panel:** HDR handling added (scRGB and HDR10, as the DX11 panel): ImGui draws in SDR into the panel's own RGBA8 attachment (subpass 0); subpass 1 composes it over the swapchain image in linear light, UI white 203 nits, reading it as an input attachment (`panel_vk.cpp`: `Mode`, `make_render_pass`, `make_composite`); HDR10 also reads the game's pixel as an input attachment (GENERAL layout): winmm.dll adds `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` to HDR10 swapchains in `hk_create_swapchain` (retried without; then `Hdr10Pq` blends in PQ). ABI 8: `g4a_vk_present` gets `color_space` and
  `input_attachment`. Shaders `src/panel/shaders/panel_vk_hdr.hlsl` compiled to SPIR-V by `tools/compile-vk-shaders.sh` into the checked-in `panel_vk_shaders.h` (needs DXC with SPIR-V output; DXC v1.9.2609 in `third_party/dxc-1.9.2609/`). Verified HDR on: format 97 colour space 1000104002 (scRGB), no Vulkan errors; HDR10 untested (no Vulkan test game). **Mouse behind the panel on Vulkan:** ZA4 reads Raw Input mouse itself (`GetRawInputData`, later `GetRawInputBuffer`): `overlay::filter_raw_input` (called from xinput's hooks) empties mouse blocks while the
  panel has the mouse; verified (clicks no longer reach the game). Hover still does: the game follows `GetCursorPos`, whose import EOS's overlay rewrote; accepted. Later (below) the cursor-centering fix.
- **Camera search without a debugger (read-only, write tests; DX12 exe, mouse, Denuvo):** camtrace `snap`/`vectors`/`dump`/`write-test` plus guided scripts (sound cues: high beep = turn, two low beeps = still; three full snapshots of static data + heap, ~3 GB, ~1 s each, 6 poses, turns up to 170 deg, pitch +-60). Bug in camtrace `vectors` (and any two-snapshot compare that zips the region lists): snapshots of one run don't have the same regions (1642 vs 1644), so pair by start address (fixed). The view exists only as rewritten copies: a forward vector follows it in 26 places (static `exe+0x1a0ebd4`, `+0x1a20bd0`, `+0x1a88a30`,
  `+0x1a92428`, `+0x20c7ee0`, `+0x26f4810` (eye after it), `+0x2730980`; heap copies in the render camera blocks; a quaternion at `exe+0x1a0ebec`); write tests of 7 static and 4 heap copies all undone within a frame. No source value (angle in any unit, fraction, binary angle, accumulated mouse counts, quaternion, aim point). Camera object (heap): matrix, eye, near 0.1, far 2000, aspect 1.7778, vertical FOV 0.8727 rad (50 deg, twice) next to 0.3491.
  - **`tools/camtrace/guided.py`** (README "Guided search"; now the first step for a new game's camera): finds the view's copies by itself and keeps every angle, any-unit value, direction, quaternion or aim point that follows the view. First run picked a group of vertical up vectors as the reference (heading noise): fixed (near-vertical rejected, groups whose pitch changes preferred). Second run on the Vulkan exe: reference `exe+0x19302c0` (30 copies); found what the DX12 searches hadn't: `float pitch, yaw` radians at `exe+0x18ab450` (pitch opposite sign, yaw = heading) with the camera
    quaternion at `exe+0x18ab45c`, the same pair at `exe+0x18bc160`, yaw as turn fraction at `exe+0x18a66f0`: all undone (copies).
  - Offline look at `ZA4_dx12.exe` (code readable: the first section, named `.data`, is the executable one; Denuvo's VM is the 350 MB `.didata`): `camtrace refs` works; no usable MSVC RTTI; the camera kinds are named strings ("3rd Person Camera", "ADS Camera", "Photo Mode Camera", ~25 more) copied by static initializers (`exe+0x44a10`) into heap string objects (`exe+0x20b89e0`); camera console commands. Not followed.
  - **The in-process write watcher (core `WriteWatch`, used on Dead Space with Denuvo) is the Denuvo-safe replacement for camtrace `bp`.** Extended: `exe+<hex offset>` addresses, and two input buffers to follow the input forward: `xi` = the game's last `XInputGetState(Ex)` buffer (`r:xi+c` = sThumbRX), `ri` = its last `GetRawInputBuffer`/`GetRawInputData` buffer (`r:ri+24` = first mouse report's lLastX). **WriteWatch in ZA4 (Vulkan exe, mouse, 10 s traces): works with Denuvo, no reaction**, followed the copies back to a source in five traces:
    `exe+0x18AB450` is written by a block copy (`exe+0x356F90`, from the camera manager's view) and `exe+0xFD110` (matrix-to-angles). The camera manager (heap; update `exe+0x6522B0`) blends its current and previous camera (`+0x190`, `+0x198`; a camera's type at `+8`, eye `+0x28`, look direction `+0x34`, blend time `+0x18/+0x1C`), builds a matrix from the blended direction (`exe+0x143DC0`) and angles from it into its own view at `+0x40`; the active camera (vtable `exe+0xC189C8`) is embedded at manager+0x1A0. The look direction (`+0x34`) is set by a plain setter
    from `player+0x5A0` (caller `exe+0x652CB0`; setter later corrected to `exe+0x62B780`). **`player+0x5A0` (float x, y, z, unit, the player's look direction) is written by `exe+0x7A44E0` (normalises it), called from the player's look update `exe+0xA00EA0`**: `yaw = in[0]*k0 + in[2]*k2`, `pitch = in[1]*k1 + in[3]*k3` (two input sources times sensitivities, invert flags), aim assist (`exe+0x5C02F0`), then pitch/yaw rebuilt relative to the body, deltas added, pitch clamped, yaw wrapped, back to a direction. **Write-test of `player+0x5A0` (`--yaw 20`): kept**, the camera followed, the user saw the jump: the place to turn the view.
  - Path to the player: camtrace `ptrs` found `[[exe+0x18A7C50]+0x60]` but it died after a checkpoint reload; the game's own route from the look setter's callers: **controllers** = the list at `exe+0x17495D8` (`items` at +0, count at +0x10), a controller (vtable `exe+0xC8F608`) holds its entity handle at +0x30 and its input object at +0x9450; **entities** = the handle table at `exe+0x17451A0` (`objs` +0, `keys` u32 +8, capacity (power of two) +0x10; slot = handle & (capacity-1), probed onward; lookup `exe+0x16DBC0`).
  - **Asura module** (`src/engines/asura/`): detected by `misc\common.asr` above the exe's folder; finds the controllers' list and the entity table by code (`40 53 48 83 EC 40 48 8D 15 rel32 48 8D 4C 24 20 E8` and `41 B0 01 8B 51 30 48 8D 0D rel32`, one target each in both exes: DX12 `exe+0x18AC8A8`/`exe+0x18A8470`) and turns `player+0x5A0` (same offset in DX12). **User tests (DualSense, native, Vulkan then DX12): the gyro turns the view**; pitch first inverted (**y grows looking down**), fixed. Pitch limit is smaller while aiming (80 down vs 85 both ways): learnt from the game's clamp and dropped 10 degrees inside it.
    - Pause flag: new `tools/camtrace/states.py` (sound-cued state switches, static data; `--out`/`--also` intersect two states): `exe+0x18A88F4` (Vulkan; 38 code refs), 0 in gameplay and 1 in the pause menu; found by `38 1D rel32 F3 44 0F 10 05 rel32 4C 89 AC 24 90 00 00 00 0F 29 74 24 70` (DX12 `exe+0x1A0BA98`). Weapon wheel (held triangle): the game blocks only the stick and keeps most gyro writes; **dead end:** inferring a frozen look from 3 undone writes + probe writes every 15 frames (it fired in gameplay too). New core setting **`GyroOffButtons=`** (profile button names; held over 200 ms the gyro's camera gate closes until released, `gyro off: a GyroOffButtons button held`); ZA4: `GyroOffButtons=Triangle`. **Verified (DX12, 2026-11-04):** pause flag and the Triangle gate work.
  - **Cursor fix:** with the F10 panel open in gameplay the cursor kept going back to the middle (the game centres it with `SetCursorPos` and clips with `ClipCursor`). The first fix (import hooks on `user32!SetCursorPos`/`ClipCursor`) was followed by **the game closing right after Gyro4All loaded** — **correction: not a Gyro4All bug, a Denuvo activation error.** The closes followed new builds, not code (even a known-good build + a 30 ms `Sleep` in DllMain closed, the vanilla exe started); the user then saw the window behind it: Denuvo's `88500006` ("can't revalidate your purchase now, try again in 24 hours": too many activations). Likely, not proven: the Denuvo token
    covers something that changes with every new `winmm.dll` build, each new build reactivates, and after a day's quota only configurations activated before still start (each exe, Vulkan and DX12, has its own). **So: deploy few builds to a Denuvo game, test changes in bulk, and on a close at start ask for the error window before bisecting.** DX12 test: still recentred because Epic's overlay wrote its hooks over the exe's cursor imports after ours; now inline hooks on user32's `SetCursorPos`/`ClipCursor`, installed the first time the panel takes the mouse (log `panel: while it had the mouse, the game's N cursor moves and M clips were held back`).
- **Fallback starts while scoped (2026-11-04) — long chase, solved:**
  - Symptom: the stick fallback started at times, mostly scoped; no `look copy` line (the `[player+0x588]+0x20` copy never held the look's direction; it is something else); two came after a false pitch limit (an undone write went back to where it was and `learn_limits` took it for a clamp). Changes: each write gets a verdict on yaw alone; a pitch limit learnt only from a write not undone with the yaw kept; diagnostics (`asura: the look copy ...`, `asura: the game undid the gyro's writes 3 frames in a row: ...`). Core WriteWatch: `writewatch::request(setting, seconds, why)` (engine-armed watch taken within 500 ms when `WriteWatch=on`; Asura provides the name `player`).
  - Offline reading of the look update: the controller's update (`exe+0x9B90A0`) calls the camera manager's update and then, unless the controller's state `+0x38` is 4, **sets the player's look from the manager's view matrix** (`exe+0x9B922B`, runs every frame in play, ~165/s, and in the pause menu); the manager first pushes the look to its current and previous cameras (`exe+0x652CB0`: `+0x190`, `+0x198` while blending, each only if its virtual +0x38 says so; setter `exe+0x62B780`, camera +0x34..+0x3C). In the look update (`exe+0xA00EA0`): start direction `player+0x5A0` (or `[player+0x588]+0x20`), deltas, aim assist, cone limit while the player's state (virtual +0x2E0) is 0x15 (axis `player+0x948`, cosine `+0x944`), then possibly **the look pulled toward an entity** (`rdi+0x18/+0x19`, entity id `rdi+0x1C`, 999 none; `exe+0xA01EA8`..`0xA02148`). Ruled out in game: the aim assist (options off), the **cone** (`player+0x944` stayed 1.0 in 13,644 samples), the copy and the camera objects (WriteWatch on `player+5a0` saw only Gyro4All and the look setter's store `exe+0x7A45F2`). The undone runs start 20-100 ms after `zoom: level 1`/`not aiming` (entering/leaving the scope).
  - **Cause: the manager's blend.** The scope's camera (vtable `exe+0xC17530`, type 3, vertical FOV 13.2 deg at +0x18) and the third-person one (embedded at manager +0x1A0, type 0, FOV 0.8727 rad) blend for ~0.25 s (byte +0x14 = 1; +0x16 = 1 in most transitions makes `exe+0x652CB0` push the look to no camera); the manager's view is rebuilt in its update `exe+0x64D720` (writer `ZA4_dx12.exe+0x64E041`): while blending it lerps the eye from the previous camera's +0x28 to the current one's, then with +0x16 set the direction is normalize(manager **+0x75A0** - eye), a world aim point set as the blend starts (writers `exe+0x650710`, `exe+0x64CB34`, `exe+0x64C677`; flags +0x75AC/+0x75AD), else normalize(lerp of the cameras' +0x34); the direction goes to the current camera (virtual +0x48), the eye to +0x40 and the 3x3 matrix to +0x4C..+0x6C, from which the controller resets the look (same offsets in the Vulkan exe). Turning the cameras' look directions (tried) doesn't reach the blend's view. **Fix: while blending with +0x16 set, the aim point is turned around the eye (+0x40, last frame's) by the gyro's delta, keeping its distance** (`asura: blending: the aim point N away turned with the gyro`). **Vulkan test: "worked perfectly"** (gyro keeps turning through the scope's zoom in and out, no fallback; one 3-frame undone run on another weapon's aim camera, type 4 at 33.8 deg).
  - **Zoom compensation from the game's FOV:** `zoom_fov()` = the current camera's +0x18 (linearly blended from the previous camera's while the manager blends; the game's curve unknown) against `ZoomReferenceFOV=` or the third-person camera's +0x18; log `asura: camera FOV N degrees vertical (unzoomed M)`. With `ZoomSource=Auto` it replaces the manual zoom button. **In game (DX12):** works (`the game's field of view 13.2, unzoomed 50.0 ... gyro x0.248`; variable-zoom scope 13.2 down to 9.8 followed; x0.651 at 33.8 deg on another weapon). The fallback learned bad models from those undone runs and saved them: core `kMaxSaveSpeed` (a model faster than 900 deg/s at full stick is never saved); the bad lines (`axial,0.350,3.00,1728.2,242.9,0`; `axial,0.350,3.00,288.8,86.8,10`) overwrote a good model once.
  - **WriteWatch crashed the game** (watch on manager +0x54): the second hit came with dr0 = the target, dr7 = 0xD0001 but **dr6 = 0**, so the handler passed it on and the single step was unhandled (WER 0x80000004). The handler now also takes a single step whose DR6 names no breakpoint while DR0 is enabled and the trap flag is clear. Execute addresses in WriteWatch no longer need 4-byte alignment. Bug in the camera build: `read_cameras` passed a size to `mem::plausible`, whose second argument is the alignment (0x40), so the third-person camera at an address ending in 0x60 was rejected.

## RE Engine TDB 82 game (Onimusha: Way of the Sword demo; 2026-11-04)

- **In game (DualSense, REFramework installed):** the TDB 82 build reads the TDB (`re: TDB 82 ready: 156902 types`), finds via.Application (365 entries), hooks `LateUpdateBehavior`; `re: no known player camera` -> gyro as right-stick input (works), F10 panel works (DX12 through Streamline). **Error report window at start** (the game keeps running behind it): Capcom's `CrashReport.exe`; its `CrashReport.txt` gives exception **0x406D1388** from SDL3.dll (GyroLib's): thread "DeviceHotplugThread": SDL's `SDL_SYS_SetupThread` names the thread with `SetThreadDescription`, then `AddVectoredExceptionHandler(1, ...)` + `RaiseException(0x406D1388)` (the old debugger thread-name
  exception) + `RemoveVectoredExceptionHandler`; SDL's handler should swallow it but here the dispatch went past every vectored handler to the game's `UnhandledExceptionFilter` (starts CrashReport.exe and waits, blocking SDL's hotplug thread; the game's protection likely disables vectored handlers). It never showed before because with TDB 82 unsupported GyroLib never started. SDL3 has no hint to turn it off. **Fix:** `deps::guard_sdl_thread_names` (in `start_gyro` right after `LoadLibraryW(gyrolib.dll)`): gyrolib.dll delay-loads SDL3.dll through its own `LoadLibraryExA/ExW/A` imports, patched (new `hooks::add_in_module`); once `SDL3.dll` is loaded its `RaiseException` import goes to a filter dropping 0x406D1388 (the thread is already named); if the game has its own SDL3 loaded that one is patched. Logs `deps: SDL3.dll's RaiseException patched ...`, `deps: SDL3 thread-name exception (0x406D1388) skipped`. Verified (no CrashReport window).
- **Camera model** (reuses `mhwilds_camera.cpp`, "Onimusha: Way of the Sword (app.GameCameraController)" when `app.CameraManager` has no `_MasterPlCamera`): `CameraManager._InGameCameraOperator` (+0x70) -> `cInGameCameraOperator._GameCamera` (+0x8, `app.GameCameraController`) -> `BaseCamParam` -> `<EyeAttachSetting>`/`<LookAtAttachSetting>` -> `<AttachParam>` -> `_AttachDeg`; singletons from the exe's slots (`app.CameraManager` `+0xBD70600`, `app.PauseManager` `+0xBD70658`); pause from `_ActivePause`; pause bits 0x1, 0x100, 0x400, 0x402. **BaseCamParam's `_AttachDeg` is rewritten every frame** (stick fallback; unlike MH Wilds the value is rebuilt elsewhere). The game has its own gyro code (`app.cCameraOperateCalculator._CameraGyroOperateCalculator`, options `GyroIsReverseX/Y`, `GyroSpeedYawLevel/PitchLevel`; not checked whether the PC demo offers it).
  - A once-per-session write test (+20 yaw into each candidate; started only after the camera turned by itself and held still 30 frames) found **`GameCameraController._BaseParamWorker` (+0x90) `._CurrentParam` (+0x0, ace.cCameraParam) -> eye/look-at `_AttachDeg` keeps the turn** and BaseCamParam is rebuilt from it; the gyro then turns the camera with no fallback (yaw -188.7 / pitch +13.4 applied). (The user's `gyrolib.ini` had `context.1.gyro.activation=6` = off and later 5/HOLD_DISABLE with `activation.trigger=2`, which explains gyro-off moments, not model bugs; Gyro4All's manual zoom aims with L2 while this game's bow aims with R2.) The write test, candidates and learnt limits were then removed: the route always turns `_BaseParamWorker._CurrentParam` (plus `turn_blend`).
  - **Pitch limit:** the game clamps tighter than the model's +-80 and the clamp read as "undid the writes" (stick fallback at the limit). `mcCam_DynamicDegLimit` (`GameCameraController._MiniComponent.DegLimit` +0x18): `_CurrentDegLimit` +0x78 / `_StartDegLimit` +0x68 / `_TargetDegLimit` +0x70 (`cDegLimit`: YawCenter 0x0, YawRange 0x4, PitchCenter 0x8, PitchRange 0xc, YawFollowRot 0x10), **`_StaticPitchLimit` +0x20 (vec2: -70..45 in the demo)**. A learnt-limit rule went wrong (the camera coming back toward level was taken as new limits); `Engine::limit_turn` was added to the RE module (`Camera::limit_turn`, forwarded by `ReEngine::limit_turn`; MH Wilds too, with its `_PitchDegreeRange`): range = `_StaticPitchLimit` when usable (finite, over 10 degrees wide, within +-90; `re: pitch limit -70.0..45.0 (...)`) else +-80. **User: pitch direction correct.**
  - **Lock-on:** fallbacks during lock-on (`app.mcCam_CameraLockOn`, `_MiniComponent.LockOn` +0x58: `_IsActive` +0xf8, `_TargetObject` +0xb8, `_CurrentLcokOnDeg` +0x60, `_AddDeg` +0x70, `_RotationDegree` +0xe8; player side `app.cPlayerLockOnSupporter`). Free look now needs `_CameraMode` 0 (NORMAL), `<IsActiveLayer>` set and lock-on `_IsActive` clear (else the view isn't gameplay: the gyro is off and the gyro as right stick can't switch lock-on targets; view line `, locked on`). **User: works, the gyro stops during lock-on.**
  - **Camera blends (R2 bow aim):** each aim in/out was followed by a fallback (`ace.cCameraParamWorker` rebuilds `_CurrentParam` from its work points while blending: `ace.cCameraParamWorkPointBase`: `<StartCameraParam>` +0x10, `<EndCameraParam>` +0x18, `<CurrentCameraParam>` +0x20, `<PreviousPointInfo>` +0x28, `_RequireTime` +0x3c, `_TotalElapsedTime` +0x40; worker `_WorkPointList` +0x8, `_PreviousWorkPoint` +0x10; `app.mcCam_BaseWorker` is an `mcCam_CameraParamSelector`: `_GoalParam` +0x38, `_Worker` +0x40). Fix `turn_blend`: each gyro write's pitch/yaw step is added to the eye and look-at `_AttachDeg` of every work point's Start/End/Current param (list up to 8, plus the previous point) and `_GoalParam`. **User: "perfect"**, no fallback. Cleanup: offset dumps folded into `re: scene types (Onimusha)`; status `mode= layer= locked_on=`; GAMES.md and ADVANCED.md rows, tools/reEngine/README.md TDB list (71, 82 added). Open: yaw direction not explicitly confirmed; a few fallbacks around `camera none`/`cCameraParamArgument_TPLookAtSet` in combat; Issen (mode 1) and action/motion/rail/dialogue cameras not looked at.
## 2026-11-03 Red Dead Redemption 2 (Rockstar launcher, RAGE, 1.0.1491.50): recon and the RAGE module

- **Recon** (`games/RedDeadRedemption2/`): RAGE, native x64, `.text` encrypted at rest (entropy 8.00; Arxan-style anti-tamper is a guess); not Denuvo; no anti-cheat in the folder; story mode only, never Online. Imports `XINPUT9_1_0`, `DINPUT8`, `WINMM`, `VERSION`. `dinput8.dll` is taken by the ASI loader (ScriptHookRDR2), so the proxy goes in as `winmm.dll` (or `version.dll`). The user's install also has `DualSense.asi` (adaptive triggers, own HID access). **GTA IV** is 32-bit (`GTAIV.exe` i386; only `PlayGTAIV.exe` is x64): Gyro4All is x64-only,
  do not deploy there. New module `src/engines/rage/` (`RAGE`, last in the registry): detected by `common_0.rpf` and `shaders_x64.rpf` (also loose `common\data` + `pc\data`); `PlayGTAIV.exe` is a launcher.
- **Input and frame:** the proxy loads under the Rockstar launcher; the game window (class `sgaWindow`, "Red Dead Redemption 2") is pumped by another thread **only now and then in the front end/menus** (`PeekMessageW calls ... 3996:22/51` in 10 s, then none for 20+ s), so the gyro frame (and the translated XInput state) barely ran and the game read a frozen all-zero pad. Fix: `RageEngine::frame_source`: the module's own thread runs the frame on a 4 ms high-resolution timer (no Present hook; `present: -`, the panel never hooks this game's swapchain). The game polls `XInputGetState` *and* reads the DualSense through DirectInput
  (`GetDeviceState`, DIJOYSTATE2, from `RDR2.exe+0x263B07C`; also `socialclub.dll`), no native DualSense support, and checks XInput devices through WMI. `dinputpad::refuse_pads` (XInput=2): the game's DirectInput `CreateDevice` of a 054C/057E controller is released and fails with `DIERR_DEVICENOTREG` (works). **Works (user, DualSense, `DualSense.asi` off) and later with the default `XInput=1`** (the `XInput=2` note was removed from GAMES.md).
- **Camera search (read-only):** `guided.py`: view reference `exe+0x3ec0a50` (a static camera block: rows right `+0x00`, forward `+0x10`, up `+0x20`, position `+0x30`, then a projection: `+0x40` x scale 1.2389, `+0x54` y scale 2.2024 = 1/tan(vfov/2): 48.9 deg, `+0x78` near -0.05; a view matrix whose columns are right, up and -forward; written only by a generic 0x630-byte struct copy at `exe+0x2F1640` from a heap viewport); 133-348 identical copies; no stored yaw/pitch angle in any unit follows the view (only copies); `guided.py` now re-reads the view
  right before each later move. A run killed the game (reads failed; write-test heap candidates right after the search, before the view mode changes). **FOV found:** `[exe+0x3ec0aa4]` float = the projection's y scale, updated every frame (2.2024 = 48.9 deg unaimed; aiming a rifle eases to 3.1716 = 35.0 deg over ~0.25 s and back over ~1.4 s); vertical FOV = 2*atan(1/value). WriteWatch (no debugger; the anti-tamper didn't react): pad objects `exe+0x5718890` (0x180 bytes per pad; pad 0's right stick floats X `+0xAC`, Y `+0xB0`); the camera reads mapped control values, not the pad.
- **ScriptHookRDR2 research route** (the user's install has ScriptHookRDR2 v1.0.1491.17; probe `games/RedDeadRedemption2/camprobe/camprobe.asi`, git-ignored, never shipped, calls natives by hash): GET/SET_GAMEPLAY_CAM_RELATIVE_HEADING `0xC4ABF536048998AA` / `0x5D1EB123EAC5D071`, PITCH `0x99AADEBBA803F827` / `0xFB760AF4F537B8BF`, GET_GAMEPLAY_CAM_FOV `0xF6A96E5ACEEC6E50`, GET_GAMEPLAY_CAM_ROT `0x0252D2B5582957A6`; the setters turn the camera (degrees, rate; 1.0 = at once). Handlers: GET heading `exe+0xF617F4`, GET pitch `+0xF61820`, SET heading `+0xF648C4` -> `+0xF75C50`, SET pitch `+0xF648D8` -> `+0xF75CB8`. **Gameplay camera director `[exe+0x3ea05e8]`**, active camera `director+0x7a0`; SET heading writes a request into the director (`+0xD90` heading, `+0xD94` rate, `+0xDA8 = 1`; pitch `+0xD98`, `+0xD9C`, `+0xDA9 = 1`), `exe+0x42055a` hands them to the camera (`+0x864` heading, `+0x868` pitch, flags `+0x8a8`), and `exe+0x421584(cam, float* heading, float* pitch)` blends. **Natives-based turning is unusable:** rate 1.0 makes the camera delete its helper every set (a jump every ~200 ms); rate 0.99 drifts (pitch sinks to the -70 limit) and re-set ~260 times a second blocked both gyro and stick.
- **The camera's orientation state: its stored front `cam+0x7b0`** (unit world vector; heading = atan2(-x, y) grows left; pitch = asin(z)). The orientation update is the camera vtable's `+0x248` slot (`exe+0x421584`; vtable `exe+0x339a6c0` on foot) called from `exe+0x45bae5`: reads `cam+0x7b0` (through the parent matrix `cam+0x6c0` when `exe+0x452bc8` says so), turns it into heading/pitch (`exe+0x1707e4`), blends the director's request; the caller adds the modifiers' sum (vtable `+0x230`, `cam+0x4c8`, `+0x100` each), `cam+0x860` (heading) and `cam+0x8a4` (pitch), clamps pitch to `cam+0x828..0x82c`, rebuilds the front (`exe+0x173618`) and stores it back (`exe+0x45bcae`). **Turning `cam+0x7b0` works:** first via a ScriptHook script between camera updates, then **memory only** (mode 3: the module's frame thread turns `[[exe+0x3ea05e8]+0x7a0]+0x7b0`), **user-verified**: on foot, aiming (L2), horseback, right stick alongside, no drift. The ScriptHook script, natives and `RageCamMode/RageCamRate` settings were removed. The game creates a new camera object on each switch, so the module reads the pointer every frame; `view_angles` adds the queued turn (a first try fell back to the stick after 6 s). Camera classes by vtable: `exe+0x339a6c0` on foot, `exe+0x3399308` aiming, `exe+0x3399c08` binoculars (first person, FOV 22.6); update slot `+0xc8` (`exe+0x45abe0`/`exe+0x45c3a0`/`exe+0x45a278`). **Aim release damping:** after a switch the director keeps the previous camera at `director+0x7a8` for ~1.5 s and blends from it; it no longer updates, so the module also turns that camera's front and frame rows (right `+0x20`, forward `+0x30`, up `+0x40`). Without a readable unit front (loading), the gyro goes to the stick.
- **Binoculars: no turnable state found.** Their `+0x7b0` is not a unit vector; the frame (`+0x20`, copy `+0xf0`) follows the view but writes don't stick (rebuilt every frame); no field of the first 0x1000 bytes tracks the view as an angle; `guided.py` (6 moves) left only HUD compass integers and stale copies. The module reads the view from the frame and writes there (undone), so the stick fallback learns the binoculars' stick speeds from the view. Next if needed: WriteWatch forward from the right stick (`r:xi+c`) or the binocular update `exe+0x45a278` offline.
- **Smoothness (user: "smooth now"):** a sampler of the active camera's frame forward row (`+0x30`, written once per camera update; the render view only changes 30 times/s) in slow pans: stick steps even (~2%), gyro steps uneven (~20%, updates with no step) because the frame thread wrote the front every 4 ms out of step with the game's updates (a write between the update's read and store is lost). The frame thread now polls that row every 1 ms and runs the gyro frame right after it changes (a change within 6 ms is the same update); with no change for 20 ms (still view, menus, loading) the frame runs anyway (50 Hz). Result: step variation median 0.036 (stick 0.018), 0 updates without a step.
- **Zoom compensation:** FOV from `[exe+0x3ec0aa4]` reported as the current FOV. First: the widest-seen reference captured a default 90-degree projection when the camera appeared after a load (`gyro x0.454` on the normal camera; the game also has several unzoomed cameras: first person and three third-person distances, on foot and horse). Now the reference is the FOV of the active camera whenever it isn't a zoom camera (vtable `exe+0x3399308` aiming, `exe+0x3399c08` binoculars), held while a zoom camera is active (`rage: zoom camera (aim or binoculars): unzoomed FOV ...`); `ZoomReferenceFOV=` still overrides. Measured over 17 aims: narrowest FOV 45.0 against 45.0-50.0 unzoomed (gyro x0.89-0.91; one 37.4 of 48.8; re-aiming during the ease back takes the eased FOV as unzoomed). Unknown: whether first-person aiming, scopes or horseback aiming use the same aim class.
- **Stick fallback against the pitch limit (core, all engines):** four fallback switches in 7 s looking up/down, and a stick model learnt from that mix saved to `profiles\RDR2.ini` (`axial,0.075,1.50,508.8,151.1,0`, implausible). The core's check summed both axes, so the pitch limit stopping the pitch part of a write read as undone. Fix (`stick_fallback.cpp`): writes are judged on yaw alone (no yaw limit); a pitch-only write gets no verdict. Test `test_stick_fallback_not_at_pitch_limit`. User: "todo correcto"; two short fallback runs remained (cause unknown).

## 2026-11-03 Resident Evil Village: DX12 panel too bright in HDR (ReShade + RenoDX)

- Backbuffer RGB10A2, HDR10 through RenoDX, but `SetColorSpace1` was never seen on the captured swapchain (ReShade's proxy is probably the captured one while RenoDX sets it on the real one): the Gyro4All window was drawn as SDR into a PQ buffer (excessive brightness and contrast), while GyroLib's panel looked right (its AUTO rule: RGB10A2 on an HDR output = HDR10). First added `PanelHdr=` (Auto/SDR/HDR10/scRGB), then removed: the DX12 window uses the same rule when no SetColorSpace1 was seen (`guess_color_space`, IDXGIOutput6 ColorSpace); D3D11 keeps its no-auto rule (Mortal Shell); scRGB value fixed to 1. Verified (HDR10, ReShade + RenoDX): the window matches GyroLib's panel.

- 2026-11-04 **Remnant: From the Ashes (Steam, UE 4.2x, `Remnant-Win64-Shipping.exe`): engine data unreadable, solved.** GUObjectArray found (exe+0x34B2C18, chunked, stride 0x18) but no name table: GNames is a global pointer (exe+0x34AE908) to a heap `TNameEntryArray` with **`Chunks[256]`** (not 128): NumElements (~389k) and NumChunks (24) at table+0x800; text at entry +0x0C. Fix (`ue.cpp legacy_names_at`): tries 128 and 256 chunk slots (`g_name_max_chunks`) and requires the last used chunk pointer to be set. Works (user); `profiles/Remnant-Win64-Shipping.ini`.
- 2026-11-04 **Metro 2033 Redux (Next-Gen Update, 1.10.0): the gyro stopped after ~2 min of play** (DualSense USB). `4a: main menu` / `gyro off: no gameplay view (target main menu)` mid-play: the main-menu flag (`metro.exe+0xF4B838`, found by `kMenuCode`) went back to 1 at a level change (the view owner changed with no "view angles gone") and stayed 1, also after Continue from a menu reached by quitting. The setters (`+0x73C1AA` (1), `+0x73C0EB` (0), `+0x73AAAD`) are wrapped in `inc/dec` of a counter around a call: a load-state guard rather than a menu flag (same code in 2033 and Last Light). In 2033 only the first scene after launch (~15 s before the menu) has a readable view; the main menu itself has none, so the gyro is off there anyway. No replacement signal found in `.data` snapshots. Fix (`foura_engine.cpp`): the flag counts only in the first scene after start (`first_scene_`), stopping once it reads 0 or that scene's view is gone (`4a: the first scene is over: the main-menu flag no longer counts`). Cost: in Last Light Redux the menu reached by quitting a level (a live scene) turns with the gyro again. Verified in 2033 (menus, pause, gameplay, level change). Metro Exodus EE doesn't match `kMenuCode` (`main-menu flag not found`), unaffected.

## 2026-11-04 Cyberpunk 2077 (GOG, patch 2.50, REDengine 4): recon, the REDengine module, native gyro works

- **Recon** (`games/Cyberpunk2077/`): `Cyberpunk2077.exe` 2.50 (file version 3.0.88.46235), no Denuvo, `.text` not encrypted, no anti-cheat. Imports `XINPUT9_1_0.dll` and `HID.DLL` (reads PlayStation pads itself). Window class `W2ViewportClass`. The user's install has RED4ext (as `winmm.dll`; 1.29.1/CET 1.37.1 predate 2.50, first 2.50 releases RED4ext 1.31.0, CET 1.38.0), Ultimate ASI Loader (`dinput8.dll`) with CET, and ReShade (`dxgi.dll`), so Gyro4All runs as `version.dll`. **CD Projekt's address file** `bin\x64\cyberpunk2077_addresses.json` (~150 MB, 1.1 M entries, 84 named; "Linker map timestamp" equals the PE timestamp) keyed by RED4ext.SDK's stable hashes: `CGameEngine` 0x97F209D6 = `0003:002bcb38` (global `CGameEngine*`, RVA 0x3BBFB38), `CRTTISystem_Get` 0x4A610F64, `CNamePool_Get` 0x68DF07DC.
- **gameCameraSystem keeps the active camera's data** (static, from the exe's native registrations): `GetActiveCameraData` (handler exe+0x2b44364) copies 0x80 bytes from `this+0x500`; `+0x500` int32[3] fixed-point position, `+0x510` float[4] quaternion (i, j, k, r), `+0x520` FOV (copies +0x80). Scene path (RED4ext.SDK layouts): CGameEngine `+0x348` framework, `+0x10` GameInstance, `+0x38` `DynArray<Handle<IScriptable>>` of the game systems; IScriptable `+0x30` CClass*; CClass parent `+0x10`, name (CName FNV-1a 64) `+0x18`. Camera classes: `gameFPPCameraComponent` (on foot; `pitchMin/Max` +0x3E0/+0x3E4, `yawMaxLeft/Right` +0x3E8/+0x3EC, `headingLocked` +0x3F6, `sensitivityMultX/Y` +0x498/+0x49C), `vehicleTPPCameraComponent` (size 0x680), `vehicleCinematicCameraComponent`, `gameWorldSpaceBlendCamera`, `gameFreeCameraComponent`, `entVirtualCameraComponent`; base `entBaseCameraComponent` (`fov` +0x128, `zoom` +0x15C), `entIPlacedComponent.localTransform` +0xC0. Camera system `+0x48` is a DynArray of handles holding the active camera component (on foot `gameFPPCameraComponent`, owner `PlayerPuppet`); the FPP component keeps the active orientation at `+0xF0`.
- **Module `src/engines/redEngine/red_engine.cpp`** (`REDengine`): detection `<root>\archive\pc\content` + `<root>\engine\config`; `REDEngineErrorReporter.exe` is a launcher; a thread in configure() resolves CGameEngine from the address file; RTTI without calling game code (the vtable's GetNativeType getter `lea/mov rax,[rip+x]; ret` when `+0x30` is empty); learns per class which offset holds the orientation matching the active camera (kinds: on foot view 1, car 2, motorcycle 3, cinematic view 0).
- **First runs (no RED4ext/CET, ReShade on):** (1) **session lost to the crash reporter:** the game starts `REDEngineErrorReporter.exe` at launch; the CreateProcess hook handed it the session before the first frame, closing the game's log (and truncating `Gyro4All.log` again), probably why 1.10.0 left no trace and no F10 panel. Fix (`dllmain.cpp`): a helper exe of a detected engine (`Engine::is_launcher`) returns before the mutex and the log, and `hand_session_to` keeps the session when the child is such a helper (`session: kept (... is a helper)`). (2) **Crash at the first scan:** Cyberpunk imports `AddVectoredExceptionHandler`; its handler sees an access violation before our `__except`, so a faulting guarded read ended the game. Fix: the module checks every read with VirtualQuery first (then QueryWorkingSetEx, below). **Other engines' guarded reads would crash this game too.**
- **Gyro "like jelly" on foot (the blind stick fallback: dead zone, curve, pulses, calibration drift) -> direct write.** Search with `guided.py --up z` (pass `--ref` = the forward vector checked against the camera system's `+0x510` quaternion; it first chose the largest group of unit vectors, heading only +-90). Copies with writers (one-hit cdb breakpoints): `{roll, pitch, yaw}` in a render-frame struct (`+0x31210`) from a quaternion; `gameFPPCameraComponent +0x42C/+0x430` (pitch degrees), pulled every frame (`exe+0x5fd7cc`) from the named float variables of the player's `entAnimatedComponent` (FPP `+0x380`; the table rebuilt each frame); the player root `entPlaceholderComponent` (`PlayerPuppet +0xB0`) orientation `+0xD0` (rewritten by `exe+0x404389`) and world `+0xF0`; `moveComponent +0x1D0` (written by `exe+0x20d83d`).
  - **Pitch source:** a float in degrees in an animation graph node's state (buffer of the anim graph instance, no RTTI header): a +20 write kept and the view followed. The native integrator `exe+0xa93d7c`: `state+0x1B0 += in[+0x8]` clamped to `in[+0x14..+0x18]`, `state+0x1C0 = state+0x1B0 + state+0x1B8`; its yaw twin `state+0x1AC += in[+0x0]` (0 on foot); FOV at `state+0x1A8`. Stable path: `FPP +0x380` (the player's entAnimatedComponent) `+0x240` (second entAnimatedComponent) `+0x270` (graph instance) `+0x28` -> pointer -> `+0xDA0`. **Yaw source on foot:** the mover update reads the body orientation every frame from `moveComponent +0x160` (native object, vtable `exe+0x3098390`) `+0x30` (quaternion; getter `exe+0x1945270`, reached from `exe+0x20c984` when `moveComponent +0x154 != 0` and `+0x140 != 0`); a 30 deg write there kept and turned the view (`+0x240` is identity when idle: no). The `moveComponent` is one of the PlayerPuppet's components (entEntity `+0xA0`). Live write test: yaw +20 and pitch +15 held and the view followed. FPP pitch limits +-80.
  - **Module:** on foot `apply` rotates the body quaternion about Z (right = negative) and adds the pitch to the look node; `limit_turn` keeps the pitch within the FPP limits; sources re-resolved and checked every frame, falling back to the stick on mismatch. First run alternated every frame between direct write and stick (the value checks ran after our own write while the copies still held last frame's view): the value checks now run once per object the path finds (`trusted_body_`, `trusted_look_`); after that only the structure. `view_angles` returns false (no write check). A closing GyroLib panel made one frame look undone and the camera id never changed so the core never came back: now no write check at all.
  - **Zoom FOV:** the camera system's `+0x520` (also `+0x80`) narrows while aiming (69 -> 56.85 with a pistol); the unzoomed one is the camera component's `entBaseCameraComponent.fov` `+0x128` (stays 69). The FOV setting (70-100) is not either value (80 gave ~60, 90 gives 69: tan(v/2) = 0.6875 tan(s/2)). Scopes: the camera system has a zoom factor at `+0x53C` (copies `+0x8C`, FPP `+0x27C`) 1 -> 5.25 with a sniper rifle's scope while `+0x520` stays; the module reports `2*atan(tan(fov/2)/zoom)` (first build reported twice the angle), declares zoom on all views.
  - **Vehicles:** in first person the FPP camera stays active and the body follows the vehicle, so the on-foot check failed. The look node's yaw (`+0x1AC`, 0 on foot) turns the head (`+0x1BC = +0x1AC + +0x1B4` copied to FPP `+0x42C`; recenters to 0 in ~2 s; pitch limits -20/60 seated). Seated vs on foot: moveComponent `+0x158` 0 vs 1, FPP `+0x400`/`+0x418` 1 vs 0; `mountedVehicle` (`PlayerPuppet +0x450`) null. Third person in car and bike: `vehicleTPPCameraComponent` matched once then not (stored orientation lags), so it counts as cinematic (gyro off). User asked for no gyro and no stick fallback while driving: seated reports no gameplay view. Weapon drawn while seated: sound-cued sampling found FPP `+0x454` 10 and `+0x460` 50 while driving and 0 with a weapon drawn in the seat (the first build had it reversed; also unreflected `+0x3CC`, `+0x444`, `+0x450`); the seated gyro is on only while both are 0. The game recentres the look yaw past some angle; one touch of the right stick frees the aim until the weapon is holstered (no flag found, only pose floats). Core `EngineFrame::stick_nudge_x`: a small right-stick deflection shown to the game (XInput, native pad, DirectInput) on frames where the player's stick and fallback are idle; the module sends 6 frames of +-0.2 alternating sides when a weapon is drawn in the seat and whenever the look yaw goes back toward 0 on its own (>0.15 per frame from beyond 3 deg, 20-frame cooldown; `seated aim recentring`). **Tested (user, DualSense wired, Steam Input off): the gyro aims from the car seat with a weapon drawn, is off while driving, works well** (a nudge every ~1.6 s while aiming).
- 2026-11-05 **Poor frame rate with Gyro4All on (new game).** `perf:` showed Gyro4All's own frame on the game thread ~1.8 ms growing to 9-12 ms average (p99 23 ms) after a few minutes. Cause: the module's safe-read check called `VirtualQuery` for every new region each frame; `VirtualQuery` walks the whole region (standalone test: 11 us at 16 MB, 255 us at 256 MB, 1.3 ms at 1 GB, 16.5 ms at 4 GB; `QueryWorkingSetEx` 0.32-0.38 us and `ReadProcessMemory` 0.34-0.36 us at every size). **Any per-frame `VirtualQuery` on game heap memory is suspect in other engines too.** Fix (`red_engine.cpp`): each 4 KB page checked with `QueryWorkingSetEx` (resident, readable or writable, no guard page), cached per frame in a 1024-entry direct-mapped table (a paged-out committed page reads as not readable); the camera system kept between frames while the game instance and its class stay. Tested: `perf:` idle 111-213 us, active 95-127 us, stable. Also 303 one-frame "on foot -> cinematic -> on foot" flips in 3.5 min (the classification matches the FPP component's stored orientation against the active camera within 0.9995 ~3.6 deg and the copy lags a frame): the component that matched last frame keeps matching within a dot of 0.985 (~20 deg, 2000 deg/s at 100 fps); other components still need 0.9995. Result: 8 flips in 6 min; scenes and the car camera still detected; released as 1.10.1.
## 2026-11-09 S.T.A.L.K.E.R. 2 (Steam, Unreal): false "DualSense ignored" at start

- DualSense USB, no Steam Input, `XInput=1` (the game supports the DualSense natively and it works). Gyro: generic `ControlRotation`, `camera: confirmed` in the view `Exploration`, ~9 min in game. The game imports `HID.DLL` but no `libScePad`; it opens and closes the DualSense during startup (partly through `dinput8.dll`), registers Raw Input usage 4/5 at ~+6 s, keeps a handle and reads report 0x01 at +7 to +11 s from the first open, then opens and closes another handle every 0.5 s.
- Bug: `dualsense_closed_unread()` gave up after 5 s, so auto mode showed the DualSense as XInput for 2-6 s (`not shown` -> `shown as XInput controller 0` -> `not shown: it registers Raw Input`), with two `WM_DEVICECHANGE` and notices, and with no profile value `learn_dualsense` wrote `DualSenseAsDualShock4=1`, `Learned=1` (an existing `=0` stops that write). Fix: the wait is 30 s when `hid.dll` among the imports is the only Sony sign (`xinput::sony_hid_import_only()`), still 5 s with `libScePad` or with no Sony sign (Titanfall 2). Requiring `libScePad` for learning would not work (Mortal Shell and others link Sony's library into the exe and only show as `hid.dll`). Cost: those games get the translated controller or the learned DS4 identity up to 25 s later. The HID sign's text says "maybe Sony's pad library"; only the first 60 controller open/close lines are logged (85% of this game's log). 1034 checks; not tested in game; no version bump.
- 2026-11-10 **RDR2: first-person gyro judders, third-person doesn't (user, ~95 FPS), measured and fixed.** Motion trace (18 s, mostly first person): sensor stream and pacing clean (applied speed varies 1.4%), but 162 of 1646 gyro frames last 20.0-21.6 ms (the frame thread's 20 ms timeout) with 2.07x the neighbours' step. Read-only sampler of the active camera's forward row (`cam+0x30`) and position (`cam+0x50`): first person is its own camera (vtable `exe+0x3396310`; third person `exe+0x339a6c0`). First person with the stick: one row change per update (10.4 ms), step variation 0.018 (smooth by itself). First person with the gyro: the game's update changes the row by no yaw/pitch (position moves, median 0.16 cm); the turn appears 0-1 ms later, so the row holds Gyro4All's turn and the game keeps it, and when the update leaves the row's bytes as they were the frame thread sees no update and waits for the timeout (45 steps at 20-21 ms in 7 s). Third person: the turn is in the game's own row change every 10 ms. Dead end: a chain of second writes under 6 ms apart (the measured second writes are Gyro4All's own). Fix: the frame thread also watches the frame's position (`cam+0x50`) to see an update (it moved over the missed updates, 0.37 cm median). Deployed: first-person turning steps at 20-21 ms gone (45 -> 0 in 7 s); third person unchanged. **User: first-person gyro smooth now** (controller not under Steam Input).
- 2026-11-11 **Routine (Steam, Unreal, `Routine-Win64-Shipping.exe`): "A D3D11-compatible GPU (Feature Level 11.0, Shader Model 5.0) is required to run the engine" at start.** The exe delay-loads `d3d11.dll`; Gyro4All patched its delay-load slot in DllMain (`hooks: d3d11.dll!D3D11CreateDevice patched in the delay-load table ... (not called yet)`), so the game's delay-load helper never ran and `d3d11.dll` was not loaded when the game called the hook. `hk_d3d11_create` resolved the original with `GetModuleHandleA` only, got nothing and returned `E_FAIL`, which UE reports as that GPU error. The same pattern was in other hooks: DXGI `CreateDXGIFactory*` (no fallback at all: a call through null), D3D9 `Direct3DCreate9*`, OpenGL `SwapBuffers`/`wglSwapBuffers`, the Vulkan loader path (`vkCreateInstance` -> `VK_ERROR_INITIALIZATION_FAILED`). Fix: one call-time resolver, `hooks::resolve_original(original, function, fallback_dll[, ordinal])`: the cached original, else the export from `source_dll(function)` (else the fallback), `GetModuleHandleA` then `LoadLibraryA`, cached with an interlocked compare-exchange. Used by overlay (D3D10/11, DXGI), panel_host (D3D9), panel_gl, panel_vk, xinput and nativepad (their own copies removed). Game profile `profiles/Routine-Win64-Shipping.ini`. 1034 checks; no version bump.
- 2026-11-11 **RE Requiem: gyro while aiming (scope, laser sight) and zoom compensation** (`requiem_camera.cpp`, `re_engine.cpp`; not released).
  Before, aiming (`ADSCameraController`) left the gyro off ("no gameplay view"); a user reported the same with a laser-sight pistol.
  - **Aiming camera:** `camera_finder.lua` through a scope kept 20 rotations that follow the camera, but a write to any of them moved it 0
    (copies: the blender's `_CameraRotation`/`Old`/`JointOld`/`_RequestRotation`, `PrevCameraController._FPSActionInterpolation...`,
    `AimAssistController._CameraRotation`, the player's foot/IK rotations). A second test script wrote 8 more candidates in 3 phases
    (`UpdateBehavior`, `LateUpdateBehavior`, `PrepareRendering`): only **the `ADSCameraController`'s own GameObject transform** turned the
    camera (10 of 10 degrees, it stays); camera transform, player transform, `_MouseVec2` input: 0. A raw memory write at **`via.Transform`
    +0x40** (local rotation; parent `MainCameraControllers`, world == local) works the same; world yaw pre-multiplied, local X post-multiplied,
    **pitch sign -1** like the free-look camera (on screen: up). Pitch limits from `_OperateController` (+0x58, `app.CameraOperateController`)
    `<PitchRangeDeg>` (+0x40, via.Range; -50..50 seen through the scope, -60..60 others). `IsScope` (+0x9d) is true through a scope.
  - **FOV:** `via.Camera` +0x38, vertical (`get_VerticalEnable` true), read from the main blender's `_Camera` (+0xC8). Seen: 51 first person,
    40 third person, ~43 aiming without a scope (still `PlayerFPSCameraController`), 32 third-person aim, 25 / 11 through the scope (zoom steps).
    The blender's `_RequestFOV` (+0x1AC) holds the base FOV while aiming in first person, but follows the zoom at times (25 for frames after
    leaving the scope, 30/32/43 seen as "unzoomed"): the reference is the widest `_RequestFOV` each player camera (TPS, FPS) has settled at
    (|request - FOV| < 0.5), forgotten on loading (pause bit 0x1); aiming uses the player camera it came from. `ZoomReferenceFOV=` overrides.
    Every RE game's view now says zoom is available (models without FOV report none: compensation stays off).
  - **User-verified** (DualSense, REFramework installed; first build, reference = last `_RequestFOV`): gyro turns the view through the scope
    and with the laser-sight pistol and a normal gun, directions right, zoom compensation feels right (`gyro x0.202` at FOV 11/51,
    `x0.826` at 43/51, `x0.788` at 32/40). **Widest-settled reference user-verified** the same way (third person aim
    and scope, first person, laser pistol): unzoomed stays 40 (TPS) / 51 (FPS), 0 during loads, no more 25/43 references.
  - Tools: `methoddump.py` read TDB 81+/83 tables with old offsets (`TYPEDEF_SIZE` missing): it now takes the type's methods from the file;
    its base search uses the largest section when `.text` is a stub (Requiem: `.xtls`), but scored only 42 of 1673 there and the code is
    obfuscated: not usable on Requiem.
