# MODLOG: universal gyro aiming for any game (Gyro4All, formerly GyroUE)

Goal: DualSense gyro turns the real game camera in any single-player game (or multiplayer without anti cheat), without mouse emulation 
or gyro to stick.

## Findings

(Record exact class/function/property names, signatures, offsets, UE versions and what worked or failed.)

- 2026-10-08 **GyroLib accepts external samples**: `gl_register_endpoint(ctx, &gl_endpoint)` +
  `gl_submit_sample(ctx, endpoint, &gl_sample{sensor_ns, arrival_ns, gyro_dps, accel_g})`. Rules
  (docs/INPUT.md): endpoint ids nonzero and unique (max 32); motion qualifies after 2+ increasing sensor
  timestamps with arrivals ≤ 100 ms apart; stale after 150 ms. SDL reader: `gl_sdl_create(ctx, 0)`; the
  host must pump SDL events on the owner thread. Steam: `gl_steam_runtime_create` borrows the loaded
  `steam_api64.dll`.
- 2026-10-08 **Games that ship Sony's `libScePad.dll`** (Pacific Drive) already drive the DualSense
  natively. Alternative sensor source: hook `scePadReadState` + `scePadSetMotionSensorState` and feed
  `gl_submit_sample`, which avoids a second HID reader. Details in `games/PacificDrive/MODDING_PLAN.md`.

- 2026-10-08 **gyroprobe** (`tools/gyroprobe`, built with VS CMake against the GyroLib 1.2.2 SDK in
  `third_party/`, which is git-ignored): headless `SdlInput` sees "DualSense Wireless Controller" over USB,
  source SDL, ~490 accepted samples/s. Before calibration a stationary pad drifts ~0.6 deg/s
  (`calibration_state=0`), so the mod must calibrate (`gl_begin_calibration` / auto-calibration).
- 2026-10-08 **Pacific Drive + UE4SS v3.0.1-1028** (release zip, `dwmapi.dll` +
  `ue4ss\` in `PenDriverPro\Binaries\Win64`, settings: UE 4.27, console on, hot reload on):
  - AOB scan OK, except `ConsoleManagerSingleton` (2 candidates; only matters for console commands).
    `GameEngineTick` found. Lua mods start; GyroCamProbe loads.
  - **GUI console crashes the game at startup**: a stack overflow (AV write just below the stack limit)
    with UE4SS, `gameoverlayrenderer64` (Steam overlay), `d3d12` (SCSKiller proxy), ReShade `dxgi` and
    `nvoglv64` (the UE4SS OpenGL GUI) recursing on the stack. Fix: `GuiConsoleEnabled = 0`. The text
    console is fine (diagnosed from a Windows crash dump).
  - BPModLoaderMod created an empty `LogicMods` folder in the game's Paks.
- 2026-10-08 **Pacific Drive camera path (GyroCamProbe v1, first save, USB, Steam Input off)**:
  - PC = `BP_PlayerController_MainCharacter_C`, pawn = `PDP_MainCharacter_C` **both on foot and in the
    car** (no vehicle possession; at the main menu the pawn is `DefaultPawn`). PCM class = `PlayerCameraManager`, FOV 90.
  - **On foot:** camera rotation == `ControlRotation`. `SetControlRotation` moves the view in exact degrees
    (yaw +10 → +10, pitch +5 → +5). `AddYawInput(10)` → +25° (`InputYawScale=2.5`,
    `InputPitchScale=-2.5`). So on foot the plan's `SetControlRotation(GetControlRotation()+delta)` works.
  - **In the car:** `ControlRotation` still changes with both methods, but the camera doesn't move
    (the PCM rotation stays at the car heading ± small head motion). The car view uses its own rotation
    (camera component / free-look variable), so it needs a per-game "InCar" view → probe v2 (F5/F11
    snapshot diff while looking around with the right stick).
  - `bShowMouseCursor=false`, `IsLookInputIgnored=false`, `Paused=false` in gameplay (pause menu not tested yet).
  - F6 works but only writes to the log (shipping build: nothing on screen).
  - Probe keys to avoid: **F11** (windowed/fullscreen toggle). F10 was Steam's screenshot key; the user
    moved it in Steam on 2026-10-08, so F10 is free now.
- 2026-10-08 **Pacific Drive car camera (probe v2 snapshot diff, right-stick look in the car):**
  - In the car: view target = attach parent = **`BP_PlayerCarNew_C`**. Active camera:
    `BP_PlayerCarNew_C.FirstPersonCamera` (CameraComponent).
  - Stick look writes **`CameraControllerRotation_Target`** (FRotator on the car). It is smoothed into
    **`CameraControllerRotation_Current`**, which is copied into `FirstPersonCamera.RelativeRotation` (and
    `LastUsedRotationCameraRotation`, `3rdPersonSpring.RelativeRotation`).
  - The yaw seems clamped at **±140°** (stick held at the limit gave exactly 140.000).
  - Auto-recentering: `IsAutoControlling` 1→0, `Camera_AutoControlAmount_current/_target` 1→0 while the
    player looks; `TimeNotSteering` (float) keeps counting. These probably bring the view back to the
    front when the player stops looking.
  - Only yaw was tested; pitch in the car is unknown.
  - Probe v3 (F7/F9 in the car add to `CameraControllerRotation_Target`): **direct writes work**.
    Yaw +10 and pitch +5 land exactly, `_Current` follows within ~50 ms, `FirstPersonCamera` turns, and
    the value holds for 3+ s (no recentering while parked; `IsAutoControlling` stays false). The BP clamps
    yaw at **140** (writes past it stay at 140.00). Stick look-up also writes the Target pitch. Pitch
    limits are not measured yet (seen -19.8 to +11.5).
  - Recentering while *driving* (`TimeNotSteering`, `Camera_AutoControlAmount`) is not tested yet.
    It may pull a gyro-moved view back to the front. Check it in the C++ mod.
- 2026-10-08 **Coexistence confirmed by user**: with gyroprobe's SDL reader running, the game's native
  DualSense haptics and adaptive triggers kept working (USB, Steam Input off). So SDL is OK as the sensor
  source for Pacific Drive, and the libScePad hook fallback isn't needed for now.
- 2026-10-08 **GyroLib settings** (`gyroprobe --settings` lists every id, range and choice). Why
  calibration never ran: `calibration.automatic` defaults to **0 = Off** (1 = menus only, 2 = any time).
  The panel key `ui.menu_key` defaults to **F10** (free since the user moved Steam's screenshot key).
  The pad shortcut Back+Start = DualSense Create+Options; check it doesn't also trigger game actions.
  Settings that need host support:
  - `camera.recenter_button` → recenter callback;
  - `flick.*` → suppress the game's right stick;
  - `gyro.zoom_compensation` → FOV (`GetFOVAngle`).
- **Pacific Drive apply rule** (draft profile `profiles/pacific-drive.json`):
  - pawn attached to a `BP_PlayerCarNew_C` → add the deltas to
    `car.CameraControllerRotation_Target`;
  - otherwise → `SetControlRotation(GetControlRotation() + delta)`.
  - Degrees are real in both paths, so no `InputYawScale` conversion is needed.
- 2026-10-08 **SDL + libScePad coexistence (gyroprobe, 11 min alongside gameplay):** GyroLib's SDL reader got
  continuous DualSense motion (~460 samples/s, 6 rejected, 0 dropped, no errors) while the game ran with
  its native DualSense. `calibration_state` stayed 0 all the time, so auto-calibration doesn't kick in on
  its own; the mod must call `gl_begin_calibration` (or check the settings). Effect on haptics/triggers:
  pending user report.

- 2026-10-08 **GyroUE v0.1 works in Pacific Drive** (user confirmed: on foot and in the car; stops in menus/pause).
  - Standalone `version.dll` proxy (`src/GyroUE`); UE4SS disabled (`dwmapi.dll.off`). Gyro frame runs from an
    IAT hook of `PeekMessageW` on the game thread.
  - Shape-scan found GUObjectArray (exe+0x54722B0, item stride 0x18) and FNamePool.Blocks (exe+0x5435D10).
  - Detected layout matches UE4SS: SuperStruct 0x40, ChildProperties 0x50, FField.Next 0x20, Name 0x28,
    ElementSize 0x3C, Offset_Internal 0x4C, sizeof(FProperty) 0x78. Engine class `DrivingGameEngine`.
  - The first build failed to detect `sizeof(FProperty)` because the size loop started unaligned and skipped 0x78.
  - Main menu: PC `BP_MainMenu_PlayerController_C`, pawn `DefaultPawn`, cursor=1 → camera off.
    Pause: `WorldSettings.PauserPlayerState` set (paused=1).
  - `gl_diagnostics.calibration_state` stays 0. That field seems to track *manual* calibration only. The status line
    now logs `bias`/`stationary` to check whether automatic menu calibration learns a bias.
  - Settings panel: v0.1 disables F10/Create+Options on purpose, because there's no frontend yet.
    `gyrolib.ini` edits reload live (1 s poll). An in-game panel needs GyroLib's DX12 overlay:
    - hook `IDXGISwapChain::Present` and capture the DIRECT `ID3D12CommandQueue`;
    - forward window messages;
    - gate game input while the panel is open.
    That's risky next to ReShade + SCSKiller + Steam overlay (the UE4SS GUI already crashed there).

- 2026-10-08 **GyroUE v0.2: in-game settings panel works** (GyroLib DX12 overlay, F10 / Create+Options).
  - It's gated by `gyrolib.ini`: if `ui.menu_key=` is empty and `ui.gamepad_menu_shortcut=0`, no graphics hooks
    are installed (read in DllMain before the renderer starts).
  - Hooks (all inside our own module, no inline patching):
    - IAT of `CreateDXGIFactory`/`CreateDXGIFactory1`;
    - factory vtable: CreateSwapChain (10) and CreateSwapChainForHwnd (15), which capture the swapchain and
      its DIRECT queue;
    - swapchain vtable: Present 8, ResizeBuffers 13, Present1 22, ResizeBuffers1 39;
    - WndProc subclass that forwards to `gl_overlay_win32_message`.
  - Pacific Drive with ReShade + RenoDX + SCSKiller + Steam overlay: swapchain captured about 0.7 s after
    start, panel ready, no crash. Without a renderer (non-DX12), GyroUE force-closes the panel so it can't
    hold the gyro invisibly.
  - Automatic calibration ("Menus only") works: bias learned at the main menu, (0.10 -0.08 -0.25) deg/s.
  - Known limitation: while the panel is open, gamepad input still reaches the game (only mouse/keyboard are gated).

- 2026-10-08 **GyroUE v0.2 user test in Pacific Drive: everything works, no crashes.**
  - The panel saves to `gyrolib.ini` and GyroUE reloads the file each time (9 reloads, all `result 0`).
    User settings now:
    - `ui.language=es`;
    - on foot: `gyro.activation=4` (toggle), `gyro.smoothing_ms=10`;
    - in car: sensitivity 2.0 X/Y, which is a local exception; the rest is inherited (`context.2.inherit=1`).
  - Toggle mode works (`enabled` flips 1/0) in both views. Car/foot switching is correct over repeated
    entries and exits. The automatic menu calibration learned bias (0.24 -0.19 -0.65) deg/s, which
    matches gyroprobe's drift.
  - Correction: the panel's "Inherit settings from: None" was the On-foot tab (the root view); the car does inherit.
- 2026-10-08 **GyroUE v0.2.1**: no gameplay view while the pawn is a `DefaultPawn` (main menus; SpectatorPawn
  derives from it). v0.2 applied a tiny −0.1/−0.9° to the main-menu camera when the cursor was hidden.
  Verified: main menu → `view=none`, applied 0.

- 2026-10-08 **Bug (user): in the car, while driving (R2), the gyro stops moving the view after a while; a right-stick
  look brings it back.** It doesn't happen parked. Likely cause: the car BP's camera auto-recentering
  (`IsAutoControlling`, `Camera_AutoControlAmount_*`) engages while moving without stick look input and drives
  `CameraControllerRotation_Target` back to the front every tick. The stick path cancels it; our property write doesn't.
  - GyroUE v0.2.2 adds generic profile keys per view:
    - `OnGyro=Prop:value,...`: scalar writes on the view object whenever the gyro moves the view;
    - `Watch=Prop,...`: log on change;
    - `Dump=1`: log all properties once.
    These are supported for Float, Double, Int, Byte and Bool properties.
  - Pacific Drive InCar: `OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0,Camera_AutoControlAmount_current:0`
    (the values the game sets on stick look). **Fixed, user-verified while driving.** The watch log showed
    the BP re-arming auto-control 6 times while driving (`IsAutoControlling=true`,
    `Camera_AutoControlAmount_target=1`), and each time OnGyro cancelled it about 3 ms later. Releasing
    the gyro still lets the game recenter normally.
  - Car camera properties from the Dump (BP_PlayerCarNew_C, 4.27, float rotators):
    - `CameraControllerRotation_Current/_Target` @0xAF4/0xB00;
    - `CameraControllerRotationSpeed` 55 (7 while snapping);
    - `CameraYawMinMaxAngles` / `CameraPitchMinMaxAngles` (Vector2D);
    - `IsAutoControlling` @0xB68;
    - `AutoRecenteringSpeedSlow/Fast` 0.3/1.3;
    - `AutoControlRotCurrent/Target`;
    - `TimeSteering` / `TimeNotSteering`;
    - `Camera_AutoControlAmount_current/_target` @0xC28/0xC2C.
  - Diagnostics are disabled again in the profile; only OnGyro stays.

- 2026-10-08 **Second game: Returnal (Steam 1649240), UE 4.25.1 licensee, MODULAR build** (264 `Returnal-*-Win64-Shipping.dll`).
  - No anti-cheat found (EOS SDK present for co-op). It ships `libScePad.dll` (native DualSense) and has ReShade
    (`dxgi.dll`, RenoDX UE addon), SCSKiller (`d3d12.dll`) and Streamline (DLSS) modules.
  - Saves backed up before testing.
  - **GyroUE v0.3** makes the DLL work for modular builds:
    - every import hook applies to the exe + all `<Project>-*.dll` modules, and modules loaded later are
      patched from hooked `LoadLibrary*`;
    - memory scans cover all game modules.
    Returnal: `version.dll` is imported by Core, `PeekMessageW` by ApplicationCore/Core, and DXGI by
    DXBridge/StreamlineD3D12RHIPreInit (loaded later).
  - **Result without any profile or game-specific code:**
    - GUObjectArray found in `Returnal-CoreUObject…dll+0x2F6D28`, FNamePool in `Returnal-Core…dll+0x5C2950`;
    - same 4.25+ layout as Pacific Drive; engine `GameEngine`;
    - PC `BP_PlayerController_C`, pawn `BP_PlayerCharacter_C`; the generic ControlRotation view is active;
    - menu calibration learns a bias.
  - **Panel bug found and fixed (v0.3.2):** Returnal calls `ResizeBuffers` on a different thread than
    `Present` (RHI thread 9528 vs 13112). GyroLib only accepts render calls on its one render thread, so
    `before_resize` returned -1. The panel kept its backbuffer references, ResizeBuffers failed, and the
    image froze on the black loading screen (audio, vibration and gyro kept running).
    Fix: while the panel is closed, release GyroLib's backbuffer references at the end of every frame on the
    render thread (nothing is drawn, so it costs nothing). Resizes from other threads are safe now. Still
    open: a resize from another thread *while the panel is open* would still fail (it's logged).
    v0.3.2 verified: loads into gameplay; F10 panel opens (single "Camera" tab) and closes.
  - Generic installer: `tools/deploy-gyroue.sh "<Win64 dir>" [remove]`. It never overwrites `gyrolib.ini` and
    refuses to replace a foreign `version.dll`.
  - **User gameplay test (v0.3.2): works, no crashes.**
    - About 7 min in combat with the generic ControlRotation view;
    - pause/menus gate the gyro (`paused=1 cursor=1` → `enabled=0`);
    - menu calibration bias (0.17 -0.15 -0.62) deg/s.
    The user configured the panel: `gyro.activation=3` (hold to enable) with `activation.trigger=3`
    (L2 or R2, threshold 0.2), so the gyro is active only while aiming/shooting. Triggers read through SDL
    work while the game reads the pad through libScePad. Haptics and adaptive triggers: OK (user).
  - **Panel shortcuts in Returnal:**
    - A physical F10 didn't open the panel: F10 is a system key, and the game doesn't deliver it as a normal
      WM_KEYDOWN. My SendInput test had worked.
    - Create+Options opened the panel and closed it again at once: Options also opens the game's pause menu.
      GyroLib closes the panel when owner snapshots are older than 250 ms, probably because of the hitch
      when the pause menu opens.
    - **v0.3.3:** GyroUE polls the configured F-key (`GetAsyncKeyState`) on the game thread, toggles the
      panel with `gl_overlay_set_open`, and no longer forwards that key's messages to GyroLib (so it can't
      toggle twice). Returnal's `gyrolib.ini` now has `ui.gamepad_menu_shortcut=0`.
    - Verified F10 opens/closes the panel in Returnal (single Camera tab, the user's L2/R2 hold activation shown).
      Physical F10 confirmed by the user, **but only from the game's menus/pause; during gameplay F10 doesn't
      open the panel**. The user accepts this. The cause isn't diagnosed: the log has no panel-state lines.
      If needed, log `gl_panel_open` / `gl_overlay_capture` on each toggle request to see whether GyroLib
      refuses to open or closes the panel immediately in gameplay.
    - Panel edits in the pause menu saved and reloaded correctly (several `gyrolib.ini changed, reloaded (0)`).

- 2026-10-08 **GyroLib updated to 1.3.0** (commit b241b7d; SHA256SUMS verified for the SDK, the demo and
  `gyrolib.dll` 04f32087…). It's a drop-in: ABI 1 and INI schema 0.2.0 are unchanged. Relevant fixes:
  - "Recalibrate gyro" works (the panel used to revert it);
  - manual calibration is more tolerant and is saved per SDL sensor across restarts;
  - Steam mouse Block/Convert fixes.
  New host rule: report menu state independently of camera permission. GyroUE already does
  (`menu_open` = cursor; `camera_allowed` is separate).
  - SDK version pinned in `tools/gyrolib-sdk.sh`, which is used by `tools/build-gyroue.sh` (reconfigures when
    the pin changes) and both deploy scripts. gyroprobe was rebuilt too.
  - GyroUE v0.3.4 logs the SDK it was built with. Deployed to Pacific Drive and Returnal; Returnal smoke test
    OK (settings kept, panel ready, Camera view).
  - `third_party/GyroLib-1.2.2` deleted (unused).

- 2026-10-08 **Performance:** benchmarks and per-frame cost analysis live in `bench/BENCHMARKS.md`.
  Summary: no measurable FPS/frametime impact in Returnal (GPU-bound).


- 2026-10-08 **Third game: Cronos: The New Dawn (Bloober Team), UE 5.4.4, monolithic** (`Cronos\Binaries\Win64`).
  - GyroUE 0.3.6 with no profile: GUObjectArray at exe+0xAD91670, FNamePool at exe+0xACADED0, same layout as 4.25+
    but FField.Next 0x18 / Name 0x20 / ElementSize 0x34 / Offset_Internal 0x44 / sizeof(FProperty) 0x70 (UE 5.4).
    Engine `GameEngine`. ReShade + RenoDX present; DX12 panel works (user).
  - Main menu: PC `PlayerController`, pawn `DefaultPawn`, view target `CameraActor` → `view=none` (correct).
  - Gameplay: PC `SHPlayerControllerPlay_BP_C`, pawn/view target `CronosCharacterPlay_BP_C`. The generic view writes
    `ControlRotation` (applied yaw reached -220°) **but the camera doesn't move**. The game's camera uses another
    rotation (the PC class prefix `SH` = Bloober's Silent Hill 2 Remake framework).
  - GyroUE 0.3.7 adds a generic probe (`[GyroUE] Probe=1`): F7 snapshots every float/double/rotator (nested structs
    up to 3 levels) of PC, pawn, PlayerCameraManager, view target and every object whose Outer chain reaches them
    (components); F8 logs what changed.
  - **Probe result (right-stick look, 2 runs):** the stick does **not** change `ControlRotation` (only the gyro's
    ~0.6°). Changed rotators, all outputs: `SHPlayerCameraManagerPlay.ViewTarget/CameraCachePrivate/
    LastFrameCameraCachePrivate.POV.Rotation`, the PCM's `TransformComponent0.RelativeRotation`,
    `Animation(SHCharacterPlayAnimComponent).LastAimRotation`. Also `SpringArm(SHCharacterPlaySpringArmComponent)
    .TargetArmLength` (180 → 102 when looking up), anim `AimOffset.Y` = pitch, `CameraYawDifference`.
    So the look state is non-reflected C++ (probably inside `SHPlayerCameraManagerPlay`).
  - GyroUE 0.3.8: the probe also diffs the raw memory of every probed object (up to `PropertiesSize`, read at
    ChildProperties+8) and logs aligned floats/doubles that now equal the POV yaw/pitch (deg or rad) or changed by
    the POV delta.
  - **Raw probe result (2 runs, yaw +148/+179, pitch +55/+61):** most matches are output copies (PCM +0x380/0x1450/
    0x1C90 = POV, component transforms, anim). The look state candidate is the pawn's
    **`View` (`SHCharacterPlayViewComponent`)**: two non-reflected **double rotators at +0x2A0 and +0x420** (P, Y, R)
    that equal the POV in both runs. `SpringArm`+0x2D8 and `Animation`+0x1C0 also match (world rotation / LastAim).
  - GyroUE 0.4.0: views can target any object and raw offsets. Profile keys `Object=Root.Sub...` (Root = PC, Pawn,
    Camera, ViewTarget; each Sub is an object property or a subobject name, cached), `Offsets=0x..,0x..`,
    `RotatorSize=24|12`. The status line logs the raw rotators. Cronos profile: `Object=Pawn.View`,
    `Offsets=0x2A0,0x420` (both get the delta), `Dump=1`.
  - **User test (0.4.0): works, "perfectly acceptable".** About 3 min of gameplay. Gyro turns the camera and
    aim; the right stick still works; pause gates the gyro. Both offsets always hold the same value. While paused
    they read (P 0, Y -115) (not used). The Dump of `View` printed no reflected float/rotator properties.
  - Calibration gap: the Cronos main menu and pause menu show no cursor with a gamepad (`cursor=0`), so
    `menu_open` was false and "menus only" auto-calibration rarely ran (bias stayed 0 most of the session; the
    panel's ini reloads also reset it to 0). **GyroUE 0.4.1:** `menu_open = !pc || cursor || paused || no view`
    (generic). Profile cleaned up (Probe/Dump off). Pacific Drive/Returnal still run 0.3.x.
  - **0.4.1 test:** calibration fixed. Bias learned in the main menu with `cursor=0` within 10 s
    ((0.15 -0.14 -0.57) → (0.17 -0.16 -0.64)), thanks to "no gameplay view". The pause menu shows `cursor=1` only
    after a keyboard key is touched.
  - **New bug: the F10 panel didn't appear in that session** (it worked with 0.4.0). The GyroUE log has nothing on it,
    because F10 requests, panel state and render results weren't logged. ReShade logged a
    `ResizeBuffers(2, 3840x2160, fmt 24)` at the main menu on the **game thread 34404** (render thread 31108). The
    panel was closed and buffers released, so it should be safe, but the next render must reacquire buffers.
    GyroUE 0.4.2 logs F-key requests, panel open/closed, panel drawn/hidden, render results (and re-inits the renderer
    once on a render error), ResizeBuffers HRESULTs and Presents on uncaptured swapchains.
  - User hint: the panel worked while the game's HDR was **off**; with HDR on, the **RenoDX** add-on (ReShade) takes
    over. Hypothesis: RenoDX upgrades the swapchain format behind the game's back (the game passes RGB10A2 = 24 to
    ResizeBuffers, the real buffers become RGBA16F), so GyroUE saw "no format change", skipped the required GyroLib
    shutdown/init, and the panel stopped drawing silently. **GyroUE 0.4.3:** after every ResizeBuffers(1) it reads
    the real `GetDesc` count/format and re-inits the panel renderer if they differ (logged as "real buffers ...").
    Tested with HDR on.
  - **0.4.3 log disproves the format hypothesis**: every ResizeBuffers keeps 2 buffers, format 24 (real = requested;
    ReShade also sets `SetColorSpace1(12)` = HDR10). **Real cause: UE moved Present to another thread.** The panel
    was initialized on thread 10864. After the 3rd ResizeBuffers (game thread 33312, 28 s after start), Present came
    from thread 33348 and `gl_overlay_dx12_render` returned -1 "render requires initialized render thread". 0.4.2's
    retry made it worse: shutdown from the new thread failed too (-1), then init failed with "Overlay already
    initialized/detached", so the panel was disabled for the session (F10 requests then returned -2). Thread 10864 was
    still alive (it called SetFullscreenState at exit). This is probably also why F10 didn't work in Returnal gameplay.
  - **GyroUE 0.4.4: render worker.** Every GyroLib render call (init/render/before_resize/shutdown) runs on GyroUE's
    own thread. The presenting or resizing thread hands the job over and waits, which keeps the panel drawn before
    the game's Present, whatever thread UE uses. While the panel is closed and holds no buffers, Present doesn't call
    GyroLib at all (zero per-frame cost). The cross-thread resize problem (Returnal) is handled the same way. A worker
    stuck for 5 s disables the panel. The 0.4.2 render-error retry was removed.
  - **0.4.4 test: F10 still didn't open.** The worker initialized fine, no render errors, and every F10 request
    returned 0, but `gl_panel_open` never became 1. Cause: the "skip GyroLib while closed" optimisation. GyroLib
    refuses/closes the panel when the render side is stale (> 250 ms, see overlay.h). **Rule: call
    `gl_overlay_dx12_render` every frame, even while closed.** 0.4.5 renders every frame through the worker again.
  - **0.4.5 user test (HDR on, RenoDX, ReShade): panel works in menus and in game; no stutter with it closed.**
    Log: 3 ResizeBuffers (all format 24), then 5 open/close cycles in the main menu and in gameplay
    (`panel state open` → `panel drawn (capture 7)` → hidden/closed), one ini save reloaded (0).
    Present hook cost with the panel closed (worker handoff + GyroLib render + buffer release): avg 16–20 µs,
    p99 29–42 µs, about the same as 0.3.x in Returnal. With the panel open, Present averages 4–7 ms (p99 ~15 ms),
    the same as before the worker (0.3.6 showed 6–11 ms open). That is GyroLib's own panel draw/fence wait, not a regression.
  - Fallback if a game still breaks the panel (user's idea): detect the failure, not the mods. Disable the panel for
    the session with a clear log line ("configure via gyrolib.ini, reloads live"); optional profile `Panel=0`;
    README section. Not needed for Cronos.

- 2026-10-08 **README.md** written for GitHub: contents, install, profiles, a probe walkthrough, GyroLib MIT notice and a
  third-party table. `tools/deploy-gyroue.sh` now ships `GyroUE/licenses/` (the GyroLib SDK licenses,
  `SDL-CHANGES.md`, `GyroUE-LICENSE.txt`), as GyroLib's THIRD_PARTY.md requires. `LICENSE.txt` still has the template
  placeholders (`[year] [fullname]`).
- 2026-10-08 **GyroUE 0.4.6:** probe lines are labelled with the profile path of each object
  (`Pawn.View(SHCharacterPlayViewComponent)`, `Camera.TransformComponent0(SceneComponent)`). The roots are tried in
  the order PC, Pawn, Camera, ViewTarget, so the part before "(" can be copied into `Object=`. Builds; not run in game.
- 2026-10-09 **Gears of War: E-Day** (Xbox/Game Pass GDK build, UE 5.6 non-modular per UE4SS log).
  Exe `<Xbox games folder>\Gears of War- E-Day\Content\FairlightConcept\Binaries\WinGDK\GoWEDay.exe` (encrypted/ACL'd:
  pefile/dumpbin can't read it, so its import table is unknown). Already present: UE4SS as `dwmapi.dll`, ReShade as
  `dxgi.dll` (+ RenoDX `renodx-mfgunlock.addon64`), `EACbegone.bat`.
  - User report: default install (`version.dll`) wrote no log; renamed to `winmm.dll` it failed with "entry point
    `timeGetDevCaps` not found".
  - Cause 1 (no log): the DllMain gate only accepted `*-Shipping` / `*-Win64-*` exe names and returned before
    `log_open`, so `GoWEDay.exe` was silently skipped. Whether the exe imports `version.dll` is still unverified.
  - Cause 2 (winmm): the DLL only exported the 17 version.dll functions; the exe statically imports winmm.
  - **GyroUE 0.4.7:** gate also accepts any exe whose folder's parent is `Binaries` (`Binaries\WinGDK\GoWEDay.exe`).
    The proxy exports version.dll + all 180 named winmm.dll exports (`proxy.def`, was `version.def`) and forwards to
    `System32\<own file name>`, so the same DLL works as `version.dll` or `winmm.dll`. Out-of-game test: as winmm,
    `timeGetDevCaps`/`timeGetTime` work; as version, `GetFileVersionInfoSizeW` works and a winmm export returns
    `ERROR_PROC_NOT_FOUND`. `deploy-gyroue.sh` takes `PROXY=winmm.dll`, finds renamed exes (largest exe in the dir)
    and removes our proxy under the other name. Deployed to Gears as `winmm.dll`; **not yet run in game**.
- 2026-10-09 **GyroUE 0.4.8:** the exe-name/location gate is gone (it only ever filtered exes in the DLL's own
  folder, and it silently skipped renamed exes). Instead a mutex `Local\GyroUE-<hash of the lowercased install
  folder>` makes the first process that loads the proxy the only one running GyroUE; later processes are plain
  forwarders (no pad, hooks or log). Out-of-game test with two processes: the second sees the mutex and doesn't
  attach, forwarding still works, the mutex is gone once both exit. Deployed to Gears as `winmm.dll`; not run in game.
- 2026-10-09 Gears 0.4.8 log: GyroUE loads as `winmm.dll` (log written, game thread = DllMain thread), but
  `hook: PeekMessageW not imported by any loaded game module yet` and nothing after it: no gyro frame ever ran, so
  GyroLib never initialized (no `gyrolib.ini` created). UE4SS says the build is non-modular (MainExe only), so the
  exe itself doesn't import `USER32.dll!PeekMessageW` by that name. Hypotheses: API-set import names
  (`ext-ms-win-ntuser-message-*`, GDK/OneCore linking), a stripped name table, or SecureEngine (`SecureEngineSDK64.dll`,
  Themida/WinLicense) resolving imports itself.
  - **GyroUE 0.4.9:** at startup the log dumps the exe's import table (each DLL: slots/by name/by ordinal/no name
    table) and every message-pump import (Peek/GetMessage, MsgWait..., DispatchMessage) with where its slot points,
    also matched by user32's real address. Hooks now accept API-set DLL names (`api-ms-*`/`ext-ms-*`) and, without a
    name table, match slots by the resolved address. Tested on a small exe (PeekMessageW found and hooked).
    Deployed to Gears; waiting for a run.
  - **0.4.9 Gears run: the exe's PE import directory is a protector stub.** Import directory at RVA 0xfbe4795
    (unaligned, at the end of the 0x12ee5000 image), 52 DLLs with **1 slot each and no name table** (USER32, dxgi,
    WINMM, VERSION, ...). That only forces the DLLs to load; the real imports are resolved later by the protector
    (SecureEngine/Themida-style, `SecureEngineSDK64.dll` ships with the game) into its own table, after our DllMain.
    So no IAT hook can work in this exe: not PeekMessageW, not the LoadLibrary hooks, not the DXGI factory hooks
    (the F10 panel won't attach either). Note: VERSION.dll **is** in the stub import list, so `version.dll` would
    also have loaded (the original no-log symptom was only the name gate).
    Options: (1) inline hook of user32!PeekMessageW (and dxgi exports) as a fallback when the IAT hook finds nothing
    (MinHook, BSD-2); UE4SS and ReShade already patch code in this process. (2) scan the protector's table after
    unpacking and patch pointers there (may not exist if calls go through stubs; writes into protected memory).
    (3) a thread timer on the game thread (no patching, but WM_TIMER is limited to ~10-16 ms, so not every frame).
  - **GyroUE 0.5.0 (user chose option 1):** MinHook **1.3.4** (commit c3fcafdc, BSD-2) cloned into
    `third_party/minhook-1.3.4` (git-ignored like all of third_party; re-fetch with
    `git clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook third_party/minhook-1.3.4`), built as
    a static lib in `src/GyroUE/CMakeLists.txt`. `hooks::init` detects the stub (imports present, no name tables) and
    switches to inline mode: every `hooks::add` with a named DLL hooks that DLL's export with MinHook (`*original` =
    trampoline); LoadLibrary hooks (dll null) are skipped (only needed by modular builds). `GYROUE_INLINE=1` forces
    the mode. Log: `hooks: ... import-table hooks|inline hooks on exports`, `hooks: inline X!F at module+off: MH_OK`.
    Out-of-game test (small exe, forced): inline PeekMessageW MH_OK and the gyro frame ran. Releases ship
    `licenses/MinHook-LICENSE.txt`. Deployed to Gears as winmm.dll; **not yet run in game**. Open risks: the protector
    may detect/undo patches in user32/dxgi (ReShade's dxgi.dll here), and the game thread is assumed to be the DllMain
    thread.
  - **0.5.0 Gears run (02:08, ~3.5 min, per UE4SS/ReShade logs): no GyroUE.log at all** (the file stayed the 0.4.9 one
    from 01:56), mod and panel inactive. `winmm.dll` was 0.5.0. GyroUE opens its log right after the instance mutex,
    so either the DLL never ran DllMain or the mutex already existed. Main hypothesis: the 01:56 instance hadn't fully
    exited and still held `Local\GyroUE-<hash>`. Nothing in the Application event log; no process had WinGDK modules
    loaded afterwards, so it can't be checked post hoc.
  - **GyroUE 0.5.1:** every process that loads the proxy appends one line to `GyroUE\GyroUE-boot.log` (time, version,
    pid, thread, exe, outcome: runs GyroUE / another process already runs it / mutex failed) before the mutex decides.
    Restarts past 64 KB. Tested with two processes. Deployed to Gears.
  - The 02:18 run had no boot log either; the user had deleted `GyroUE.log` by hand before launching. Launched again
    at 02:25 (0.5.1): `runs GyroUE`. So the two silent runs (02:08, 02:18) remain unexplained; watch the boot log.
  - **0.5.1 Gears run (02:25): inline hooks work.** `imports: ... no name tables`, `inline hooks on exports`,
    MH_OK on `USER32!PeekMessageW` and ReShade's `dxgi.dll!CreateDXGIFactory/1/2` (+0x17be70/+0x17be80/+0x17c320).
    The gyro frame ran on the game thread (9412 = "GameThread" in the dump): GyroLib ABI 1, `gyrolib.ini` created
    (menu key F10, gamepad shortcut 1), SDL reader started, `motion=1 samples=61`. FNamePool found at
    `GoWEDay.exe+0xEBA1CD0`. `view=none pc=` at the front end (UE4SS sees `FairlightPlayerController_FrontEnd_BP_C`).
    First host frame took 2.43 s (GyroLib/SDL init).
  - **Crash 02:26:36:** AV in `C:\WINDOWS\system32\dxgi.dll+0x2c459` (read [rbx+0x128], list walk), game thread,
    dump `%LOCALAPPDATA%\CrashDumps\GoWEDay.exe.1052.dmp`. Stack: GoWEDay -> `sl_interposer` (NVIDIA Streamline) ->
    Streamline plugin (`slGetPluginFunction`) -> **our inline `CreateDXGIFactory*` hook** -> `RTSSHooks64` (RivaTuner)
    -> system dxgi. Same DLL stack ran fine with 0.4.x import hooks (which did nothing here), so the inline hooks on
    the factory exports (four layers chained on them: Streamline, ReShade, RTSS, us) are the cause.
  - **GyroUE 0.5.2:** in inline mode the overlay no longer hooks dxgi exports. A thread started from DllMain (runs
    after the loader lock) loads `System32\dxgi.dll`, creates its own factory and patches that **real factory's
    vtable** (CreateSwapChain 10, CreateSwapChainForHwnd 15): the wrappers all call it with the real queue, and the
    real swapchain's Present is hooked as before. The build now emits `build/GyroUE/Release/version.pdb` (`/Zi`,
    `/DEBUG /OPT:REF /OPT:ICF`; not shipped) for dumps. Test: D3D12 sample app with `GYROUE_INLINE=1` -> factory
    vtable hooked, swapchain captured, `DX12 panel ready`. Deployed to Gears.
  - **0.5.2 Gears run (02:32): no crash; F10 panel works** (user-verified: opens/closes in game). Log: system factory
    vtable hooked, `captured D3D12 swapchain`, ResizeBuffers(6, 3232x1888, format 24), `DX12 panel ready (6 buffers)`.
    Auto-calibration ran (bias (0.13 -0.18 -0.69)). Gyro not applied: `view=none pc= pawn=` for 3 minutes, because
    **GUObjectArray was never found** (only `ue: FNamePool.Blocks at GoWEDay.exe+0xEBA1CD0`), so `ue::init()` fails
    every second. UE4SS also fails several AOBs here (GMalloc, FName::ToString, FName ctor) but its object cache works.
  - **GyroUE 0.5.3:** the GUObjectArray scan also tries strides 0x28/0x30, and after ~10 failed attempts also scans
    writable+executable sections (protectors may merge .data into RWX sections), every 10 s. The first time it logs
    the exe's section table and up to 8 header-valid candidates with their first items (object, InternalIndex,
    vtable module). Deployed to Gears; not yet run.
  - **0.5.3 Gears run (02:39):** exe sections: unnamed R-X (rva 0x1000, 0xAD9EA3B), R--, **RW- data at rva 0x0E497000
    (0x956D08)** (FNamePool at +0xEBA1CD0 is in it), small RW/R sections, .debug, .edata, .idata RW, .tls, .rsrc,
    **.themida RWX (rva 0x0FBF7000, 0x206C000)**, **.boot R-X (rva 0x11C63000, 0x1280800)**, .reloc. Themida-protected.
    Not a single object-array header candidate anywhere, RWX included.
  - Explanation found in `ue4ss/UE4SS_Signatures/GUObjectArray.lua` (a custom signature someone wrote for the Steam
    build; anchored in UObjectBase::AddObject): "The global holds an XOR-encoded FUObjectArray pointer, not an inline
    array." Pattern there: `0F BA EF 19 83 E0 FB 89 46 ?? 48 8B 0D <rel32> 48 BB <imm64 key> 48 33 CB 75 ?? B9 B8 00 00
    00` (mov rcx,[global]; mov rbx,key; xor rcx,rbx; jnz; mov ecx,0xB8 = sizeof(FUObjectArray) for the lazy
    allocation). UE4SS 3.0.1 failed with it on the Xbox build too ("Was unable to find AOB for 'GUObjectArray'"), and
    also fails GMalloc, FName::ToString/ctor, StaticConstructObject, FUObjectHashTables, GNatives, ConsoleManager,
    GameEngineTick AOBs.
  - **GyroUE 0.5.4:** `ue.cpp` scans the executable sections once (VirtualQuery, committed readable pages, SEH-guarded)
    for the generic idiom `REX.W 8B /r [rip+disp32]` -> `REX.W B8+r imm64` -> `REX.W 33/31` (any registers), keeps up
    to 8192 unique (global, key) pairs, and on each retry decodes `*global ^ key` and accepts it only if a valid
    ObjObjects header + 16 live items (InternalIndex, vtable in a module) sits within the first 0xB8 bytes. Logged as
    `ue: N XOR-encoded pointer loads ...` and `ue: FUObjectArray <p> decoded from GoWEDay.exe+X ^ key (code at ...);
    ObjObjects at +0xNN`. Header check refactored into `object_array_stride()`. Deployed to Gears; not yet run.
  - **0.5.4 Gears run (02:48): `ue: 0 XOR-encoded pointer loads in the game code (109 ms)`.** Either that code is
    virtualized/compiled differently in the Xbox build, or the scan didn't cover the code (no stats logged then).
  - **GyroUE 0.5.5:** the code scan logs MB scanned, regions, unreadable regions and faults. New last resort, independent
    of code: a background thread scans every committed `MEM_PRIVATE` `PAGE_READWRITE` region (raw prefilter on
    Max/Num/MaxChunks/NumChunks, then the full `object_array_stride()` check), at most every 30 s and 10 times; the
    game thread picks the result up in `ue::init()`. `module_offset()` prints `heap 0x...` for non-module addresses.
    Test: a fake heap FUObjectArray (5000 objects, stride 0x18) is found at the exact address. Deployed to Gears.
- 2026-10-09 **GyroUE 0.5.6:** the DLL carries a Windows version resource (`src/GyroUE/version.rc.in`, configured by
  CMake from project(), so the version still lives only in CMakeLists.txt): FileDescription "GyroUE - gyro aiming for
  Unreal Engine games", ProductName GyroUE, File/ProductVersion, InternalName, OriginalFilename version.dll,
  LegalCopyright "MIT License" (LICENSE.txt still has the [year] [fullname] placeholders), Comments (removed in 0.5.7). Verified with
  Get-Item VersionInfo. Not deployed to Gears (the user is testing 0.5.5 renamed to version.dll there).
- 2026-10-09 **GyroUE 0.5.7:** Comments field removed from the version resource (user request). Not deployed to Gears.
- 2026-10-09 **Gears of War: E-Day WORKS (user-verified, 0.5.5 renamed to `version.dll`, run 19:22-19:28): gyro turns
  the camera and the F10 panel works.** Pad/connection/Steam Input state not reported.
  - 0.5.5 as `winmm.dll` at 19:06 didn't load at all (no boot line), like the 02:08/02:18 runs. Running processes then:
    RTSS + RTSSHooksLoader64, MSI Afterburner, NVIDIA Overlay, Steam. `RTSSHooks64.dll` statically imports WINMM.dll
    (ReShade's dxgi.dll delay-imports it). Hypothesis (unconfirmed): an injected DLL sometimes loads System32
    winmm.dll first, so ours is never mapped. `version.dll` loaded first try. **Use `version.dll` for Gears.**
  - Code scan stats: 224.5 MB in 3 regions, 0 unreadable, 0 faults, **0 XOR-encoded loads**: the Steam-build idiom is
    not native code in the Xbox build (virtualized or different codegen).
  - **Heap scan found GUObjectArray:** `heap scan found ObjObjects (1.37 GB in 1959 regions, 344 ms)`, ObjObjects at heap
    0x7FF4FC720010, stride 0x18, 75910 objects, picked up ~30 s after start (scan starts at the 3rd failed init).
    Layout: SuperStruct 0x40, ChildProperties 0x50, FField.Next 0x18, FField.Name 0x20, ElementSize 0x34,
    Offset_Internal 0x44, sizeof(FProperty) 0x70, ControlRotation StructProperty. Engine `GameEngine_2147482572`.
  - Front end: pc `FairlightPlayerController_FrontEnd_BP_C`, view target `BP_UIActor_FrontendCam_C`, no pawn (view=none,
    calibration runs there). Campaign: pc `BP_PlayerController_Campaign_C`, pawn/target
    `BP_Pawn_Marcus_Casual_Holster_LipScar_C`, **generic ControlRotation view works, no profile needed**
    (`applied yaw=109.5 pitch=-17.4`). Pause detected (paused=1 during the intro).
  - Perf (game thread, PeekMessageW frame): idle avg 90 us p99 274 us; active avg 109 us p99 373 us; Present hook avg
    25 us p99 59 us.
  - Open: why winmm.dll sometimes doesn't load (repeat version.dll launches with RTSS open to confirm it's reliable);
    the 30 s delay before the gyro is available (heap scan could start earlier).
  - 0.5.7 deployed to Gears as `version.dll` (19:43; no winmm.dll left in WinGDK).
- 2026-10-09 **0.5.7 deployed (Steam) to Cronos (was 0.4.6), Pacific Drive (was 0.3.4), Life is
  Strange Remastered and Out of Sight (both new, not yet run).** Saves backed up first:
  `um backup` lis-remastered-saves (%LOCALAPPDATA%\LiS\Saved\SaveGames) and outofsight-saves
  (%LOCALAPPDATA%\OutOfSight\Saved). Profiles copied for Cronos and Pacific Drive; users' gyrolib.ini kept.
  - LiS Remastered: `LIS\Binaries\Win64\LiS-Win64-Shipping.exe`, UE4SS present as dwmapi.dll (left as is), SDL2.dll.
  - Out of Sight: `OutOfSight\Binaries\Win64` holds two full game builds, `OutOfSight-Win64-Shipping.exe` (153 MB)
    and `OutOfSight.exe` (309 MB, imports WINMM/dxgi/..., delay-loads steam_api64/EOSSDK); one runs at a time, so the
    instance mutex is not an issue; the boot log will show which one Steam launches. ReShade 6.7.3 (dxgi.dll) +
    RenoDX UE add-on, a d3d12.dll and the D3D12 Agility folder present. No anti-cheat files in any of the four.

- 2026-10-09 **GyroUE 0.5.8: probe output in its own file.** With `Probe=1`, results go to
  `GyroUE\GyroUE-probe.log` (recreated each session, header with the F7/F8 steps and how to read rot/raw lines);
  `GyroUE.log` only gets `probe: snapshot #N taken ... -> GyroUE-probe.log` / `probe: diff of snapshot #N (...)`.
  `log.cpp`: `probe_log_open`/`probe_log` (falls back to GyroUE.log). Tested: file + header created with a test
  profile; F7/F8 output path compiled only (needs a UE game). README probe section updated. Not deployed.

- 2026-10-09 **GyroUE 0.6.0: probe for non-experts (steps 1-3 of the plan).**
  1. **Follow check (always on):** each frame compares the gyro rotation written (|yaw|+|pitch|) with how much the
     camera's POV moved (`PlayerCameraManager.CameraCachePrivate.POV.Rotation`, new `ue::camera_pov`, offsets cached
     per class; `CameraCache` fallback). 2 s windows with >= 10 deg of gyro: ratio < 0.2 = ignored, > 0.5 = followed.
     Only when view target == pawn (or a profile view with ViewTargetClass), not paused, no cursor. 3 ignored windows
     and none followed -> one `camera: the gyro turned X ... the game ignores it. ... put [GyroUE] Probe=1 in ...`;
     3 followed -> one `camera: confirmed, the camera follows the gyro (...)`.
  2. **Automatic capture (Probe=1):** snapshot once the POV has been still 0.5 s (min 1.5 s after the last capture);
     when the POV has turned > 30 deg yaw and is still again for 0.3 s, a scored diff, unless the gyro wrote more
     than 20% of the turn ("discarded"). Re-snapshot if the pawn changes or after 2 min. Up to 8 per session.
     F7/F8 still work and switch the automatic capture off for the session.
  3. **Ranking + suggested profile:** each diff with |yaw| >= 10 deg is scored. Candidates = reflected rotators and raw
     float/double (pitch, yaw) pairs that now equal the POV pitch/yaw (0.6 deg) and whose yaw changed. Ignored with a
     reason: `Camera` root (output), RelativeRotation / raw offsets inside the SceneComponent part (transforms),
     PC.ControlRotation (the default view), nested struct properties. Usable ones that hit every scored diff (all but
     one from 4 on) are grouped per object (raw offsets of one size together, like Cronos' two copies) and ranked
     Pawn > ViewTarget > PC, more rotators first, named before raw. From 2 scored diffs on, written to
     `GyroUE\profiles\<exe>.suggested.ini` (top pick as [View.Default] + commented alternatives + ignored ones) and
     to GyroUE-probe.log; GyroUE.log gets `probe: suggested profile: <pick> -> <path>`.
  Read-only (no memory writes). Not run in a UE game yet. README probe section rewritten around the automatic mode.
  **Validation set up in Cronos:** 0.6.0 deployed, verified profile kept as `Cronos-Win64-Shipping.ini.verified`,
  `Cronos-Win64-Shipping.ini` = `[GyroUE] Probe=1` only. Expected: the follow check warns (ControlRotation ignored)
  and the suggestion is `Object=Pawn.View Offsets=0x2A0,0x420 RotatorSize=24`. Restore afterwards by redeploying.

- 2026-10-09 **GyroUE 0.6.1: on-screen notices + follow check stops after its verdict.**
  - `notify.cpp`: a GDI box at the top centre of the game's largest visible window (layered 225 alpha, click-through,
    WS_EX_NOACTIVATE/TOOLWINDOW/TOPMOST, own thread + message loop created on first use, 250 ms timer to follow the
    window, hidden when the game isn't foreground, font = client height/40 clamped 16-36 px Segoe UI). Renderer-agnostic
    (DX11/DX12); not visible in exclusive fullscreen. `Notices=0` under [GyroUE] in the profile turns it off.
    Verified visually with a test window (DPI-unaware test exe, so scaled).
  - Messages: follow-check warning (3 variants: probe on / no profile -> how to create the Probe=1 profile / profile
    outdated), probe ready, turn discarded (gyro moved), capture N done (+ suggested profile written, rename hint),
    no usable candidate yet.
  - Follow check: off after the first verdict (confirmed or warned); the POV is then only read if Probe=1.
  - Cronos re-deployed with 0.6.1, verified profile refreshed as `.ini.verified`, `.ini` = Probe=1 only.

- 2026-10-09 **Crash in Cronos with 0.6.1 at startup: EXCEPTION_STACK_OVERFLOW** (UE crash folder
  %LOCALAPPDATA%\Cronos\Saved\Crashes\UECC-Windows-84ADE0C5..., analysed with cdb + version.pdb): endless recursion
  `hk_create_swapchain_hwnd+0x40 -> d3d12!Ordinal196+0x2399c -> hk_create_swapchain_hwnd -> ...`. The d3d12.dll is
  **SCSKiller 1.2.4 (D3D12 proxy, "shader compilation stutter killer")**, installed in Cronos' Win64 on 2026-10-09
  02:08 (also present in Out of Sight). Not specific to 0.6.x: **bug in patch_vtable since the first panel versions.**
  We hooked factory vtable slot 15, SCSKiller hooked the same slot after us (saving ours as its original), and on the
  next factory patch_vtable saw a foreign function in the slot and patched it again, saving SCSKiller's function as
  our original: we call it, it calls us.
  - **GyroUE 0.6.2:** `patch_vtable` patches each slot once and keeps the original per slot address
    (`g_slot_originals`); every hook calls `original_of(object, index, fallback)`, the original saved for the slot its
    object's vtable points to (CreateSwapChain 10, CreateSwapChainForHwnd 15, Present 8, ResizeBuffers 13, Present1 22,
    ResizeBuffers1 39). Test `chain.cpp` (a second hooker overwrites slot 15 after us, then a second factory and the
    swapchain): 0.6.1 loops (depth 51), 0.6.2 calls it once, swapchain captured, panel ready.
  - Also in that log: `ue: ChildProperties/FField layout not found` at 7810 objects, 4 s after start (retried every
    5 s; earlier Cronos runs found it later). Watch on the next run.
  - 0.6.2 deployed to Cronos (probe test profile kept, verified one as .ini.verified) and Out of Sight (SCSKiller too).
    Gears, Pacific Drive and LiS still run 0.5.7 (same latent bug if a chaining hooker shows up).

- 2026-10-09 **GyroUE 0.6.3:** probe-ready notice reworded (user text: "GyroUE probe ready to find cameras. In game, put the controller down, turn the camera with the RIGHT STICK (more than 30 deg), then stop. Repeat 3-4 times.") and shown 30 s instead of 15. Deployed to Cronos (probe test profile kept).

- 2026-10-09 **GyroUE 0.6.4: global settings file + the probe writes the game's profile.**
  - `GyroUE\GyroUE.ini` (all games in that folder): created with the defaults on the first run if missing, never
    overwritten, not shipped in releases (like gyrolib.ini). `[GyroUE] Probe=0`, `Notices=1`, commented. `setting()`
    reads the game's profile `[GyroUE]` first (override), then GyroUE.ini, then the default. Log:
    `settings: Probe=N Notices=N (...)`. Tested: created with defaults; Probe=1 there enables the probe; a profile with
    Probe=0 overrides it.
  - Probe output: if the game had no profile when it started and the result has a usable view, the probe writes
    `profiles\<exe>.ini` itself (header "created by the camera probe ... NOT VERIFIED. Restart ... then set Probe=0 in
    GyroUE\GyroUE.ini") and keeps updating that file during the session; otherwise `<exe>.suggested.ini` (header "the
    game already has a profile ..."). A result with no usable view never becomes the profile.
  - Notices/log point to GyroUE.ini ("set Probe=1 in GyroUE\GyroUE.ini, restart ..."); capture notice says "Game
    profile created ... Restart the game to use it, then set Probe=0" or "already has a profile ... .suggested.ini".
  - README (install layout, probe steps 2-3), CLAUDE.md (releases ship no GyroUE.ini) updated.
  - **Cronos test set up the new way:** 0.6.4 deployed, `Cronos-Win64-Shipping.ini` removed (verified copy kept as
    `.ini.verified`), `GyroUE\GyroUE.ini` with Probe=1. Expected: warning, captures, then the probe creates
    `profiles\Cronos-Win64-Shipping.ini` = Pawn.View 0x2A0,0x420.

- 2026-10-09 **Life is Strange Remastered probe test (0.6.4, DX11, GyroUE.ini Probe=1, no profile).** UE4 build with a
  non-stock FField layout: `ue: layout SuperStruct=0x40 ChildProperties=0x48 FField.Next=0x28 FField.Name=0x18
  FProperty.ElementSize=0x34 Offset_Internal=0x44 sizeof(FProperty)=0x70 ControlRotation type=` (empty). Engine found
  (`GameEngine_...`), PC `LiSPlayerController_BP_C`, pawn `MaxCharacter_BP_C`, camera `LiSPlayerCameraManager`, view
  target mostly `CameraActor_10xx` (scripted cameras), sometimes the pawn. GUObjectArray at exe+0x35A3908, FNamePool
  exe+0x358C390.
  - **Bug:** `field_type()` read the FFieldClass at a hard-coded +0x08, wrong in this build, so every property type was
    "" and every probe snapshot had **0 values** (145-161 objects). The automatic capture still measured POV turns
    (`camera_pov` only needs names/offsets: 41-174 deg yaw, 0-4 deg pitch) but each diff hit the empty-leaves branch
    and printed the misleading "no snapshot (press F7 first)". 5 turns were discarded because the gyro moved too.
  - Gyro not moving the camera: ControlRotation is written (applied yaw -117) but the camera doesn't follow; no
    follow-check verdict because most of the time the view target is a CameraActor (check skipped by design).
  - **GyroUE 0.6.5:** `detect_layout` finds FField::ClassPrivate (0x08-0x40, the pointer whose FName is
    "StructProperty" on ControlRotation and "ObjectProperty" on Pawn); logged as `FField.Class=0x..` in the layout
    line; falls back to 0x08 with a warning. Empty-snapshot message now says the snapshot holds no values (layout
    problem) instead of "press F7 first". Deployed to LiS (GyroUE.ini Probe=1 kept). Not yet run.

- 2026-10-09 **LiS run with 0.6.5:** `ue: FField class offset not found` -> still 0 values. Reason: **LiS Remastered is
  UE <= 4.24 (UProperty objects, no FField)**. The "FField" layout detected earlier (Name 0x18, Next 0x28,
  ChildProperties 0x48) is really UObject::NamePrivate / UField::Next / UStruct::Children; the property's type is
  its UClass at +0x10 (UObject ClassPrivate), named by the UClass's own UObject name (+0x18), not FFieldClass::Name
  at +0. All 8 automatic captures (61-174 deg yaw, 0-12 deg pitch) had nothing to compare and showed nothing on
  screen. One "turn discarded" during the opening cutscene: the cutscene turned the camera 30 deg while the
  uncalibrated resting controller drifted a few degrees (> 20 % of 30).
  - **GyroUE 0.6.6:** the type detection tries FFieldClass::Name at +0 first, then a UClass name at +0x18
    (`g_field_class_name`); `field_type()` uses it; layout line says "FField properties, UE 4.25+" or "UProperty
    objects, UE <= 4.24" and `Field.Class=`. Discard only if the gyro moved > 5 deg and > 20 % of the turn; on-screen
    notice only if > 10 deg. A capture that can't be compared now shows "capture N could not be compared".
    Retry message reworded ("property layout not found yet"). Deployed to LiS. Not yet run.

- 2026-10-09 **GyroUE 0.6.7:** GyroUE-boot.log removed (user: no longer useful). It was the 0.5.1 diagnostic for the winmm.dll runs that never loaded; the instance mutex stays, a second process just stays a plain proxy silently. README troubleshooting row removed. Old GyroUE-boot.log files in game folders are left as they are. Deployed to LiS.

- 2026-10-09 **LiS run with 0.6.7: the probe reads LiS now** (`ue: layout (UProperty objects, UE <= 4.24) ...
  Field.Class=0x10 ... ControlRotation type=StructProperty`, ~3300-3780 values per snapshot). Every scored turn
  (8, 61-174 deg yaw) matched the same 9-11 rotations: PC.ControlRotation (excluded: default view), Camera
  ViewTarget/CameraCachePrivate POV, Camera/PC TransformComponent0 transforms, and **raw float rotators
  LiSPlayerCameraManager +0x33CC and +0x33FC** (excluded as "camera output") -> "no usable candidate".
  Discards: during the opening cutscene (30 deg turn, gyro drift 10 deg) and while turning the stick with the pad on
  the table (gyro 223-294 deg: the sum is of absolute degrees, so the pad rocking under the thumb adds up).
  - **GyroUE 0.6.8:** (1) the gyro output is paused while the automatic capture runs (`probe_capturing()`), so the
    gyro-discard path is gone and the pad can stay in the hands; (2) a turn only counts if the right stick was
    deflected (> 0.3) since the snapshot (`gl_get_flick_input`, GyroLib's selected controller); otherwise "ignored:
    the right stick wasn't used (cutscene or scripted camera)" (filter off if GyroLib never reports the stick);
    (3) raw rotators of the Camera root past the engine's PlayerCameraManager part (`engine_part_size`) are
    candidates (rank 1, like PC), the engine part stays "camera output". Probe-ready notice text unchanged (user's
    wording; "put the controller down" is no longer required). README capture steps updated. Deployed to LiS
    (old .suggested.ini removed). Expected: profile Object=Camera Offsets=0x33CC,0x33FC RotatorSize=12 - unverified.

- 2026-10-09 **GyroUE 0.6.9:** "put the controller down" removed (the gyro is paused while capturing and only stick turns count): probe-ready notice now "GyroUE probe ready to find cameras. In game, turn the camera with the RIGHT STICK (more than 30 deg), then stop. Repeat 3-4 times." (30 s); same in the GyroUE-probe.log header and the default GyroUE.ini comment (existing GyroUE.ini files keep their old comment). Deployed to LiS.

- 2026-10-09 **LiS with 0.6.9: the probe wrote the game's profile, and after a restart the camera follows the gyro
  (log).** Probe session (22:49-22:50): 6 turns without the right stick ignored (opening cutscene/scripted cameras),
  then 4 stick turns scored (124-176 deg yaw, one with 36 deg pitch), 11 rotations each. New candidates vs 0.6.7:
  `Camera(LiSPlayerCameraManager)+0x3ACC` and `Pawn.LiSCamera(LiSCameraComponent)+0x1B0` (the latter a transform).
  The probe created `profiles\LiS-Win64-Shipping.ini`: `Object=Camera Offsets=0x33CC,0x33FC,0x3ACC RotatorSize=12`.
  - Next session (22:51, Probe=0, that profile): `camera: confirmed, the camera follows the gyro (the profile's raw
    offsets, view 'Camera')` at 22:53:33. The status lines show **+0x3ACC accumulates exactly the applied gyro**
    (applied yaw -8.4/pitch -7.3 -> +0x3ACC (P -7.26 Y -8.43); applied yaw 37.6 -> Y 37.63) while **+0x33CC/+0x33FC
    are rewritten by the game every frame** (they stay at the POV, e.g. Y -108.1, whatever the gyro adds). So
    +0x3ACC is the working input (an additive look offset the game keeps) and +0x33CC/+0x33FC are outputs; writing
    them is harmless but useless. Awaiting the user's in-game confirmation.
  - Idea: the profile view could read each raw offset back next frame and log the ones the game overwrites
    ("output, not input"), and the probe could drop them from the suggestion the same way.

- 2026-10-09 **LiS: user confirms the gyro works, but it "suddenly stops" while walking and turning the pad sideways.**
  Status lines (every 10 s): with a fixed CameraActor target, +0x3ACC == applied gyro (an additive look offset only
  the gyro touches); following Max (target MaxCharacter_BP_C) +0x3ACC is the full look rotation (e.g. Y -166, 145),
  also driven by the game. `cursor=1 gyro enabled=0` appears at times in gameplay (GyroUE pauses the gyro when the
  mouse cursor is shown). Suspects: (1) LiS interaction cursor/icons near objects -> cursor=1 -> gyro paused;
  (2) auto-recentering behind Max while walking overriding +0x3ACC; (3) less likely, the writes to the output
  offsets +0x33CC/+0x33FC. 10 s samples can't tell which.
  - **GyroUE 0.6.10:** logs every change of the gyro gate with its reason (`gyro on` / `gyro off: mouse cursor shown`
    / `off: game paused` / `off: no gameplay view`, plus the view target class). LiS profile trimmed by hand to
    `Offsets=0x3ACC` (probe's original kept as `LiS-Win64-Shipping.ini.probe.bak`). Deployed to LiS.

- 2026-10-09 **LiS with the profile trimmed to +0x3ACC: the gyro didn't move the camera at all (user).** The 0.6.9
  analysis was wrong: +0x33CC/+0x33FC staying equal to the POV did NOT prove the game overwrites them. GyroUE writes
  before the game ticks; the game reads our value, integrates it and writes the result back, so they always equal the
  camera *including* the gyro. They are the input; +0x3ACC only accumulates the gyro (in follow-Max mode it is a
  game-driven absolute rotation). Lesson: a read-back after one frame can't tell "integrated and written back" from
  "overwritten"; the "drop outputs by read-back" idea from the previous entry is invalid as stated.
  - The log said `camera: confirmed` anyway: false positive, the follow check counted camera motion from the stick.
  - **GyroUE 0.6.11:** follow-check windows in which the right stick moved are not judged.
  - LiS profile now `Offsets=0x33CC,0x33FC` (A/B test: works + no cut while walking -> +0x3ACC was interfering;
    works + yaw still cut while walking -> the game's auto-recentering; doesn't work -> restore the probe original
    `.probe.bak` = 0x33CC,0x33FC,0x3ACC, which worked). 0.6.11 deployed to LiS.

- 2026-10-09 **LiS A/B with Offsets=0x33CC,0x33FC (0.6.11): only pitch moves the camera** (user). The follow check
  caught it by itself: `the gyro turned the profile's raw offsets by 1274 degrees but the camera moved only 90`.
  With +0x3ACC alone nothing moved; with all three everything worked except yaw while walking. So the LiS camera
  splits the axes: **pitch from +0x33CC/+0x33FC, yaw (following Max) from +0x3ACC**, and while Max walks the game
  turns +0x3ACC toward her walking direction (auto-recentering), overriding the gyro yaw.
  - Profile restored to the probe's original (0x33CC,0x33FC,0x3ACC) with a `[GyroUE] Probe=1` override for a manual
    F7 (standing) / walk without touching the camera / F8 capture, to find what drives the recentering (timers,
    flags, speeds in the `num` lines), like Pacific Drive's car (`OnGyro=`).

- 2026-10-09 **LiS manual probe (F7 standing, walk without touching the camera, F8):** the walking camera is the camera
  mode object `PC.TP_Walk_Manual_C_<n>` (TP_Walk_Manual_C, outer = the PC). In both walking diffs (82 and 51 deg of
  camera yaw with no right stick) its `SmoothedAutoManualRatio` fell toward 0 (0.0719 -> 0.0025): the blend between the
  automatic follow camera (0) and manual control (1). The gyro isn't manual input for the game, so walking blends to
  auto and the follow camera wins the yaw. Other changes while walking: PC `LastTimeLeftStickChanged`,
  `TimeBeforeAdditiveWalk` (counts down), `fCurrentTimeoutNoVelocityForceIdle`; TP_Walk_Manual `fTimeSinceActivation`,
  `DampedPitch`, `SmoothedLocationZ/TargetZ`, `CurrentGroundSpeedSmoothed`.
  - **GyroUE 0.6.12:** path step `@Class` = first subobject of that class (`ue::find_subobject_of_class`, cached like
    name lookups), for instance names that change every session; `OnGyro=` entries may carry an object path
    (`Root.Sub.Name:value`), resolved like `Object=` (`resolve_path`); a failed write is logged once. README profile
    reference updated. Pacific Drive's OnGyro (no path) unchanged.
  - LiS profile: `OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1` added, `[GyroUE] Probe=1` removed. Deployed.
    Not yet verified in game.

- 2026-10-09 **LiS verified (user): gyro yaw and pitch, fixed cameras and following Max, also while walking** (0.6.12,
  OnGyro on PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1, no OnGyro write failures).
- 2026-10-09 **Cronos probe validation (0.6.4, run 21:56, found while cleaning up):** with only Probe=1 and no profile,
  the probe wrote `profiles\Cronos-Win64-Shipping.ini` itself after 5 scored captures: `Object=Pawn.View
  Offsets=0x2A0,0x420 RotatorSize=24`, exactly the manually verified profile.
- 2026-10-09 **GyroUE 0.6.13 (cleanup after LiS):**
  - Conflict check against the other games' logs: every UE 4.25+/UE 5 game read `ControlRotation type=StructProperty`
    with the old fixed FFieldClass at +0x08 (Cronos/Out of Sight/Gears: ChildProperties 0x50, Next 0x18, Name 0x20;
    Pacific Drive/Returnal: Next 0x20, Name 0x28). The new detection tries FFieldClass::Name at +0x08 first, so they get
    the same result; the UProperty pass only runs if that fails. Other LiS-driven changes only act with Probe=1
    (stick filter, gyro pause, camera-manager candidates ranked below Pawn), on new profile syntax (`@Class`, OnGyro
    paths; Pacific Drive's OnGyro unchanged) or narrow the follow check (stick windows skipped).
  - No test code in the repo (test programs live in the scratchpad). `ue: property layout not found yet` logs once.
    The `gyro on/off` gate lines stay (only on change; general diagnostic).
  - `profiles/LiS-Win64-Shipping.ini` added to the repo (clean, verified), shipped in releases. README: LiS row in the
    compatibility table; engine requirement now "UE 4 and UE 5, both property systems".
  - Deployed to LiS (repo profile) and Cronos (verified profile restored, GyroUE.ini Probe=0). Left in game folders:
    LiS `LiS-Win64-Shipping.ini.probe.bak` / `.suggested.ini`, Cronos `Cronos-Win64-Shipping.ini.verified`, old
    GyroUE-boot.log files.

- 2026-10-10 **Switch Pro Controller judder (user, Cronos).** With GyroLib/SDL (`input.steam_mouse=1` Block), the camera
  isn't smooth; Steam Input gyro-to-mouse (Pass through) is. The DualSense shows almost no difference.
  - gyroprobe with the Switch Pro: ~200 accepted samples/s (695 in 3.5 s), 0 rejected/dropped. SDL delivers all 3 IMU
    samples per report, but they arrive together about every 15 ms (~66 reports/s). The DualSense gives ~460/s.
  - Cause: at frame rates above ~66 fps, GyroLib outputs rotation only on frames where a burst arrived and 0 on the
    others. `gyro.smoothing_ms=20` + `smoothing_threshold_dps=100` barely helped (user): GyroLib's filter only steps when
    samples arrive, so the empty frames stay empty. Steam's gyro-to-mouse emits at a steady rate.
  - **GyroUE 0.6.14: pacing.** GyroUE measures the interval between non-zero GyroLib outputs (EMA, gaps over 100 ms
    ignored). If it is longer than 1.25 frames and at most `PacingMaxMs` (50 since 0.6.15), each burst is released at a constant rate
    over that interval (rate = pending / interval, never overshooting). Otherwise the output is applied directly, so no
    latency is added on the DualSense. Pending motion is dropped on view change, camera gate or probe capture.
    `GyroUE.ini`: `Pacing=1` (default), `PacingMaxMs=50`. The status line logs the burst interval, the frame interval
    and the pacing state. Pending user test with the Switch Pro (USB and BT) and the DualSense.
  - Possible upstream issue for GyroLib: optional output pacing/interpolation for burst-reporting controllers.
  - **Luto (UE5, Epic, `winmm.dll` proxy) test of 0.6.14, Switch Pro, Block:** pacing engaged the whole time
    (`motion bursts every 20-26 ms, frames every ~12 ms -> pacing on`). The sample rate was only **~67/s** this session,
    against ~200/s in the 01:31 Luto session and in gyroprobe, so the source path differed (likely Steam-filtered isolated
    reader or BT vs USB; to confirm). The 20-26 ms burst interval was close to the 30 ms limit, so **0.6.15 raises the
    `PacingMaxMs` default to 50**. The user set `gyro.smoothing_ms=25` mid-session, which adds lag on top of pacing.
  - User: pacing "improved a bit, but nothing like Steam Input mouse" (pass-through, or Steam's own mouse output).
  - **Luto, Steam Input OFF (0.6.15, Block, smoothing 0):** ~200 samples/s again (vs ~67/s with Steam Input on, so the
    Steam-filtered isolated reader loses about 2/3 of the samples). But the burst cadence seen per frame is unchanged:
    a non-zero GyroLib output every 21-27 ms (~40-45/s, about 5 samples each) at ~83 fps. So the samples reach GyroUE
    in groups more widely spaced than the controller's ~15 ms reports. Not yet known whether that is the controller/BT,
    SDL/HIDAPI or GyroLib's reader.
  - **GyroUE 0.6.16: motion trace.** `MotionTrace=1` in GyroUE.ini records 30 s from the first gameplay frame to
    `GyroUE\motion-trace.csv`: every accepted sample with sensor/arrival ns (GyroLib `gl_set_sample_observer`), and
    every frame with `new_samples`, raw GyroLib yaw/pitch and the paced yaw/pitch. `tools/motion_trace_report.py`
    summarises the sensor interval, arrival interval, samples per arrival, frame interval, frames with no rotation,
    and the speed CV while turning (raw vs paced). Checked against a synthetic trace.
  - **Trace, Luto, Switch Pro over Bluetooth, Steam Input off, 0.6.16 (30 s):**
    - 198 samples/s. The sensor interval is regular: 5.24 ms mean (p10-p90 4.8-6.1). SDL delivers all 3 IMU samples
      per report. Sensor timestamps drift against the host clock (about 0.7 s over 30 s), so they can't be mapped
      directly to host time.
    - Arrivals are bunched in the transport (BT / Windows HID): about 45 arrival bursts/s instead of ~66 reports/s,
      gaps median 25.4 ms (p10 2.9, p90 38, max 68). New samples per update: 0 (73%), 3, 6, 9 or 12, so 1-4 reports
      come at once.
    - GyroUE's hook ran ~143 times/s at ~85 fps (median interval 1.9 ms): Luto pumps messages twice per frame.
    - Raw GyroLib yaw: 73% of updates had no rotation, speed CV 1.99. 0.6.16 pacing: 26%, CV 0.78. That matches the
      user's "a bit better, nowhere near Steam".
  - **GyroUE 0.6.17: jitter-buffer pacer** (replaces the per-burst rate):
    - Each update's GyroLib rotation is split over its samples, weighted by gyro speed (the sample observer now always
      runs). The samples are queued and played back at the measured sample rate (1 s windows, EMA), with speed nudged
      ×0.5–2 to keep `target = min(gap_mean + 0.5·σ_gap, PacingMaxMs)` seconds queued.
    - Pacing is active only when `gap_mean > 1.5 × update interval`, so pads that report every update (DualSense)
      aren't delayed. The queue is capped at 1.5 × PacingMaxMs after stalls and flushed when pacing turns off or the
      view/gate changes.
    - Replaying the recorded trace (`tools/sim_pacer.py`), speed CV over 12 ms slots: raw 1.56, 0.6.16 0.75, 0.6.17 0.44
      (empty slots 51% / 9% / 2%). Queue delay median 18 ms, p90 32 ms. A margin of 1.0-1.5σ didn't improve CV
      (plateau ~0.43), only added delay (24-30 ms median).
    - Status line: `motion N samples/s, bursts every X ms, updates every Y ms -> pacing on, Z ms buffer`.
    - Release zip built.
  - **0.6.17 user test (Luto, Switch Pro BT, Steam Input off):** "clearly smoother, but not at the level of Steam
    Input gyro-to-mouse". Status: ~200 samples/s, bursts every 20-25 ms, updates every 6.5 ms, pacing on with a 26-32 ms
    buffer (larger than the 18 ms simulated, because the in-game gap mean was higher). The new trace had too little
    motion to score (11° total in 30 s); the applied output had 0-1% empty slots.
  - **Decision:** stop investing in GyroUE-side pacing. The jitter belongs in acquisition/timing, which GyroLib owns
    (sensor timestamps, its device-clock estimator, per-sample processing). Keep the 0.6.17 pacer as a contained,
    switchable stop-gap (`Pacing=0`). Report to GyroLib: (1) BT burst delivery makes output steppy; it should play back in
    sensor time with a small adaptive delay; (2) the Steam-filtered isolated reader delivers ~67 instead of ~200
    samples/s for the Switch Pro. Steam Input's implementation is closed source and can't be compared. The closest open
    references are SDL's HIDAPI Switch driver and JoyShockMapper/GamepadMotionHelpers.

- 2026-10-10 **GyroUE 0.6.18: ships as `winmm.dll` by default, `version.dll` is the fallback name** (user: winmm has
  wider support to start with). Build output, version resource OriginalFilename and `proxy.def` LIBRARY are now winmm;
  the DLL still exports both sets and forwards by its own file name, so nothing changes at runtime. `deploy-gyroue.sh`
  defaults to `winmm.dll` (`PROXY=version.dll` for the fallback; Gears of War: E-Day needs it, see 2026-10-09: with
  RTSS running the system winmm.dll is loaded first), the release zip holds `winmm.dll`, `bench.sh` handles either name.
  Docs (README, TROUBLESHOOTING, ADVANCED, AGENTS, CREDITS, BENCHMARKS) updated. Already-installed games keep their
  current name; the installer removes the other name when redeploying. Not yet run in game.

- 2026-10-11 **GyroUE 0.8.0 = main's 0.7.0 merged into `feature/xinput-translation`** (user's decision: minor bump for
  the XInput translation). Conflicts resolved: `host.cpp` default `GyroUE.ini` keeps `Notices` plus the XInput keys
  (the `Pacing` keys are gone with the pacer), CMake builds `deps.cpp` and `xinput.cpp`, docs keep both sides. Builds
  clean against GyroLib 1.3.1.
- 2026-10-11 **Release: GyroUE 0.7.0** (minor bump, user's decision: new features, not only fixes). Contains
  everything after 0.6.18 that was numbered 0.6.19 below: guard-page fix, gyro-camera notice wording,
  `WorldRotation=1` + probe ranking + Severed Steel profile, GyroLib 1.3.1 (GyroUE pacer removed), dependency check
  (`deps.cpp`). Zip `build/release/GyroUE-v0.7.0.zip` (gyrolib.dll = the 1.3.1 SDK's).
- 2026-10-11 **Release numbering:** production is still 0.6.18, so everything after it ships as **0.6.19**: the
  guard-page fix below plus the Severed Steel work (status addresses, `WorldRotation=1`, probe ranking, profile). The
  0.6.20 / 0.6.21 numbers were local test builds only (their zips deleted); the demo and Cronos were redeployed as 0.6.19.

- 2026-10-10 **GyroUE 0.6.19: guarded reads no longer swallow stack guard-page hits** (ported from PSButtonsUE, see its
  MODLOG "Luto crash with v0.1.0"). `guarded_copy`, `guarded_store`, `scan_xor_loads` and `scan_heap_region` used
  `__except(EXCEPTION_EXECUTE_HANDLER)`. A stray but plausible pointer landing on another thread's stack guard page raised
  STATUS_GUARD_PAGE_VIOLATION; swallowing it lets Windows clear PAGE_GUARD, so that thread can't grow its stack and the
  game later crashes in ntdll (AV at `ntdll+0x12c083`, Windows 11 26100; PSButtonsUE hit it in Luto, UE 5.5.4, while
  init() retried every 100 ms). New `guard_filter` in `ue.cpp`: on a guard-page violation it re-applies PAGE_GUARD to the
  faulting page (VirtualQuery + VirtualProtect on `ExceptionInformation[1]`) and handles it; access violations and
  in-page errors are handled as before; anything else is no longer swallowed (EXCEPTION_CONTINUE_SEARCH). No GyroUE crash
  was attributed to this yet; it's a latent bug in the shared core. Built with `tools/build-gyroue.sh`, release zip
  packaged. Deployed to Luto (`<Luto>\Luto\Binaries\Win64`, `winmm.dll`, replacing 0.6.17; alongside
  PSButtonsUE 0.1.0 `dsound.dll` and scskiller `d3d12.dll`; generic profile, gyrolib.ini kept). Not run in game yet.

- 2026-10-11 **Severed Steel (Demo, UE 4.27.2, exe `ThankYouVeryCool-Win64-Shipping.exe`, DX11).**
  - **Crash at startup (0.6.18, also reported by a user on the full game):** the log stops after `gyro: settings ...`
    (inside `gl_sdl_create`). UE crash report: EXCEPTION_ACCESS_VIOLATION reading 0x0 in `MSVCP140+0x12590` (the
    internal lock routine behind `_Mtx_lock`) called from `gyrolib.dll`. The game ships the VC++ runtime 14.24 next to
    its exe (`msvcp140*.dll`); the process loads that copy and `gyrolib.dll` (dynamic CRT, built with MSVC >= 14.40)
    gets it too. Known MSVC break: binaries built with 14.40+ construct `std::mutex` with a constexpr constructor that
    old `msvcp140` (< 14.40) can't lock. Our `winmm.dll` uses the static CRT and is not affected. Workaround verified:
    renaming the game's `MSVCP140.dll` and `MSVCP140_2.dll` makes it start (system 14.51 runtime used). Real fix belongs
    in GyroLib (static CRT `/MT`, or `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` on all its C++ code); reported upstream by
    the user.
  - **Camera not found yet.** The generic view does nothing (user: no movement, no jitter). Auto probe: the profile it
    wrote (Pawn raw 0x1284,0x818), ReplicatedControlRotation and all raw Pawn rotators at once (0x7EC, 0x818, 0x1278,
    0x1284) are all ignored: 0x7EC/0x818/0x1284 snap back to the camera every frame, 0x1278 keeps our writes but isn't
    read. Manual probe (F7/F8, 4 diffs, yaw only): every rotator, float and raw value that moves is a copy of
    `PC.ControlRotation` (= POV), or yaw - 90 (Pawn+0xCF8, Pawn+0x1D20, HeadSpringArm). So ControlRotation is rebuilt
    every frame from a source outside the probe's roots or in another form (vector, quaternion).
  - **GyroUE 0.6.19 (test build 0.6.20 during the session):** the status block logs `status: addresses pc=0x... (ControlRotation +0x...) pawn=0x...
    camera=0x...` whenever they change, for a debugger write breakpoint on ControlRotation (next step: find who writes
    it each frame). Deployed to the Severed Steel demo (`winmm.dll`, generic view, the probe's profile parked as
    `.ini.probe`).
  - **Write breakpoint on `PC.ControlRotation.Yaw` (cdb, `ba w4`, 6 hits):** besides GyroUE's own writes (several per
    frame: the game pumps messages from more than one place, each runs `hooked_peek`), the only writer is
    `movsd [rbx+288h], xmm0` after an `Equals` tolerance test, i.e. `APlayerController::SetControlRotation`, once per
    frame, called through the Blueprint VM (ProcessEvent / script frames on the stack). So a Blueprint computes the view
    rotation from its own state each frame and sets it; ControlRotation is never read back. Candidate state: Pawn+0x1D20
    (unwrapped yaw - 90, looks accumulated) and Pawn+0xCF8 (wrapped yaw - 90), both raw and not reflected floats, so
    probably inside a Vector/Transform BP variable. Next: `Dump=1` on the pawn to name them.
  - cdb note: quitting with `qd` from inside a `ba` command string ended the game process (no crash report). Detach
    with a cleared breakpoint (`bc *; .detach; q`) next time.
  - Correction: the caller is native, not Blueprint. `Dump=1` on the pawn: `PlayerCharacter` (native C++ class under
    `ALSBaseCharacter`) has 419 reflected properties; +0x1D20 sits in an unreflected gap (0x1D04-0x1D2B). Writing it
    (`Offsets=0x1D1C`) is ignored too (yaw snaps back, pitch at 0x1D1C keeps our writes but isn't read).
  - Static disassembly (`cdb -z` on the exe, addresses relative to its preferred base): PlayerCharacter's tick reads
    the look input `+0xBD8` (yaw) and `+0xBDC` (pitch), calls two functions (yaw, pitch) that turn the
    `FirstPersonCamera` component (`+0x12A0`) from its current world rotation (`ComponentToWorld` quat at comp+0x1C0,
    cached rotator at comp+0x190), then `Controller->SetControlRotation(camera world rotation)` (vtable +0x688). So
    ControlRotation is an output of the camera component. Writing `Pawn.FPCamera.RelativeRotation` is ignored as well
    (user: nothing, standing or walking): the camera's world rotation isn't driven by its RelativeRotation.
  - The look inputs are written by the game's axis handlers every frame: the mouse handlers store
    `+0xBD8`/`+0xBDC` unconditionally (0 when the mouse is still); the gamepad ones only past a dead zone, into
    `+0xBD8`/`+0xBDC` or `+0x1B2C`/`+0x1BC4` depending on two settings bytes (`[pawn+0x1228]+0xDFD/0xDFE`). They run in
    the PlayerController's input processing, after our PeekMessage frame, so writes there are overwritten too.
  - ~~Conclusion: no memory location GyroUE can write drives this camera.~~ **Wrong, corrected on review:** the
    look functions (pitch one at `0x141705510`, preferred image base) read the camera's world rotation
    (ComponentToWorld quat at comp+0x1C0, rotator cache comp+0x180/0x190), apply the input (pitch clamped to
    StandingPitchMin/Max, pawn+0x14A4/0x14A0) and call `SetWorldRotation` on the camera (quat path while falling,
    rotator path otherwise). So the persistent state IS the camera's world rotation. The camera uses absolute
    rotation (its RelativeRotation equals its world rotation while the capsule is turned too). Writing RelativeRotation
    alone failed because the game rebuilt it from the stale ComponentToWorld in the same frame.
  - USceneComponent layout seen (UE 4.27, float): RelativeRotation +0x128, WorldRotationCache +0x180 (quat) / +0x190
    (rotator), RelativeRotationCache +0x1A0 / +0x1B0, ComponentToWorld +0x1C0 (rotation quat first).
  - **One-shot proof (cdb noninvasive, `ed` with float bit patterns; `ef` can't parse negative numbers):** wrote
    RelativeRotation yaw 91.31 -> 136.31 plus the matching quaternion at +0x1C0. Read back seconds later: both kept,
    and `PC.ControlRotation` became (P -2.64, Y 136.31) by itself. User: the camera turned 45 degrees and stayed,
    and the stick keeps working from there.
  - **GyroUE 0.6.19 (test build 0.6.21 during the session): `WorldRotation=1`** (view option, with `Property=RelativeRotation`). `ue::add_world_rotation`
    finds ComponentToWorld by value once per component class (16-byte aligned quat in the SceneComponent part equal
    to quat(RelativeRotation), skipping the two rotation caches, which are followed by the same rotator; floats or
    doubles from the property size), checks every frame that the stored quat still matches RelativeRotation (else
    skips the frame and logs the reason when it changes), adds the delta (pitch kept within +-89 so the rotator
    doesn't flip) and writes the rotator and quaternion (FRotator::Quaternion formula). Probe: an attached
    component's `RelativeRotation` whose world rotation matches it is now a usable candidate (`WorldRotation=1` in
    the suggestion) instead of "component transform (output)"; a CameraComponent ranks first. Root components stay
    outputs (actor rotation). ADVANCED.md: Example 3 + key docs.
  - Deployed the 0.6.21 test build to the Severed Steel demo with `Object=Pawn.FPCamera`, `Property=RelativeRotation`,
    `WorldRotation=1`. **Verified (user, DualSense): smooth and correct.** Log: `ue: CameraComponent
    ComponentToWorld rotation at +0x1C0 (absolute rotation, float)` at the first gameplay frame, `camera: confirmed`
    3 s later, no `WorldRotation not applied` lines. Profile added as `profiles/ThankYouVeryCool-Win64-Shipping.ini`;
    README lists Severed Steel with the msvcp140 rename note. Test profiles removed from the demo folder.
  - **Probe verified (0.6.21 test build, no profile, Probe=1, 3 scored stick captures):** the profile it wrote picks
    `Object=Pawn.FPCamera`, `Property=RelativeRotation`, `WorldRotation=1` (CameraComponent ranked first); the old
    picks (Pawn raw 0x1284,0x818, RightHandSpringArm raw 0x258, ReplicatedControlRotation) and
    `Pawn.RIghtHandGripLoc` RelativeRotation + WorldRotation=1 follow as alternatives. User restarted with it: works
    (`camera: confirmed`). The repo profile was put back afterwards.
  - **Regression check, Cronos (UE 5.4.4) updated 0.6.14 -> 0.6.21 test build (`version.dll`), no profile, Probe=1, 4 scored
    captures:** the pick is unchanged, `Object=Pawn.View`, `Offsets=0x2A0,0x420`, `RotatorSize=24` (alternatives
    Pawn.SpringArm raw 0x2D8 and Pawn.Animation.LastAimRotation); no WorldRotation candidate appeared. User restarted
    with it: works (`camera: confirmed`). Verified profile restored, Probe=0.

- 2026-10-11 **GyroUE 0.6.19: dependency check before loading GyroLib (`deps.cpp`).**
  - Why: GyroLib (1.3.0 and 1.3.1) links the VC++ runtime as DLLs (MSVCP140, MSVCP140_2, VCRUNTIME140,
    VCRUNTIME140_1; linker 14.51) and delay-loads SDL3.dll. Windows shares a DLL by name, so a copy the game already
    loaded, or one next to its exe (searched before System32), is the one GyroLib gets. Re-checked with 1.3.1 in a
    standalone test (static-CRT exe + gyrolib.dll + the demo's 14.24 runtime copies in its folder): `gl_sdl_create`
    segfaults; with the system's 14.51 runtime it works. So 1.3.1 still has the bug (good repro for the upstream report).
  - What: `deps::check_gyrolib()` runs in `start_gyro()` before `LoadLibrary(gyrolib.dll)`. It reads gyrolib.dll's
    linker version and its regular + delay-load imports (mapped with LOAD_LIBRARY_AS_IMAGE_RESOURCE, nothing runs),
    finds the copy of each VC++ runtime DLL GyroLib would get (already loaded > exe folder > System32) and reads its
    version from the module's version resource (not GetFileVersionInfo: version.dll is one of GyroUE's own proxy names).
    MSVCP* older than 14.40 with GyroLib built by 14.40+ = blocking: GyroLib isn't loaded, the gyro stays off for the
    session, the game starts normally, `deps:` log lines + a 45 s notice name the files and the fix (rename the game's
    copies, or install the VC++ Redistributable if System32's copy is the old one). Older than the build tools but
    14.40+ = log warning only. SDL3.dll already loaded by the game = log warning with both versions. Otherwise one
    `deps:` info line per runtime DLL (useful in future crash reports).
  - Tested on the Severed Steel demo (launched via Steam by the agent): with the game's 14.24 MSVCP140/MSVCP140_2 in
    place, no crash, the main menu loads, the log names both files (MSVCP140 already loaded by the game) and the notice
    is shown complete over the menu (screenshot); VCRUNTIME140/_1 14.24 only warn. With the two files renamed back to
    `-bak`: info lines (MSVCP140 14.51 from System32), `gyro: SDL reader started`. TROUBLESHOOTING, ADVANCED (log
    reference) and README updated.

- 2026-10-11 **GyroUE 0.8.0: SDL controller shown to the game as XInput** (branch `feature/xinput-translation`; the
  local test builds deployed below were numbered 0.7.0 and 0.7.1 before main released its own 0.7.0).
  Goal: games that only read XInput (UE's `XInputInterface`) and run without Steam Input (Epic: Luto) ignore a
  DualSense / Switch Pro except for the gyro. New `xinput.cpp` answers the game's `XInputGetState` for one slot with the
  SDL state of GyroLib's selected controller (`gl_output.physical_id` / `gl_get_selected_device` -> `gl_get_endpoint`
  -> `gl_sdl_endpoint_for_instance` -> `SDL_GetGamepadFromID`, no extra reference), `XInputSetState` -> `SDL_RumbleGamepad`
  (refreshed every 500 ms, 1 s duration), `XInputGetCapabilities`, `XInputEnable`, `XInputGetStateEx` (ordinal 100, adds
  Guide 0x0400). State is built on the game thread right after `gl_update` and read under a mutex by the hooks.
  - Findings: **Luto (UE 5.5.4) imports `XINPUT1_4.dll!XInputGetState/XInputSetState` by name through the delay-load
    table**; `hooks.cpp` only walked the regular import table, so it now also walks `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT`
    (an unresolved slot points to the linker stub inside the module: it is patched before the first call, so the stub
    never runs, and the original is resolved later from `hooks::source_dll()`). The **Windows SDK's `xinput.lib` imports
    by ordinal** (xinput1_3/1_4: 2 GetState, 3 SetState, 4 GetCapabilities, 5 Enable, 100 GetStateEx; xinput9_1_0 uses
    other ordinals but exports by name), hence the new `hooks::add_ordinal` (exact DLL name required).
  - Only game modules are patched, so SDL in GyroLib (which loads XInput dynamically) never sees the virtual pad: no
    feedback loop. That's also why it's off in inline-hook mode (protected exes): an export hook would reach SDL too.
  - UE only re-polls empty XInput slots after `WM_DEVICECHANGE` (`bNeedsControllerStateUpdate`), so GyroUE posts
    `WM_DEVICECHANGE(DBT_DEVNODES_CHANGED)` to the game thread's `UnrealWindow`s when the pad appears or goes. Not yet
    verified in a real UE game.
  - Settings (`GyroUE.ini`, profile overrides): `XInput` 0 off / 1 auto (default) / 2 always, `XInputNintendoLabels`
    (swaps A<->B and X<->Y; the first version used `SDL_GetGamepadButtonLabel`, replaced by an unconditional
    swap so it also works for pads SDL can't label), `XInputTouchpadBack` (touchpad click = View).
  - Auto mode keeps it off when the pad is Xbox-type or has a Steam handle, when any real XInput slot is connected
    (watcher thread, 1 s, so empty-slot polling never costs the game thread), or when the game reads pads itself:
    `libScePad*.dll` or a non-GyroLib `SDL2/SDL3.dll` loaded, `RegisterRawInputDevices` usage page 1 / usage 4,5,8,
    `CreateFileW` on `\\?\hid#vid_<selected pad vendor>`, or `DirectInput8Create` (all observed from game modules only).
    It waits 2 s after the pad appears before showing it.
  - While translating, GyroUE also declares `GL_HOST_NATIVE_STICK_SUPPRESSION` and zeroes the right stick when GyroLib
    asks (`suppress_native_right_stick`, flick stick). Untested.
  - Verified so far: builds clean (/W4); a stand-in exe (`*-Win64-Shipping.exe`, Windows SDK `xinput.lib` with
    `/DELAYLOAD:XINPUT1_4.dll`, an `UnrealWindow`, PeekMessageW loop) gets `XINPUT1_4.dll!XInputGetState (ordinal 2)`
    and `XInputSetState (ordinal 3)` patched in the delay-load table and its calls reach GyroUE. **No controller was
    connected, so the translation itself, rumble and the `WM_DEVICECHANGE` re-poll are not verified yet; nothing run
    in game.**
  - Deployed to Luto (`winmm.dll`, replacing 0.6.19; existing `GyroUE.ini`/`gyrolib.ini` kept, so the new keys
    take their defaults: `XInput=1` auto). Awaiting the in-game test.
  - **Luto test of 0.7.0, DualSense (connection not reported), Epic, no Steam Input: works.** User: every input they
    could try worked. Log: both delay-load slots patched before the first call; `DualSense Wireless Controller shown as
    XInput controller 0` 2.8 s after the SDL reader started, `WM_DEVICECHANGE posted to 1 game window(s)`, and the game's
    first `XInputGetState` call came 1.1 s later (so Luto doesn't poll XInput before its engine loop runs; whether the
    `WM_DEVICECHANGE` was needed isn't proven). No auto-mode false positive (Raw Input, HID, DirectInput) in Luto.
    Frame cost unchanged (idle avg ~58 us, active ~70 us).
  - **Luto, Switch Pro over Bluetooth, no Steam Input: basic input works (user).** `Nintendo Switch Pro Controller shown
    as XInput controller 0` 4.6 s after start, game's first poll 0.7 s later. Default mapping (by position).
  - Decision: ship as **alpha with the default mapping**. Extra buttons stay unmapped for now (DualSense mic button,
    Edge paddles/Fn, Switch Capture; PS/Home only through `XInputGetStateEx`). Ideas parked in "Ideas for later".
  - **Terminator: Resistance (UE4, Steam)** imports `XINPUT1_3.dll` **by ordinal (2 GetState, 3 SetState) in the
    regular import table**: covered by the ordinal hooks. Updated there from 0.6.17 to 0.7.0, kept as `version.dll`
    (alongside PSButtonsUE `dsound.dll` and ReShade `dxgi.dll`). **Tested: works perfectly (user, DualSense)**; log:
    ordinal 2/3 patched in the import table, `DualSense Wireless Controller shown as XInput controller 0`.
- 2026-10-11 **GyroUE 0.8.0 (local test build 0.7.1): native pad libraries detected from the import tables.** The Outer Worlds: Spacer's Choice
  (Epic, UE4, exe `IndianaEpicGameStore-Win64-Shipping.exe`, native DualSense) ships `libScePad.dll` and imports it
  **through the delay-load table** (`scePadInit`, `scePadOpen`, `scePadReadState`, `scePadSetTriggerEffect`), plus
  `XINPUT1_3.dll` by ordinal 2/3 and `RegisterRawInputDevices`. A delay-loaded libScePad only appears in the process on
  the game's first scePad call, possibly after the 2 s grace, so 0.7.0 could show the pad as XInput and then withdraw
  it. New `hooks::imported_dlls()` lists the game modules' regular and delay-load imports; auto mode treats an import of
  `libScePad*` or `SDL2/SDL3.dll` as native support, checked in DllMain (`xinput::install`) and on every watcher pass
  (modules loaded later). Deployed there as `winmm.dll` (replacing 0.6.18). Expected log: `xinput: the game reads
  controllers itself: it imports Sony's pad library (libScePad.dll, delay-load, ...)` and `not shown`.
  - **Tested (user, DualSense, Epic): all good.** Log: native support detected in DllMain from the import table, before
    the pad was found; `DualSense Wireless Controller not shown: it imports Sony's pad library`; the game still polls
    XInput (`XINPUT1_3` ordinals, real devices only), and its native DualSense input is untouched. Generic gyro path
    works (`view=Camera`, `IndianaPlayerCharacter_BP_C`); main menu correctly `view=none` (DefaultPawn, CineCameraActor).

- 2026-10-11 **Hogwarts Legacy (Steam, UE 4.27, exe `HogwartsLegacy.exe`, UE4SS 3.0.1 also installed), GyroUE 0.8.0.**
  - **Engine not readable:** GUObjectArray found (`ObjObjects` at exe+0x97EB7E0, the same array UE4SS finds at
    exe+0x97EB7D0) and FNamePool (header len high bits), but `detect_layout` fails every 5 s with `ue: core classes not
    found` (UClass `Class` itself was found). So no scene, no view, gyro off, and no notice: the "camera ignores the
    gyro" notice needs a view, and the probe needs the same reflection data. Not a regression: this code is unchanged
    since the first commit; the game was never supported. UE4SS reports a standard UObjectBase (Class 0x10, Name 0x18,
    Outer 0x20) but `FProperty::Offset_Internal = 0x4C` (stock 4.25+: 0x44), and resolves names through
    `FName::ToString` rather than the pool, so the name decoding or the property layout may differ.
  - Added: `ue: core classes missing: ...` logs once which of Object/Actor/Controller are missing, how many objects are
    classes and the first 12 class names, and for each missing name the object with that name and its class (to tell
    a name-decoding problem from a class-pointer one). After 60 s without readable engine data, GyroUE logs it and
    shows a 20 s notice (game not supported yet, report it with the log) instead of staying silent.
  - **XInput:** the DualSense was shown as XInput at +3 s, then withdrawn at +18 s when the game opened its HID device
    (`it opens the controller's HID device itself (vendor 1356)`): Hogwarts has native DualSense support. User: the
    notices appeared and the translation stopped correctly. The exe **imports `hid.dll`** (`HidD_GetFeature`,
    `HidD_SetFeature`, `HidD_GetAttributes`, `HidP_GetCaps`...), which stock UE doesn't (Luto doesn't); UE's RawInput
    plugin does, also native support. Auto mode now treats a `hid.dll` import as native, so such games never get the
    XInput pad, not even for those 15 s.
  - Built as 0.8.0 (user's decision: not released yet) and deployed to Hogwarts (`winmm.dll`). Waiting for the log with
    the class diagnostics.
  - **Diagnostics run (user, while the game compiled shaders):** `core classes missing: Controller; 9929 of 96600
    objects are classes (first: Object, p, i, e, s, r, i, ...)`; Object (#1) and Actor (#22) were found. Single letters
    = names decoded from the wrong memory. The 60 s notice fired too (correctly: the data really couldn't be read).
  - **Root cause: a false FNamePool.** Read-only `ReadProcessMemory` look at the running game: the exe image holds three
    qwords equal to name block 0. GyroUE took the first, rva 0x9754380 (CurrentBlock 0, block 1 null, blocks 2+ set
    0x20000000 apart): it isn't the pool, so only block-0 names (Object 0x162, Actor 0x16A) decoded; index 0xFC10C
    (block 15) landed in UTF-16 config text. The real pool is at **rva 0x97AF290** (CurrentBlock 131, blocks every
    0x20000). Name entries are stock (2-byte header, len in the high 10 bits, stride 2).
  - Fix in `find_name_pool`: a candidate must have every block up to CurrentBlock set and the next four null (the
    allocator fills blocks in order). Checked against the live game: both false candidates rejected, the real one
    accepted, and with it Controller (#828), PlayerController, PlayerCameraManager and GameEngine decode. All of those
    exist from startup, so the engine can be read in menus/shader compilation too. Still open: UE4SS reports
    `FProperty::Offset_Internal = 0x4C` (stock 0x44); `detect_layout` measures it at runtime, to be seen in game.
  - **In game with the pool fix (user):** `ue: layout (FField properties, UE 4.25+) SuperStruct=0x40 ChildProperties=0x50
    Field.Class=0x8 Field.Next=0x20 Field.Name=0x28 Property.ElementSize=0x3C Offset_Internal=0x4C sizeof(Property)=0x78`
    (FField is 8 bytes larger than stock from Next on; detected at runtime, no change needed), `ue: engine GameEngine_...`,
    `view: Camera`, `gyro on`. PC `BP_Phoenix_Player_Controller_C` (ControlRotation +0x2B0), pawn `BP_Biped_Player_C`,
    view target **`BP_ThirdPersonCameraStackActor_C`** (gameplay) / `BP_SceneRigCameraStackActor_C` (scene rigs).
    GyroUE writes ControlRotation (applied yaw grows) but no notice appeared: `follow_check` only judged the camera
    when the view target is the pawn (other targets were taken as scripted cameras), and Hogwarts' gameplay camera
    is its own actor. Also seen: calib=0 and bias 0 the whole session, so applied yaw drifted ~0.5 deg/s while the pad
    lay still (no menu calibration happened yet).
  - Fix: `player_camera()` also accepts a view target class whose camera turned 30+ degrees (POV) while the right
    stick was held; logged once as `camera: view target X turns with the right stick: treated as the player's camera`.
    Then the usual 2 s windows decide (notice to set Probe=1 if the gyro is ignored). The auto probe never had the
    pawn-only filter. Built as 0.8.0; Hogwarts was running again, not deployed yet.
  - **Probe run (user, DualSense, Probe=1, 5 scored captures):** `camera: view target BP_ThirdPersonCameraStackActor_C
    turns with the right stick` logged as intended. Roots: PC `BP_Phoenix_Player_Controller_C`, camera manager
    `PhoenixCameraStackManager`, view target `BP_ThirdPersonCameraStackActor_C`. Six rotations followed the camera every
    time; five are outputs (`Camera.TransformComponent0` RelativeRotation and +0x1D0, both POV caches,
    `ViewTarget.Camera(CameraStackCineComponent)+0x1B0`); the pick is raw float rotator **+0x1C8 in
    `ViewTarget.CameraStackBehaviorConvertToTranslation_<n>`**. ControlRotation never followed (ignored by this camera).
  - **The written profile couldn't work:** `Object=ViewTarget.CameraStackBehaviorConvertToTranslation_2147460121`, but
    that number is a per-session generated name (`_2147460128` in the next session), so the object was never found.
    Probe fix: a path step whose name is `<Class>_<digits>` is labelled `@Class` when `@Class` resolves to that same
    object (first object of that class under the same outer, by index, as `find_subobject_of_class` does).
    The deployed profile was edited by hand to `Object=ViewTarget.@CameraStackBehaviorConvertToTranslation`
    (original kept as `HogwartsLegacy.ini.probe-original`), Probe=0. Whether +0x1C8 is an input or another output is
    the open question for the next test.
  - **Test of the @Class profile (user): no camera motion.** The object resolved
    (`view object CameraStackBehaviorConvertToTranslation_2147460037 +0x1C8=(P -5.55 Y 158.49)`), but after the gyro
    added 71.6 degrees the value was back at Y 158.49: +0x1C8 is rewritten every frame, another output.
  - **Read-only memory hunt** (`ReadProcessMemory`, scratch script, game running): roots = the camera-stack objects,
    pawn and PC (0x1000 bytes each) plus every heap block one pointer away (0x600 bytes); 12 snapshots taken while the
    camera was still between stick turns (yaw both ways, pitch -18..+26); floats whose change matched the camera's in
    every turn. Results: PC `ControlRotation` (+0x2B0) tracks the camera exactly (an output, which is why writing it
    did nothing); **`BehaviorStack` (class `CameraStack`, subobject of the view target, also at view target +0x340)
    holds a float rotator at +0x68 (P, Y, R) mirrored at +0x80**, about 3 degrees above the final pitch and slightly
    behind in yaw (the base rotation before the stack's offsets/smoothing), which is why the probe (exact matches
    only) missed it. Others: `CameraStackBehaviorMaintainTargetFraming+0x1D4` (yaw - 20),
    `CameraStackBehaviorAddOffsetsWhileCrouching+0x418` (pitch), CineComponent +0x14C (yaw relative to the actor).
  - **One-shot write proof (`WriteProcessMemory`, user agreed):** +45 yaw at BehaviorStack +0x6C and +0x84: camera
    output -37.26 -> 7.75 and stayed (checked over 3 s); +10 pitch at +0x68 and +0x80: -20.89 -> -10.58, stayed.
  - Profile `profiles/HogwartsLegacy.ini`: `Object=ViewTarget.BehaviorStack`, `Offsets=0x68,0x80`, `RotatorSize=12`
    (the name has no instance number; it resolves as a subobject of the view target). Deployed; needs a game restart
    and a gyro test. Lesson for the probe: a camera's state can be a smoothed/offset copy of the POV, not an exact
    match; a delta-based scan (as above) would find it.

- 2026-10-11 **Probe with live test (built as 0.8.0, not verified in game yet).** Why: the passive probe accepted any
  rotation that held the camera's values and wasn't in a known output location, so outputs elsewhere slipped through
  and became the profile (Hogwarts: `ConvertToTranslation+0x1C8`), while a look rotation kept with an offset was never
  seen (Hogwarts: `BehaviorStack+0x68`, pitch 3 degrees above the POV). User's design: test every candidate by writing.
  - Candidates (`follows()` in ue.cpp): a rotator that holds the POV's exact pitch/yaw, **or changed by the POV's
    turn** (tolerance max(0.6, 3%) per axis; scored turns are 10+ degrees of yaw), reflected or raw, float or double.
    `Candidate.exact` counts the exact hits. `probe_groups()` groups them (raw rotators of one object and size
    together, as a profile writes them; reflected properties one by one), usable first, passive "outputs" after.
  - Live test (host.cpp `run_live_test`, Probe=1 automatic mode only), after 3 scored captures, up to 40 groups:
    camera at rest 600 ms, then a 250 ms null window (no drift > 0.2), write +3 yaw through the same `apply()` a
    profile uses (so the object path, @Class, WorldRotation are exercised too); pass if the POV turns +3 (+-0.5)
    within 700 ms and holds 150 ms, then the turn is undone and the camera must come back; then the same in pitch
    (towards level). Fail: `overwritten (output)` (value rewritten) or `kept, camera didn't move`. Round 2 retests the
    passes; a pass in round 1 only is `inconsistent`. Stick, pause, cursor, focus loss or another view target abort a
    test and undo its turn (pending until it can be written), 3 retries. Notes whether the pawn's own rotation turned.
  - Result: a verified group becomes the profile (or `.suggested.ini` if the game had one), header `VERIFIED LIVE`;
    otherwise up to 8 captures, testing the new candidates each time, then only an unverified `.suggested.ini`. **An
    unverified guess is never written as the game's profile any more** (also for manual F7/F8 diffs). The gyro stays
    paused until the probe is done. On-screen notices guide each step.
  - Deployed to Hogwarts with the working profile set aside (`HogwartsLegacy.ini.verified`) and Probe=1, to check
    that the probe finds and verifies BehaviorStack by itself and rejects +0x1C8.
  - **First run (user, Hogwarts, 3 captures): it verified the wrong camera.** 8 groups tested: MotionCamera raw
    and TransformComponent0 raw `kept, camera didn't move`; the RelativeRotations `overwritten (output)`;
    **`ViewTarget.Camera` (CameraStackCineComponent) raw 0x1B0,0x1D0 MOVES THE CAMERA** in both rounds, written as
    the profile. User: it works, but it is a free camera (no arm: it turns on the spot instead of orbiting the
    character). BehaviorStack +0x68/+0x80 was a candidate in captures 1 and 2 but not in capture 3 (52 degrees of
    pitch: its offset to the POV depends on the pitch, framing), so the old every-capture rule kept it out of the
    test. Also a candidate: `BP_RotationLag_GamepadInput_C` +0x1E0/+0x1EC/+0x1F8 (captures 1-2).
  - Fixes: (1) groups need most scored diffs, at least two (`max(2, (n+1)/2)`), since the live test decides; (2)
    **orbit check**: every capture measures how far the camera's POV location moves for the turn's view angle
    (radius = chord / (2 sin(angle/2)), median of the captures, logged); with a radius of 30+ units (third
    person) a candidate's +3 yaw must also move the camera at least 40% of the expected chord, else verdict
    `kTurnsInPlace` (turned the view in place, not like the stick). First-person cameras (radius about 0) skip it.
    `ue::camera_pov_location` reads `CameraCachePrivate.POV.Location` (float or double). Redeployed; the free
    camera profile kept as `HogwartsLegacy.ini.probe-freecam`, Probe=1 again.
  - **Second run (user, Hogwarts, DualSense, 3 captures): correct.** Orbit radius 235/205/202 (median 205, third
    person). 15 groups tested: **`ViewTarget.BehaviorStack` raw 0x68,0x80 MOVES THE CAMERA in both rounds (yaw and
    pitch; camera moved 10.7 / 10.8 for an expected 10.6)**; `ViewTarget.Camera` raw 0x1B0,0x1D0 rejected as turning in
    place (moved 0.0 of 10.6); outputs rejected as overwritten (ConvertToTranslation +0x1C8, PC ControlRotation and
    +0x8F0, BP_RotationLag_GamepadInput_C +0x1E0/+0x1EC/+0x1F8, RelativeRotations); MotionCamera and
    TransformComponent0 raw kept without effect. The generated profile equals the hand-verified one. Fixed the
    log label of kTurnsInPlace (it printed "not testable").
  - **Regression check, Severed Steel demo (UE 4.27.2, first person), no profile, Probe=1, 3 captures (user: correct,
    profile works).** Installed fresh as `winmm.dll` (the game's 14.24 `msvcp140.dll`/`msvcp140_2.dll` renamed to
    `.bak` again, user agreed; the repo profile set aside as `.verified`). Orbit radius 0 in all captures (turns on the
    spot), so no orbit check. 16 groups: **`Pawn.FPCamera.RelativeRotation` (WorldRotation=1)** and `Pawn.FPCamera` raw
    0x190,0x1B0 both MOVE THE CAMERA in both rounds (noted: also turns the character, expected in first person); the
    named property ranks first, so the profile equals the verified one. Outputs (Pawn raw 0x818,0x1284,
    ReplicatedControlRotation, RIghtHandGripLoc, ControlRotation) overwritten; spring arm and TransformComponent0 raw
    kept without effect. The live test exercised the WorldRotation write path.
  - **Regression check, Cronos (UE 5.4.4, third person, double rotators), no profile, Probe=1, 3 captures (user: works).**
    First attempt ran the old 0.6.19: the folder had GyroUE 0.6.19 as `version.dll` (loaded first) and 0.8.0 as
    `winmm.dll` (stayed inactive, one GyroUE per process), so the old probe wrote an unverified profile after two
    captures. Redeployed 0.8.0 as `version.dll` (the script removed `winmm.dll`). Orbit radius 190/166/194. 6 groups:
    **`Pawn.View` raw 0x2A0,0x420 (RotatorSize 24) MOVES THE CAMERA in both rounds (camera moved 9.8 for an expected
    9.6)**; SpringArm raw 0x2D8, Animation.LastAimRotation, TransformComponent0.RelativeRotation overwritten. The
    profile equals the verified one. With Hogwarts and Severed Steel: the live-test probe reproduced all three
    verified profiles by itself (third person / first person with WorldRotation / UE5 double raw rotators).

- 2026-10-12 **Luto, GyroUE 0.8.0 (local build): flick stick and `XInputNintendoLabels` tested (user: works as it
  should).** `XInputNintendoLabels=1` now swaps A<->B and X<->Y unconditionally (any controller) instead of relying on
  `SDL_GetGamepadButtonLabel`. Tested with a DualSense (swap applied), Epic, no Steam Input: `XInput=1 (auto)
  XInputNintendoLabels=1`, `DualSense Wireless Controller shown as XInput controller 0`. GyroLib flick stick
  (`context.1.flick.mode=2`) works with the translated pad: `GL_HOST_NATIVE_STICK_SUPPRESSION` is advertised while the
  pad is shown as XInput and the virtual pad's right stick is zeroed when GyroLib asks (`suppress_native_right_stick`).

- 2026-10-12 **`XInput=2` now keeps the game from reading PlayStation/Nintendo controllers itself.** Before, `XInput=2` in a game with native DualSense support gave double input (native + translated).
  - Finding (`dumpbin /imports`): `libScePad.dll` (Pacific Drive, It Takes Two) imports exactly the `hid.dll` set that
    Hogwarts Legacy, Cronos, Banishers, Days Gone and Jedi: Fallen Order import from their exe (HidD_GetAttributes/
    GetFeature/SetFeature/Get*String/GetHidGuid/GetPreparsedData, HidP_GetCaps/GetValueCaps, plus SetupDi*, ReadFile,
    GetOverlappedResult(Ex)): those five most likely link Sony's pad library statically. Either way the controller is
    opened with `CreateFileW` on its HID path.
  - Change: with `XInput=2` the `CreateFileW` hook (now installed in auto and always modes, and also patched into a
    separate `libScePad*.dll` through `hooks::add_with_pad_library`) refuses HID paths of Sony (054C) and Nintendo
    (057E) devices and of the selected controller with `ERROR_ACCESS_DENIED`; `RegisterRawInputDevices` entries for
    joysticks/gamepads are dropped. HID path IDs are now parsed for Bluetooth paths too
    (`_vid&0002054c_pid&0ce6`; before, only USB `vid_054c` was recognised by the auto-mode HID check).
  - `XInput=2` now also stays off while a real XInput controller is connected (DS4Windows/DSX, Steam Input, Xbox pad):
    the same pad through a remapper would act twice.
  - **Tested, DualSense (user: works in both, no double input).** Pacific Drive (`XINPUT1_3`): `libScePad.dll` loads
    after the controller is shown (+5 s) and is patched as it loads (`hooks: CreateFileW patched in the pad library
    module`), then its first opens of `054C:0CE6` are refused 0.1 s later. Cronos (`XINPUT1_4` delay-load, Sony's
    library in the exe): the exe's opens of `054C:0CE6` are refused right after its first `XInputGetState`. Both
    show `DualSense Wireless Controller shown as XInput controller 0`. Bluetooth not tested yet.

- 2026-10-12 **Native flick stick (`NativeFlickStick=1`, off by default (user), new `nativepad.cpp`).** Goal: GyroLib's
  flick stick in games with their own DualSense support without giving it up for `XInput=2`. Switching between
  native and XInput at runtime was rejected: the game keeps its HID handle open (a switch means faking a disconnect,
  possibly a "controller disconnected" pause), and GyroLib's `suppress_native_right_stick` changes with views and the
  aim context, so it would switch on every aim.
  - All eight native-DualSense games checked read through Sony's pad library (separate or linked in), which opens the
    pad with `CreateFileW` and reads with `ReadFile` + `GetOverlappedResult(Ex)` (It Takes Two's older
    `libScePad.dll` imports only `ReadFile`: synchronous reads, handled too).
  - `nativepad.cpp` now owns the `CreateFileW` hook (moved from `xinput.cpp`: XInput=2 refusal and auto mode's
    opened-vendor check use it through `nativepad::refuse_pads` / `opened_vendor`) and adds `ReadFile`,
    `GetOverlappedResult(Ex)` and `CloseHandle` (game modules + `libScePad*.dll`). Sony handles opened with read
    access are followed (max 8; a handle value reused by another `CreateFileW` or closed is dropped); pending
    overlapped reads are matched by `OVERLAPPED*` and handled when `GetOverlappedResult(Ex)` reports them done.
  - Layouts: `0x01` RX/RY bytes 3-4, `0x31` (DualSense BT) 4-5 + CRC32 at 74, `0x11` (DS4 BT) 5-6 + CRC32 at 74 (CRC
    of 0xA1 + bytes 0..73). Reports whose CRC doesn't verify are left alone. The layout counts as checked when the
    report's right stick agrees with SDL's (byte*257-32768, tolerance 48) in >= 15 deflected frames (and >= 4x the
    misses); 30+ misses dominating -> never suppressed for that pad. Only then `GL_HOST_NATIVE_STICK_SUPPRESSION`
    is declared (host: `xinput_shown || native`), and while GyroLib asks, RX/RY in the game's copy become 128.
  - **Pacific Drive, first test (DualSense USB, `XInput=1`, `NativeFlickStick=1`): flick works, but the stick still
    turned the camera too** (holding it up pitched the camera up while the flick rotated). Log: libScePad opens
    `054C:0CE6` (opens, closes, reopens 0.5 s later), reads overlapped, done at once, **640 bytes per read = ten 64-byte
    USB reports**; only the first was centred, so the other nine still carried the stick. Layout check passed (15/15),
    suppression on, 28156 reads changed. Fix: the device's `InputReportByteLength` (`HidP_GetCaps` on the game's
    handle, fallback 64/78 by report ID) splits each read, and every report in it is centred.
  - Pacific Drive, second test (same setup): `64-byte input reports`, reads of 768 bytes (12 reports), every report
    centred (~490/s, matching the pad's USB rate), 15/15 layout check; suppression follows GyroLib (centred on foot
    and in the car, passed through in pause and main menu).
  - **Cronos (Sony's library linked into the exe, DualSense USB, `XInput=1`): works (user).** Same read pattern as
    `libScePad.dll`: opens `054C:0CE6` (closes and reopens at once, same handle value), overlapped reads of 640 bytes
    (10 x 64), 15/15 layout check, suppression on/off with gameplay and menus; the game closed the pad on exit and
    suppression was withdrawn.
  - Still to verify: Bluetooth (reports `0x31` + CRC), DualShock 4, a game with the separate DLL over Bluetooth.

- 2026-10-13 **Mortal Shell (Epic, UE 4.25+, exe `Dungeonhaven-Win64-Shipping.exe`): DualSense shown as a DualShock 4
  (`DualSenseAsDualShock4=1`, new in `nativepad.cpp`, USB only). Works in game (user: almost every button checked).**
  - Why: the game links an old Sony PC pad library (`WinDualShock` plugin, same `hid.dll` imports as `libScePad.dll`)
    that knows only the DualShock 4 (PID 05C4 x147 / 09CC x43 / 0CE6 x8 in the exe). It opens the DualSense for reading
    but ignores it. `XInput=2` can't help: the game never calls `XInputGetState` while its Sony plugin is active (the
    exe's only call site, UE's `XInputInterface::SendControllerEvents`, never ran, even with a real XInput pad that the
    game does use some other way; not investigated further).
  - What the library checks before accepting a DualShock 4 (disassembly of the exe): `HidD_GetAttributes` PID;
    `HidP_GetCaps` Input >= 64, Feature >= 0x35; feature value caps (`HidP_GetValueCaps(HidP_Feature)`) must contain
    (page 0xFF80, usage 0x43, report 0xA3, count 0x30) and no (0xFF04, 0x27, 0x06, 0x34) entry, and a device field
    >= 0x13 (probably the feature caps count); feature 0x12 (address, 16-byte buffer), 0xA3 (firmware, 49: hardware
    version LE16 at 0x23 must be >= 0x3100; date/time strings at 1/17, other versions at 0x21/0x25/0x29/0x2B/0x2D),
    `SetFeature 0x14`, feature 0x02 (calibration, 37). On rejection its cleanup path frees uninitialised pointers:
    heap corruption crash (0xC0000374). First crash also had GyroUE zero-filling the caller's whole stated buffer
    length: now only the DualShock 4 report size is written.
  - Emulation: identity 054C:09CC "Wireless Controller"; caps 64/32/64 with 27 synthetic feature value caps; 0x02 from
    the DualSense's 0x05 (same layout), 0x12/0x81 address from 0x09, 0xA3 synthetic; other IDs fail; SetFeature
    swallowed. Input: each 64-byte DualSense report rewritten in place as a DualShock 4 USB report (sticks, buttons,
    triggers, IMU, timestamp /16, touch, cable/battery). Output: DS4 0x05 (motors 4-5, RGB 6-8) -> DualSense 0x02
    (48 bytes: enable bits, motors 3-4, RGB 45-47) with GyroUE's own buffer; the game's request completed as written.
  - Log: game reads 7 x 64-byte reports per synchronous `ReadFile`; layout check for native flick passed (15/15).
  - Diagnostics added on the way (kept): `input:` lines for every Raw Input registration, HID device opened (with or
    without read access) and HID device the game gets Raw Input reports from; every followed HID handle's first read.
  - **Bluetooth works too (user: functions checked).** Over Bluetooth the game first skipped the pad: the library
    accepts 054C:05C4/09CC only with `HidD_GetAttributes` VersionNumber, read as decimal digits, >= 23 (a USB DualSense
    passes; Windows reports 0x0000 over Bluetooth), so the emulation now reports 0x0100. The game's buffers use the
    spoofed 64-byte input length while the device's is 78: GyroUE reads into its own buffer (same report count) and
    converts 0x31 (USB layout shifted by one; 0x01 basic report also handled). Output: 0x31, tag 0x02, effects one
    byte later than USB, CRC32 seed 0xA2 at 74; the device's output length is 547. Feature reads use the device's
    feature length over Bluetooth. Log: 8 x 64 bytes per synchronous read after conversion, layout check 15/15.
  - Profile `profiles/Dungeonhaven-Win64-Shipping.ini`: `[GyroUE] DualSenseAsDualShock4=1`, generic camera (confirmed
    in game: `camera: confirmed ... ControlRotation`). No settings panel (DX11).
  - To do: Jedi: Fallen Order (same library imports).

- 2026-10-13 **Auto mode: Sony-library signs only apply to PlayStation controllers (not tested in game yet).** Before,
  a game importing `libScePad`/`hid.dll` (Hogwarts, Cronos, Mortal Shell, Jedi) also kept a Switch Pro or a DirectInput
  pad from being translated, though its Sony library ignores them: no input, no Xbox prompts. Now `native_sign` keeps
  two kinds: Sony (libScePad imported/loaded, hid.dll imported) blocks only vendor 054C; Any (own SDL, Raw Input
  gamepad registration, DirectInput, the game opening the pad's HID device) blocks every pad. The HID-open sign is
  ignored when a Sony library is present: its enumeration opens every HID device for reading (Mortal Shell log:
  keyboards, lighting controllers). Caveat: in games that never call XInputGetState while their Sony plugin is active
  (Mortal Shell) the translated pad got no input: that was the EOS overlay undoing GyroUE's XInput hook (fixed, see below).
  - **Jedi: Fallen Order (Steam), Switch Pro, `XInput=1`: works (user).** Log: `the game reads PlayStation controllers
    itself: it imports the HID library`, then `Nintendo Switch Pro Controller shown as XInput controller 0`, game polls
    XInputGetState 8 s later; its Sony library opens 057E:2009 once while enumerating and closes it. Jedi reads XInput
    alongside its Sony library (unlike Mortal Shell). The earlier DualSense-as-DualShock 4 test there (user, log not
    kept) also worked.

- 2026-10-13 **Mortal Shell: XInput translation works; the Epic Online Services overlay was undoing GyroUE's hook.**
  Switch Pro, `XInput=1`: shown as Xbox controller and the game responds (user: works perfectly).
  - Correction of the earlier finding: the game does poll XInput (UE's `XInputInterface`, call at exe+0xE7FA3B) all the
    time. `XInputTrace=1` (new diagnostic: MinHook inline hooks on `XInputGetState`/`Ex` in every loaded xinput DLL,
    each calling module logged once) showed the exe calling `xinput1_3!XInputGetState` directly, not through GyroUE.
  - Cause: `EOSOVH-Win64-Shipping.dll` (EOS overlay) rewrites the exe's import slots for `XInputGetState` and
    `LoadLibraryW/A/ExW/ExA` about 2 s after start, after GyroUE patched them (its XInput hook tail-jumps to the real
    export, so the return address stays in the exe). The DSX-emulated Xbox pad worked because it is a real XInput pad.
  - Fix: `hooks::verify()` (watcher thread, every second) checks every slot GyroUE patched; a rewritten slot is logged
    once with its new target and, for XInput imports only, patched again (`patched again` / `left to it`). Our hook
    keeps calling the real export, so EOS's XInput hook is bypassed (overlay gamepad input not checked).
  - Log: `hooks: the import of XINPUT1_3.dll!XInputGetState was written back by something else (now
    EOSOVH-Win64-Shipping.dll+0x251d0); patched again`, then `the game polls XInputGetState` 0.9 s later and
    `Nintendo Switch Pro Controller shown as XInput controller 0`.
  - Any EOS game (Epic Store) can do this; before the fix the translation silently never reached them.

- 2026-10-13 **DualSense as DualShock 4 is now learned, not a user setting. Tested in Mortal Shell (DualSense over
  Bluetooth, no profile, auto mode): first start showed the notice and the DualSense worked as an Xbox controller
  (user); GyroUE created `GyroUE\profiles\Dungeonhaven-Win64-Shipping.ini` with its header and the key; second start
  loaded it (`1 view(s)`, generic camera) and the game used the DualSense as a DualShock 4 (calibration, rumble and
  light bar to report 0x31, converted reads).**
  - Experiment B (switch identity in the same session), Mortal Shell, DualSense over Bluetooth: started with the real
    identity; 5 s after the game's look (open, HidD_GetAttributes, close) GyroUE refused its opens for 3 s, then showed
    the DS4 identity. The game never opened the DualSense again (0 refusals), also with `WM_DEVICECHANGE`
    (DBT_DEVNODES_CHANGED) posted to its window at both steps: its Sony library only looks for controllers at startup
    (two passes 0.5 s apart; the exe imports no RegisterDeviceNotification). Dead end without RE of its scan loop.
  - Option C: `nativepad::dualsense_ignored()` = the game opened a DualSense, never read one, and has none open 5 s
    after the first open (a supporting library keeps its handle and reads at once: Pacific Drive 0.5 s, Cronos 20 ms).
    With a Sony library among the imports (`xinput::sony_library()`, auto mode), the host writes
    `[GyroUE] DualSenseAsDualShock4=1` into `GyroUE\profiles\<exe>.ini` (created with a header if missing), shows a
    15 s notice (restart to use it as a PlayStation controller) and the auto mode translates that DualSense to XInput
    for the session. `DualSenseAsDualShock4` left GyroUE.ini and the user docs; it stays a profile key.
  - The probe now keeps the profile's `[GyroUE]` section when it rewrites the profile, and a profile without camera
    views (no `[View.Default]`, no `Views`) doesn't count as an existing profile for it.
  - DualSense handles are now always followed (read hooks always installed), to see whether the game reads them.

- 2026-10-13 **Build: GyroUE 0.8.1** (version chosen by the user), `build/release/GyroUE-v0.8.1.zip`. Since 0.8.0:
  DualSense shown as a DualShock 4 to games whose Sony library only knows the DS4 (USB and Bluetooth), learned
  automatically and saved in the game's profile (notice: restart); Mortal Shell profile; auto mode translates non-Sony
  controllers (Switch Pro, DirectInput) also in games with a Sony library; GyroUE takes back XInput imports an overlay
  rewrites (Epic Online Services overlay in Mortal Shell); diagnostics `XInputTrace=1` and `input:` log lines.

- 2026-10-15 **Gyro4All refactor: engine-neutral core + engine modules** (no version bump yet, still 0.8.2; names,
  files and install layout unchanged: `winmm.dll`, `GyroUE\`, `GyroUE.ini`, logs). `src/GyroUE/` became:
  - `src/core/` (CMake object library `gyro_core`, no include path to `engines/`): dllmain, proxy, hooks, overlay,
    xinput, nativepad, notify, deps, log, perf, plus new `engine.hpp/.cpp` (the `Engine` interface and
    `select_engine`), `settings.cpp` (GyroUE.ini + profile `[GyroUE]` overrides, from host.cpp), `mem.cpp` (guarded
    reads and module sections, from ue.cpp), `util.hpp` (`split`, `wrap180`), and an engine-neutral `host.cpp`.
  - `src/engines/unrealEngine/` (`gyro_unreal`): `ue.cpp` (reflection, unchanged apart from mem/util),
    `ue_engine.cpp` (`UnrealEngine`: detection, scene, profile views, apply, follow check, UE status lines),
    `ue_probe.cpp` (probe, live test, profile writing).
  - `src/engine_registry.cpp`: the engines linked in; `src/CMakeLists.txt` now holds the version.
  - Engine-specific bits moved behind `Engine`: the modular-build DLL prefix (`hooks::init(prefix)`), the window
    class that gets WM_DEVICECHANGE (`UnrealWindow`, `xinput::Settings::window_class`), the Probe= block of a new
    GyroUE.ini (the generated file is byte-identical to 0.8.2's).
  - Engine choice in DllMain: `Engine=<name>` in `[GyroUE]`, else the first module whose `detect()` matches (Unreal:
    `*-Win64-Shipping.exe`/`*-WinGDK-Shipping.exe`, or `..\..\Content\Paks` / `..\..\..\Engine\Binaries` next to
    the exe's folder), else Unreal. New log line `engine: Unreal (detected)`.
  - Log changes: `settings: Probe=%d Notices=%d` is now two lines (`settings: Notices=...` from the core,
    `settings: Probe=...` from the Unreal module); the status `addresses`/`view object` lines now come before the
    `motion` line.
  - `tools/build-gyroue.sh` builds from `src/` and reconfigures when the cache was made from another source folder
    (the copied `build/GyroUE` pointed at the original GyroUE checkout; it was set aside as `build/GyroUE.stale-copy`).
  - Verified: clean /W4 build, same 197 DLL exports as the pre-refactor build, same release zip file list.
    **Not yet tested in game.**

- 2026-10-15 **Build: Gyro4All 0.9.0** (version chosen by the user): GyroUE renamed to Gyro4All, on top of the
  core/engine split above. `build/release/Gyro4All-v0.9.0.zip`.
  - Renamed everywhere except history (versioned "GyroUE x.y" mentions, this journal) and the GitHub URLs in README
    (the repository keeps the name gfgerard/GyroUE for now, user's decision): install folder `Gyro4All\`, `Gyro4All.ini`, `Gyro4All.log`, `Gyro4All-probe.log`,
    INI section `[Gyro4All]`, notices, version resource (ProductName Gyro4All), session mutex `Local\Gyro4All-<hash>`,
    namespace `gyro4all`, macros `GYRO4ALL_*`, scripts `tools/{build,package,deploy}-gyro4all.sh`, build dir
    `build/Gyro4All`, zip `Gyro4All-v<version>.zip`, CMake project `Gyro4All`. The DLL is still `winmm.dll`/`version.dll`.
  - Compatibility with GyroUE installs: `settings::read` takes a key from `[Gyro4All]`, else `[GyroUE]` (Gyro4All.ini
    and profiles); writes go to `[Gyro4All]`, which wins (Probe=0 after a verified probe, DualSenseAsDualShock4). The
    probe keeps both sections when it rewrites a profile. On start, `settings::migrate_from_gyroue` copies
    `GyroUE\GyroUE.ini` -> `Gyro4All\Gyro4All.ini`, `gyrolib.ini` and `profiles\*.ini` (never over existing files, so
    newer shipped profiles win), then renames `GyroUE\` to `GyroUE.migrated` (log line `settings: GyroUE folder found`).
    Gyro4All also takes GyroUE's mutex (`Local\GyroUE-<hash>`, same hash): an old GyroUE proxy left under the other
    proxy name loads into the same process and stays a plain proxy (or Gyro4All does, if GyroUE got there first).
    `deploy-gyro4all.sh` recognises both DLLs as ours (`(Gyro4All|GyroUE) x.y.z in`), and `remove` keeps
    `GyroUE*` folders.
  - Verified: clean /W4 build, same 197 exports, version resource reads Gyro4All 0.9.0; a test exe (scratch, not in
    the repo) ran the migration on a fake install: 10/10 checks (copies, rename, legacy section reads, [Gyro4All]
    wins, shipped profile kept, second run no-op). **Not tested in game.**

- 2026-10-15 **RE Engine recon: Resident Evil 7 biohazard (Steam)**, first non-Unreal target. Static analysis only
  (exe read from disk, nothing installed, game not run).
  - **Build**: `re7.exe` FileVersion/ProductVersion 1.0.0.7, PE timestamp 2026-03-24, the post-2022 "RT"/DX12 build
    (`re7_config.ini` `[Render] Capability=DirectX12`, Agility SDK in `D3D12\`, `dxilconv7.dll`). Data in
    `re_chunk_000.pak` (+ `.patch_00N.pak`, all 0 bytes in this install).
  - **Engine version**: RE Engine has no public version number. The usable version is the **type database (TDB)**
    version: header `TDB\0` + u32 version, embedded in the exe's `.data` (file offset 0x7206e40). **RE7 = TDB 70**
    (the pre-RT DX11 build was TDB 49; REFramework `shared/sdk/TDBVer.hpp`/`GameIdentity.cpp`: RE2/RE3 RT 70, RE8 69,
    RE4/SF6 71, DD2 73, MHWilds 81, RE9 83). Layout of types/fields/methods depends on that version (REFramework's
    `TYPE_INDEX_BITS`/`FIELD_BITS` per version). This is our equivalent of the UE version.
  - **Protection**: no Denuvo/Enigma sections; `.bind` section = SteamStub (Steam DRM wrapper). `.text` entropy 6.19
    (not encrypted on disk). REFramework has no integrity-check bypass for RE7 (only RE3/RE8/MHRise/SF6 need one).
    No anti-cheat. To check in game: whether IAT hooks from DllMain survive SteamStub (it should not rebuild the IAT).
  - **Loading**: imports `WINMM.dll` (2 functions), `VERSION.dll` (3), `DINPUT8.dll` (REFramework's proxy name),
    `XINPUT1_3.dll` (4), `USER32!PeekMessageW`, `dxgi`, `d3d11` (1); D3D12 loaded dynamically; `HID.DLL`+`SETUPAPI`
    (native PlayStation pad support); delay-loads `steam_api64.dll` and `openvr_api.dll` (not shipped). So the
    standalone `winmm.dll` proxy and the per-frame `PeekMessageW` hook should work as in UE games; **REFramework is
    not needed** (it is a `dinput8.dll` loader + RE Engine SDK + Lua; MIT, so its SDK is a usable reference, like
    UE4SS was for Unreal). REFramework source studied at commit d052e8f (2026-10-10).
  - **Reflection, the UE equivalents**: TDB (types, fields with offsets, methods; found by its `TDB\0` magic + version
    in the exe image, like GUObjectArray by shape); managed singletons (REFramework `REGlobals.cpp`: pattern
    `48 8D ? ? ? ? ? 48 B8 00 00 00 00 00 00 00 80`, fallback: TDB types deriving from SingletonBehavior); native
    singletons (`via.SceneManager`, `via.Application`); VM context (`REContext.cpp`, pattern
    `48 8B 0D ? ? ? ? BA FF FF FF FF E8`). Gameplay objects are `via.GameObject` with component lists.
  - **RE7 camera (from REFramework's `scripts/re8_vr.lua`, `utility/RE7.lua`, `src/mods/vr/games/RE8VR.cpp`)**:
    local player `app.ObjectManager` (managed singleton) field `PlayerObj`; camera component `app.PlayerCamera`
    (DLC: `app.CH8PlayerCamera` Not a Hero, `app.CH9PlayerCamera` End of Zoe), updated in `lateUpdate`. Fields:
    `<CameraRotation>k__BackingField` (quaternion, no shake), `CameraRotationWithMovementShake`,
    `CameraRotationWithCameraShake`, `PrevCameraRotation`, `CameraCtrlParam.CameraRotation`, `BaseTransSolver`
    (`CurrentController` -> `<rotation>k__BackingField` quaternion, `IsVerticalRotateLimited`; `CameraControllers[]`;
    `<currentType>k__BackingField`, 0 = MaximumOperatable = player has the camera, else scripted/cutscene).
    Pitch of the upper body: `app.PlayerUpperVerticalRotate.doLateUpdate`. Pad: `app.Pad.update`. Final render
    rotation: primary camera (`via.SceneManager` main view) GameObject transform and its joint 0.
  - **TDB string pool (this build)** also has the PSVR plumbing: `ExternalCameraRotation`,
    `ExternalCameraRotationEnable`, `<TrackingRotation>k__BackingField`, `<CameraRotationWithTracking>k__BackingField`,
    `IsTrackingApplyDisable`, `HmdTrackingController`, `GamePadTrackingController`, `IsVRMode`; and native pad types
    `DualSense`, `DualSenseDevice`, `PSGamePadDevice`, `DualShock4RemotePlay`. Field names alone; owners and offsets
    still to be resolved from the TDB.
  - **Other references**: Nexus RE7 mod 166 (Clocktown, REFramework): separate H/V controller sensitivity; notes the
    game's ~25% stick deadzone. No existing gyro mod for RE7 on PC found.
  - **Plan (not started)**: (1) offline `tdbdump` tool (TDB 70 parser after REFramework's `RETypeDB.hpp`) to list
    `app.PlayerCamera`/controller fields with offsets; (2) `src/engines/reEngine/`: detect (`re_chunk_000.pak` next to
    the exe + `TDB\0` in the image), TDB reader, singleton lookup, scene = ObjectManager.PlayerObj ->
    `app.PlayerCamera`; (3) apply = rotate the controller's persistent `<rotation>` quaternion (yaw about world up,
    pitch about local right), the analogue of ControlRotation; verify live which field the game integrates the
    stick into, and test `ExternalCameraRotation` as an additive alternative; (4) gate: `currentType != 0`
    (cutscene), pause/menus via `via.Application`/GUI managers. RE Engine rotations are quaternions (UE: rotators).

- 2026-10-15 **`tools/tdbdump.py`: offline RE Engine TDB dumper** (Python, stdlib only; reads the exe, writes nothing
  in the game folder). TDB 69/70 only, layouts from REFramework: header `tdb70::TDB` (TDB 70 has an extra pointer at
  0xC0, so the string pool pointer is at 0xD0; 0xC8 in TDB 69), type definitions `RETypeDefVersion69` (0x50 bytes,
  18-bit type ids), `RETypeImpl` 0x30 (name/namespace offsets, `num_member_methods` u16 at 0x12, `num_member_fields`
  i32 at 0x14), `REField` 8 bytes (declaring:18, impl_id:20, **offset:26**), `REFieldImpl` 12 bytes (flags u16 at 2,
  field_typeid:18, name_offset:30), `REMethodDefinition` 16 (impl_id at bit 18, code pointer), `REMethodImpl` 12
  (name at 8). String offsets are masked with the next power of two above the pool size minus 1.
  - **On disk the TDB is not initialised** (`initialized` = 0 at +8): its table pointers are offsets from the TDB
    header (types at +0x300); method code pointers and `managed_vt` are 0 (filled at startup). So offline we get
    names, types and field offsets, not code addresses nor the object's field base (`get_fieldptr_offset`, an int32
    before the managed vtable). The runtime module must read the initialised TDB in memory for those.
  - RE7 (TDB 70): 78687 types, 147763 fields, 507723 methods; a full dump (`--all`) runs without errors. Validated
    against REFramework's RE7 script: every name it uses for RE7 exists with that exact spelling
    (`_CurrentParamContainer`, `CurrentController` without backing field, lowercase `<rotation>k__BackingField`).
  - **Field base (not yet verified live)**: offsets are from the managed field start. In TDB 70 (REFramework
    `ReClass_Internal_RE2_TDB70.hpp`, also used for RE7 RT) `REManagedObject` is 0x10, `REComponent` 0x30
    (ownerGameObject at 0x10), `REBehavior` 0x48, `REGameObject.transform` at 0x18. Expected: field base 0x48 for
    behaviours (`app.PlayerCamera`, `app.ObjectManager`), 0x10 for plain objects (`BaseTransformSolver`,
    controllers). Confirm in game with the int32 before `managed_vt`.
  - **RE7 camera objects (offsets from the field base)**:
    - `app.ObjectManager` (<- `app.SingletonBehavior`1`, static `_Instance`): `PlayerObj` +0x38 (`via.GameObject`).
    - `app.PlayerCamera` (<- `app.DoomsBehavior` <- `via.Behavior`; DLC `app.CH8PlayerCamera`/`app.CH9PlayerCamera`
      derive from it): `<CameraPosition>` +0x20 vec3, `<CameraRotation>` +0x30 quat, `<CameraFov>` +0x40,
      `<OperatedCameraRot>` +0x60 quat, `<DesiredCameraRot>` +0x70 quat, `<inputHorizontalRotationSpeed>` +0x84,
      `<inputVerticalRotationSpeed>` +0x88, `<deltaHorizontalRotation>` +0x8C, `<deltaVerticalRotation>` +0x90 (floats:
      the per-frame stick delta, a candidate injection point), `<IsControlledByAnimation>` +0x94, `<controlRate>` +0x98,
      `Camera` +0xA8 (`via.Camera`), `_CameraJoint` +0x1A0, `_CurrentXRotateLowerLimit`/`Upper` +0x1B0/+0x1B4,
      `AdditionalHorizontalRotation` +0x2A4, `AdditionalVerticalRotation` +0x2A8 (floats, another candidate),
      `OldCameraRotation` +0x2D0, `CameraRotationWithMovementShake` +0x490, `CameraRotationWithCameraShake` +0x4B0,
      `IsGunAim` +0x4D0, `ShootRay` +0x4E0, `LookRay` +0x500, `CameraCtrlParam` +0x538, `BaseTransSolver` +0x540,
      `CameraType` +0x548, `PrevCameraRotation` +0x570. Methods (names only): `addHorizontalRotation`,
      `addVerticalRotation`, `calcDeltaHorizontalRotation`, `calcDeltaVerticalRotation`, `updateOperatedCameraAngle`
      (vtable 25), `lateUpdate` (vtable 6), `update` (vtable 15), `resetRotation`, `interpRotation`.
    - `app.PlayerCamera.BaseTransformSolver`: `<localPosition>` +0x0, `<rotation>` +0x10 quat, `<currentType>` +0x20
      (`CameraTypeEnum`), `<prevType>` +0x24, `CameraControllers` +0x30, `PreviousController` +0x48,
      `CurrentController` +0x50, `Param` +0x90. Methods `update`, `changeType`, `resetRotation`, `updateVrMode`.
    - `app.PlayerCamera.CameraTypeEnum` in declaration order: MaximumOperatable, PivotRotation, ElasticRotation,
      ElasticRotationForCounter, HalfAnimation, HalfAnimationNoResetAngle, FullAnimation, InterpRotation2FullAnim,
      Return2MaxOperatable, QuickTurn, Num (values not decoded; REFramework treats 0 as MaximumOperatable).
    - `app.PlayerCamera.CameraController` (base of `MaximumOperatable`, `QuickTurn`, `Return2MaxOperatable`,
      `AnimationBase` -> `ElasticRotation`, `FullAnimation`, `HalfAnimation`, `PivotRotation`,
      `InterpRotation2FullAnim`; DLC `CH9MaximumOperatable`): `<rotation>` +0x10 quat, `<weight>` +0x20, `<state>` +0x24,
      `Param` +0x30, `XRotateLowerLimit`/`Upper` +0x74/+0x78, `IsVerticalRotateLimited` +0x7C,
      `ControlledCameraAngleOffset` +0x90 vec3. `MaximumOperatable` adds `InvParentWorldMat` +0xA0, `IsAimAssistEnd`
      +0xF8, PSVR stick fields; methods `updateOperatedCameraAngle` (vtable 8), `calcAngleX`,
      `updateOperatedCameraAimAssist`.
    - `app.PlayerCamera.CameraControllerParam`: `IsCameraNoneOperatable` +0x30, `CameraRotation` +0x40 quat,
      `CameraParentWorldMatrix` +0x50 mat4, `HorizontalRotationSpeed` +0x90, `VerticalRotationSpeed` +0x94,
      `RotationSpeedRate` +0x98, `CurrentXRotateLowerLimit`/`Upper` +0xA0/+0xA4, `AdditionalRotation` +0xB0 vec3,
      `IsAimAssistEnabled` +0x178.
    - `app.PlayerUpperVerticalRotate` (<- `app.PlayerBase` <- `app.DoomsUpdater`): `PlayerCamera` +0x70,
      `CameraRotation` +0x250 quat (body follows the camera; not an input).
    - `ExternalCameraRotation`/`Enable` belong to `app.SequenceTrackObject.VrTrack` (cutscene VR track), and
      `<TrackingRotation>`/`<CameraRotationWithTracking>` to `app.vr.HmdTrackingController`: VR-only, **not** a gameplay
      input path. Drop them from the plan.
  - **Apply candidates, to test live in this order**: (a) add to `AdditionalHorizontalRotation`/`AdditionalVerticalRotation`
    on `app.PlayerCamera` (if the game consumes and clears them each frame, it is a ready-made additive input);
    (b) rotate `CurrentController.<rotation>` (yaw about world up, pitch about local right, clamped to
    `XRotateLower/UpperLimit`) only when `currentType` is MaximumOperatable; (c) add to
    `<deltaHorizontalRotation>`/`<deltaVerticalRotation>` (degrees or radians unknown; would need a hook between
    `calcDelta*` and its use, since they are recomputed each frame).

- 2026-10-15 **RE7 first install (Gyro4All 0.9.0, unchanged build, `winmm.dll` in the game's root folder next to
  `re7.exe`; saves backed up with `um backup create` as `re7-saves`)**. Launched through Steam; pad and Steam Input
  state not checked (the game's HID enumeration shows no Sony 054C device).
  - Works: the proxy loads despite SteamStub (`.bind`); `engine: none detected, using Unreal`; import-table hooks
    (no protector detected); `PeekMessageW` hook in place (the live IAT slot of `re7.exe` points into our
    `WINMM.dll`); `XINPUT1_3.dll` XInputGetState/SetState/GetCapabilities/Enable hooked; the game also creates
    DirectInput8 and opens HID devices itself, registers Raw Input mouse; overlay captured the **D3D12** swapchain;
    `ResizeBuffers` comes from two threads (two swapchains: 1920x1080 format 24 and 3840x2160 format 28).
  - **Fails: the gyro frame never runs** (no `gyro:` line, so GyroLib is never even started). Cause: RE Engine's main
    thread (the window's owner, the DllMain thread) sits **blocked in `GetMessageW`** (40/40 stack samples:
    `win32u` <- `USER32!GetMessageW` <- `gameoverlayrenderer64` <- `re7.exe`); the game loop runs on another thread,
    so no empty-queue `PeekMessageW` ever happens on that thread. The `PeekMessageW` frame source is Unreal-specific.
  - Needed for RE Engine: an engine-provided per-frame hook on the game update thread. REFramework hooks the
    `via.Application` entry functions (`sdk/Application.cpp`: function table found by pattern
    `89 81 ? ? ? ? 48 8B ? 48 81 C1 ? ? ? ?`, stride 0xD0 below TDB 74, description string at +0x18, priority +0x20,
    type +0x22; `Hooks.cpp` dispatches pre/post per entry, e.g. `UpdateBehavior`, `LateUpdateBehavior`,
    `BeginRendering`). For the gyro: run after `UpdateBehavior` and before `LateUpdateBehavior` (`app.PlayerCamera`
    integrates input in `update`/`lateUpdate`). Core change: let `Engine` supply the frame source instead of the
    `PeekMessageW` hook (default stays `PeekMessageW` for Unreal).

- 2026-10-15 **RE Engine module, step 1: the frame hook (works in RE7, not released, version unchanged 0.9.0)**.
  - Core: `Engine::frame_source(FrameFn)` (DllMain): true = the engine calls the gyro frame itself on its game
    thread and the core skips its `PeekMessageW` hook (`dllmain.cpp` `run_frame`, same 0.5 ms limiter and perf).
    Default false, so Unreal is unchanged. Log line `hook: the RE module runs the gyro frame on its game thread`.
  - `src/engines/reEngine/` (`gyro_re`, registered after Unreal; `Engine=RE`): detect = `re_chunk_000.pak` next to the
    exe. `re_tdb.cpp`: TDB header found by `TDB\0` + version in the exe's writable sections (RE7: `re7.exe+0x7208840`),
    ready when its types table is an absolute pointer. **The header's `initialized` field (+8) stays 0 in RE7 even
    after init**, so it is useless (first attempt waited 3 minutes on it). `via.Application` = type 49307 by
    namespace/name; its instance through the runtime type's vtable[1] getter (`REType` at typedef+0x40), called from
    a startup thread under `__try`; instance is on the heap. The entry table found by shape at instance+0x100..0x1000
    (stride 0xD0, name at +0x18, priority u16 at +0x20, function at +8, must contain BeginRendering and EndRendering).
  - RE7: **301 entries**, logged in order with their via.ModuleEntry priority; the gameplay ones: `UpdateHID`(154),
    `PreupdateBehavior`(181), `UpdateBehavior`(187), `UpdateMotion`(200), `LateUpdateBehavior`(214),
    `PrepareRendering`(222), `LockScene`(241), `WaitRendering`(242), `BeginRendering`(245), `EndRendering`(268).
    Hook = swap the entry's function pointer (data write, like REFramework's `Hooks.cpp`); `FrameEntry=` in
    Gyro4All.ini picks the entry the gyro runs before (default `LateUpdateBehavior`, original `re7.exe+0x5AEAF60`).
  - In game (RE7, DualSense 054C:0CE6; connection and Steam Input state not checked): TDB ready 0.5 s after start,
    hook in place before the first frame; **first gyro frame on the game update thread** (not the main thread), GyroLib
    1.3.1 loads, SDL reader **470 samples/s**, auto-calibrated (`motion=1 still=1`), DX12 panel ready (F10). Gyro off
    (`no gameplay view`): read_frame has no scene yet. XInput translation off by itself: the game creates DirectInput
    and reads the DualSense natively over HID (opens/closes it at start).
  - Next: scene (ObjectManager singleton `_Instance` -> `PlayerObj` -> `app.PlayerCamera` component; field base from
    the int32 before the managed vtable), pause/menu gate, then the apply candidates above.

- 2026-10-15 **RE Engine module, step 2: scene, camera and controller in RE7 (not released, version 0.9.0)**.
  - Singletons: REFramework's REGlobals pattern (`48 8D ?5 disp32 48 B8 00..00 80`, i.e. `lea r64,[rip+slot]; mov
    rax,1<<63`) over the exe's code gives **145 static slots** in RE7; a slot's object type = `*(*(obj))` (REObjectInfo ->
    its RETypeDefinition) compared with the TDB's types table. `app.ObjectManager` at `re7.exe+0x8FE9418` (also
    `app.GameManager`, `app.MenuManager`, `app.CameraManager`, `app.PadManager`, `app.vr.*`...). Filled ~2 s after start.
  - **Field base = int32 just before the object's REObjectInfo: 0x50 for behaviours (not 0x48 as guessed from
    ReClass), 0x10 for plain objects** (BaseTransformSolver, controllers); native components vary (Mesh 0x410,
    Motion 0x480...). The offline offsets of `tools/tdbdump.py` are relative to it.
  - Player: `ObjectManager` +0x38 `PlayerObj` (via.GameObject) -> `transform` at +0x18 -> components chained through
    `REComponent` +0x18 (next), owner at +0x10; ~85 components on Ethan, `app.PlayerCamera` among them.
  - Camera state (`app.PlayerCamera`): `<CameraRotation>`, the solver's `<rotation>` and `CurrentController.<rotation>`
    are the same quaternion. Free look: solver `currentType` 0, controller `app.PlayerCamera.MaximumOperatable`;
    scripted scenes: `ElasticRotation` (2), `FullAnimation` (6, IsControlledByAnimation=1),
    `InterpRotation2FullAnim` (7). Quaternion forward is +Z, up +Y; **positive yaw (atan2(fx,fz)) turns left, positive
    pitch looks up** (checked with screenshots).
  - **Live write tests (external WriteProcessMemory, game running): rotating `CurrentController.<rotation>` turns the
    camera and stays** (the game rebuilds CameraRotation and the solver from it every frame); yaw = world-up rotation
    pre-multiplied, pitch = local-X rotation post-multiplied. The game clamps pitch to `XRotateLowerLimit` -58 /
    `XRotateUpperLimit` +75 (degrees, pitch-up positive) itself, but a rotation past the pole flips the yaw by 180, so
    Gyro4All clamps first. `AdditionalHorizontal/VerticalRotation` stayed 0 (not used for input).
  - Module: view 1 = free look (`currentType` 0, MaximumOperatable or subclass, not `IsControlledByAnimation`);
    paused = `app.DoomsBehavior.IsPause` (+0x1) of the PlayerCamera; apply = rotate the controller's `<rotation>` by
    -yaw about world Y and -pitch about local X (GyroLib: yaw right / pitch up positive); recenter levels the pitch.
    Status line: `camera=<controller> type= anim= aim= pause= yaw= pitch=`.
  - **Controller**: RE7 creates DirectInput and imports HID.DLL but reads neither the DualSense nor the DualSense shown as a
    DualShock 4 (opens it, closes it unread). Auto mode hid the pad (DirectInput sign) and, after one session, wrote
    `DualSenseAsDualShock4=1` itself (the notice appeared), which the game then also rejected: no controller at all with
    Steam Input off. New shipped `profiles/re7.ini`: `XInput=2`, `DualSenseAsDualShock4=0`.
  - In game (DualSense USB/BT not checked, Steam Input off): `shown as XInput controller 0`, `gyro on (target
    MaximumOperatable)`, applied yaw 5.7 / pitch 12.5 in the first 10 s, 47 camera updates/s; gyro off by itself in
    `InterpRotation2FullAnim`/`FullAnimation`/`ElasticRotation`. **User-verified: controller and gyro work, direction
    right on both axes, gyro stops in pause and in cutscenes.**

- 2026-10-15 **What RE7 does with HID.DLL (static analysis of `re7.exe`)**: native support for the **DualShock 4 only, found
  through DirectInput**. Imports HidD_GetAttributes/GetPreparsedData/GetHidGuid/FreePreparsedData and HidP_GetCaps
  (called through `jmp [IAT]` thunks at `re7+0x5AE763E..0x5AE7656`) plus SetupAPI. One routine (`re7+0x29051D0`,
  args VID, PID, out caps) walks the HID interfaces, opens each (`CreateFileW`, read/write), keeps the one whose
  HidD_GetAttributes VID/PID equal the arguments and returns its handle and HIDP_CAPS; others are closed (the
  open/close pairs in the log). Its two callers (`re7+0x2905F32`, `re7+0x2907726`) take VID/PID from a DirectInput
  device (`IDirectInputDevice8` vtable +0x18 = GetCapabilities just before; the second splits a `DIPROP_VIDPID` dword).
  Then (`re7+0x2905F68`) the device is flagged as a DualShock 4 only if: DirectInput vendor 054C and the **HID caps have
  the DS4 descriptor's shape**: InputReportByteLength >= 0x40, Output >= 0x20, Feature >= 0x40, 1 link collection,
  1 input button cap, >= 9 input value caps, >= 23 input data indices, no output button caps, <= 1 output value cap and
  index, no feature button caps, >= 45 feature value caps and indices. The handle is kept (`+0x88`) for that device.
  - So `DualSenseAsDualShock4=1` can't pass here: Gyro4All answers the HID identity as 054C:09CC, but DirectInput still
    says 054C:0CE6, so the lookup's VID/PID compare fails and the handle is closed; the HIDP_CAPS would be the
    DualSense's too. User report: a DS4 emulated by another program doesn't work either (probably its virtual descriptor
    fails the caps shape). Making it work would need faked DirectInput VID/PID and HidP_GetCaps/preparsed data, for
    PlayStation prompts only: not worth it; RE7 stays on `XInput=2` (profiles/re7.ini).
  - Core flaw it shows: auto mode learns `DualSenseAsDualShock4=1` from "Sony library + DualSense closed unread", and
    never undoes it when the emulation is rejected too.

- 2026-10-15 **Core: a learned DualShock 4 identity is taken back when the game rejects it** (not released, 0.9.0).
  `learn_dualsense` now also writes `DualSenseAsDualShock4Learned=1`, and learns nothing when the profile already has a
  `DualSenseAsDualShock4` value. `nativepad::learned_ds4_rejected()`: learned identity on, and the game opened the
  DualSense (shown as a DualShock 4), closed it unread and has none open 5 s later (same test as `dualsense_ignored`).
  Then `xinput.cpp` shows the DualSense as XInput regardless of the auto mode's native signs (Any = DirectInput,
  Sony library), and `unlearn_dualsense` (host) writes `DualSenseAsDualShock4=0` and removes the `Learned` key, so it
  isn't learned again. Values set by the user or a shipped profile are never changed. Docs: ADVANCED.md log rows and
  the DualSense-as-DualShock-4 section.
  - Tested in RE7 with a test profile (`DualSenseAsDualShock4=1`, `Learned=1`, auto XInput): opened/closed as a DS4 at
    +2 s, `shown as XInput controller 0` and the profile rewritten to `=0` 5 s later. Shipped `profiles/re7.ini`
    (XInput=2) restored afterwards. User-verified: the controller responds in that session (XInput shown 5 s after start).

- 2026-10-15 **Build: Gyro4All 1.0.0** (version chosen by the user: first engine besides Unreal).
  `build/release/Gyro4All-v1.0.0.zip` (winmm.dll, gyrolib.dll, licenses, 8 profiles incl. `re7.ini`, readme + docs; no
  settings or logs). What's new since 0.9.0: the RE Engine module (TDB reader, via.Application frame hook via
  `Engine::frame_source`, RE7 free-look camera), `profiles/re7.ini` (`XInput=2`), the learned
  `DualSenseAsDualShock4` taken back when the game rejects it, `tools/tdbdump.py`. Docs: README (RE Engine install
  folder, RE7 row, DualShock 4 fallback), ADVANCED (engine modules, RE7 compatibility row, RE Engine requirements, `re:`
  log rows, how the RE Engine module works, `FrameEntry=`), CREDITS (REFramework layouts, Capcom in the disclaimer).
  Verified: clean build; the package script checked the DLL's version. Same code as the RE7 tests above.

- 2026-10-15 **RE Requiem (`re9.exe`, Steam app 3764200): TDB 83 support and its player camera** (not released,
  version 1.0.0 unchanged). Saves backed up with `um backup create` as `re9-saves`.
  - Install: exe in the game's root folder; the user had already installed Gyro4All 1.0.0 (`winmm.dll`) next to
    REFramework (`dinput8.dll` + `reframework\`), ReShade + RenoDX (`dxgi.dll`, `renodx-re9requiem.addon64`), Ultimate ASI
    Loader as `version.dll` (loads `RTX40MFG.asi`), DLSS-G -> FSR3 replacement, DSX. 1.0.0 stopped at `TDB 83 is not
    supported yet` (header `re9.exe+0xB76FB70`).
  - **Protected exe**: sections `.xtext`, `.xtls` (333 MB, the real code), `.trace`, `.sbss`; the import table is partly
    a decoy (`advapi32.dll!UrlEscapeW -> USER32...` in the log). Gyro4All's import-table hooks still work for kernel32
    (CreateFileW/ReadFile: nativepad sees the game read the DualSense) but not for graphics: the game never calls the
    `dxgi.dll!CreateDXGIFactory2` import, so the overlay never sees a swapchain.
  - **TDB 83 layouts** (REFramework tdb83 / RETypeDefVersion83, ported to `re_tdb.cpp` and `tools/tdbdump.py`): header
    counts num_types +0x08, num_string_pool +0x58; tables types +0x68, typesImpl +0x70, methods +0x78, methodsImpl +0x80,
    fields +0x88, fieldsImpl +0x90, stringPool +0xD8. Type definition 0x50: ids 19 bits (parent bits 19-37, declaring
    38-56), impl_index bits 38-55 of the second qword, member_method/member_field packed at 0x20 (bits 22-43 / 44-63),
    runtime type at **0x38** (69/70: 0x40), managed_vt 0x40. RETypeImpl: names packed 28+28 bits in the first qword,
    num_member_fields bits 33-56 of the qword at 0x10. REField: declaring:19, impl_id:19, field_typeid:19; REFieldImpl:
    u16 unk:1 flags:15, u32 offset:26, u32 name:28. Methods 0xC bytes, no code pointer. Object -> REObjectInfo ->
    type definition and the field base (int32 before REObjectInfo) are unchanged. `via.Application` entries from TDB 74:
    stride 0xC8, name +0x10, priority +0x18. Native layouts unchanged (GameObject transform +0x18, component next
    +0x18, owner +0x10, arrays count +0x1C / items +0x20). 178180 types.
  - In game: TDB ready 1.3 s after start, `via.Application` found, **338 entries** (two `UpdateBehavior`; the hook takes
    the first entry of the name), frame hooked before `LateUpdateBehavior` (`re9.exe+0x2A2220`) on the game thread.
    **REFramework coexists**: it swaps the same entries' pointers 4 s later and calls ours. The game reads the DualSense
    itself (USB report 0x01): no XInput translation, PlayStation icons kept.
  - **Native gyro**: `app.CameraInputSubSystem` (`_IsMotionSensor`, `_MotionSensorYaw/PitchDiff`,
    `MotionSensorPlatformType`), `app.CameraInputUserData._MotionSensorSensitivityYaw/Pitch`. User: on PC it works only
    while aiming (L2). Gyro4All's gyro is off then (ADS controller): no doubled motion.
  - **Camera**: `app.CameraSystem` (an `app.AppSingleton`1`, in the exe's static singleton slots like RE7's:
    `re9.exe+0xE9A16A8`; 148 slots) -> `_CameraBlenders` (array) -> the `_Role` 0 `app.CameraBlender` ->
    `<BusyCameraController>` (`app.PlayerTPSCameraController` / `app.PlayerFPSCameraController`, others: ADS, Action,
    Event, Fixed, Gimmick, Grapple, Bike, DetailSearch, PhotoMode, ConstTarget). The player controllers' GameObject has an
    `app.PlayerCamera{TPS,FPS}ActionInterpolation` component -> `_RotationInterpolation`
    (`app.PlayerCameraRotationInterpolation`, `_PitchRangeDeg` = via.Range start -60 size 120) -> `_NextInfo`
    (`app.PlayerCameraRotationCalc`) -> **`<CameraRotation>k__BackingField`**: a live write there (yaw +20, pitch +15)
    turns the camera and stays (the interpolation and blender follow). Pause: `app.PauseManager` slot
    `re9.exe+0xE998348`, `_CurrentPauseTypeBits` (0x1 loading, 0x80 / 0x2 seen briefly in game; 0x2 matched the moments
    GyroLib's panel had the controller). **Pitch sign is the other way round from RE7** (user: inverted with RE7's
    sign); yaw is the same.
  - Module: camera models per game family (`re_camera.hpp`: `re7_camera.cpp` = RE7's code moved unchanged,
    `requiem_camera.cpp`), the first whose classes are in the TDB is used (`re: camera model: ...`); per-model pitch
    sign. User-verified in third person: the gyro turns the camera, yaw and (after the per-model sign) pitch the right way
    round; the game starts without errors with `profiles/re9.ini`. First person user-verified too. Recenter not tested.
  - **Settings panel: not available in RE Requiem.** `OverlaySystemFactory=1` (new core setting: hook the real System32
    DXGI factory for the panel only, like protected exes, keeping import-table mode for the rest) gets the swapchain
    (`DX12 panel ready`), but: with DLSS frame generation on, the game crashed 2 s after start in Streamline
    (`sl.interposer` -> `sl.dlss_g` -> `sl.common`, access violation; REFramework's crash log); with frame generation off,
    it hung (no gyro frames after the first resizes, REFramework re-hooking D3D12 every 11 s, black screen) — REFramework
    and Streamline hook the same real swapchain. The UE games' panel worked with frame generation because there the hook
    sits above Streamline (the game's own factory import). Next idea: hook Streamline's own factory
    (`_storage_\sl.interposer.dll`, which the game calls directly) when it loads. `profiles/re9.ini` ships without it.
  - RE7 moved to another folder and its exe is missing at the moment: the TDB 69/70 path of `re_tdb.cpp` and
    `tdbdump.py` (same offsets, moved into per-version tables) was not re-run against RE7.
- 2026-10-16 **RE Requiem: the settings panel works, through Streamline's factory** (not released, version 1.0.0
  unchanged). Chosen automatically (`overlay.cpp`): in import-table mode Gyro4All always hooks Streamline's factory
  when `sl.interposer.dll` loads, and the first Streamline factory decides: if the game's own dxgi factory import was
  called before, the panel stays on the import (the UE games; user-verified in a UE game with frame generation on
  and off, toggled in game: all OK); else it uses Streamline's (`overlay: the game makes its DXGI factory through Streamline ...`). Protected
  exes (inline mode) keep the system factory. `profiles/re9.ini` needs no setting. The unreleased
  `OverlaySystemFactory=1` became the undocumented diagnostic override `OverlayFactory=`: `streamline` (only
  Streamline's), `game` (never Streamline's), `system` (the old `OverlaySystemFactory=1`). The first version of this
  entry was an opt-in `OverlayFactory=streamline` in the profile. Saves backed up as `re9-saves`. Same install as 2026-10-15 (REFramework, ReShade + RenoDX as `dxgi.dll`, Ultimate ASI Loader as
  `version.dll` with RTX40MFG, the DLSS-G -> FSR3 replacement).
  - How the game reaches Streamline: `re9.exe` loads `sl.interposer.dll` (copied to `_storage_\` like the other DLLs;
    REFramework logs the redirect) about 2 s after start, and its strings name `slUpgradeInterface`, `slSetD3DDevice`,
    `slGetNativeInterface`, `CreateDXGIFactory*`. In game it calls the interposer's `CreateDXGIFactory2` and then
    `CreateDXGIFactory1` (both proxies, vtables in `sl.interposer.dll`); `slUpgradeInterface` is hooked too but wasn't
    seen on a factory.
  - Streamline path: `LdrRegisterDllNotification` (or the already loaded module) -> MinHook on the interposer's
    `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` and `slUpgradeInterface` exports -> the existing
    `hook_factory` on the proxy factory -> `CreateSwapChainForHwnd` returns Streamline's proxy swapchain, whose
    `Present`/`ResizeBuffers` vtable gets the usual hooks.
  - **Dead end 1, device mismatch**: the first run captured the proxy swapchain (no crash) but GyroLib refused it:
    `DX12 init failed (-1): Queue/swapchain device mismatch`. The queue the game passes is a Streamline proxy
    (`sl.interposer.dll`) on a Streamline proxy device, while the proxy swapchain's `GetDevice` returns ReShade's device
    (`dxgi.dll`). Fix (any mode): when the two devices differ, try the native objects under Streamline's proxies
    (`slGetNativeInterface`) and render with the first matching pair: here the proxy swapchain + the native queue
    (ReShade's, same device). Present is still hooked on the proxy, so the panel is drawn above frame generation.
  - **Dead end 2, crash on toggling frame generation**: the game creates a new swapchain for the same window; Gyro4All
    (its captured swapchain/queue) and GyroLib's renderer still held the old one, and `sl.dlss_g` -> `sl.common`
    crashed inside Streamline's `CreateSwapChainForHwnd` (access violation, REFramework's crash log, our
    `hk_create_swapchain_hwnd` as the outer caller).
  - **Dead end 3, release just before the new swapchain**: shutting GyroLib's renderer down and releasing our references
    at the start of the game's `CreateSwapChainForHwnd` (on the render worker) fixed frame generation OFF -> ON (the
    old proxy and the swapchain under it went to 0 references), but ON -> OFF froze: every creation failed with
    `0x80070005` (E_ACCESSDENIED: the window still has a swapchain), the game retried every ~12 ms and then waited on an
    error dialog hidden behind its window (`DialogBoxIndirectParamW` from the swapchain thread, seen with `cdbX64 -pv`;
    the WinDbg app's cdb works for non-invasive stack dumps). Baseline with the panel off (the then opt-in
    setting left empty: no swapchain hooked): ON -> OFF works. Reading: without the panel the game's last Release destroys Streamline's proxy while frame
    generation is still loaded, and DLSS-G cleans up; with the panel the proxy outlived the unloading of frame
    generation, and something under it (DLSS-G or the DLSS-G -> FSR3 replacement) kept a swapchain on the window.
  - **Fix** (Streamline path only, to keep the default path unchanged): hook `Release` (slot 2) of the captured proxy
    swapchain; when its count falls to our references plus GyroLib's (ours: 2; GyroLib's measured around
    `gl_overlay_dx12_init`: 1), the game has let go: shut the renderer down and release ours right there (log
    `the game released its swapchain: released swapchain ... (0 references left; native swapchain ...: 0)`); the next
    swapchain is captured as usual. The release before a new swapchain stays as a fallback. `slGetNativeInterface`
    AddRefs what it returns (measured at run time: references 2 -> 3). The default path holds old swapchains the same
    way (and never releases them): worth the same fix if a game recreates its swapchain.
  - **User-verified**, DualSense 054C:0CE6 (report 0x01: USB or basic Bluetooth; Steam Input state not recorded):
    frame generation off: panel opens with F10 and works, gyro turns the camera, no hang (`system` hung there);
    frame generation on: panel opens and works, gyro turns the camera (third person), about 3 minutes of play;
    with the final build, frame generation ON -> OFF and OFF -> ON in the settings with the panel re-created each time,
    no freeze or crash, quit normally; with the automatic selection (no setting): Streamline picked by itself, F10
    panel works, gyro turns the camera, FG toggled twice cleanly, quit normally.
    One earlier start closed after 3 s during REFramework's startup: the user had closed it themselves.
- 2026-10-16 **Pragmata (`PRAGMATA.exe`, RE Engine, TDB 83): crash ~32 s after the frame hook, Capcom anti-tamper**
  (not resolved; version 1.0.0 unchanged). Gyro4All alone, no REFramework. Log: TDB 83 ready (119105 types), 359
  application entries, frame hooked before `LateUpdateBehavior` (`PRAGMATA.exe+0x24400`), `re: no known player camera
  in this game` (neither RE7's nor Requiem's camera classes), panel on Streamline's factory, DualSense 054C:0CE6 read by
  the game itself (report 0x01). Then the game's own CrashReport: `EXCEPTION_ILLEGAL_INSTRUCTION` at
  `PRAGMATA.exe+0xCB07FF0`.
  - Dump (cdb): the faulting thread is the game thread that runs our gyro frame (same thread id). The fault address is
    the last 16 bytes of the RWX `.impdata` section (raw size 0x200, filled at run time) and holds `0F 0B` (**UD2**):
    a planted gadget, reached from a job dispatch (`PRAGMATA.exe+0x4BFAB51` -> `+0x51E4847`). No Gyro4All or GyroLib
    frame on the stack. Exe sections: `.xcode` (code), `.sdata` (holds the IAT, RVA 0x9791610), `.idata` (173 MB,
    RWX: the obfuscated code), `.impdata`, `.xtext`, `.ecode`.
  - This is the anti-tamper penalty REFramework's `IntegrityCheckBypass.cpp` describes for RE9 (and has PRAGMATA
    patterns for): on a failed integrity check, random thread-scheduler job pointers are replaced with UD2 gadgets; it
    also has a PE-header check, a renderer "heartbeat" (`via.render.Renderer`, TDB >= 82) and a list of application
    entries it never hooks (`ignore_application_entries`). **RE Requiem was only ever tested with REFramework
    installed**, whose bypass hid all of this: Gyro4All has never run alone in a Capcom-protected RE Engine game.
  - Not yet known which of our writes trips the check: the IAT patches in the exe (XInput x4 and the other import
    hooks, `.sdata`) or the `via.Application` entry pointer swap (heap). Next: bisect (proxy only / + import hooks /
    + entry hook), and confirm with REFramework installed (expected: no crash). Pragmata also needs its own camera model.
  - **With REFramework (`dinput8.dll` dated 2026-04-18) added: crash earlier, ~7 s after start**, while the game loads
    its paks. Our side got as far as the entry hook and the game reading the DualSense, but no `first gyro frame`.
    No dump left (the game's `CrashReport\` folder for that run was gone). REFramework identifies `pragmata`, TDB 83, but
    its bypass is only partial for this build: `Could not find thread scheduler corruptor` (falls back to hooking 7 job
    submission call sites), `Could not find PE header integrity check`, `Could not find stack destroyer`,
    `Failed to find get_encoded_pointer`, `Renderer::hasInstance not found`. The crash can't be attributed yet: next,
    REFramework alone (our `winmm.dll` set aside) and/or a current REFramework nightly.
  - **With a newer REFramework (`dinput8.dll` dated 2026-10-11): the game starts and runs**, the frame hook runs on
    the game thread, and the F10 panel works (Streamline factory, native queue, swapchain released and recaptured
    once). User report. Still no camera model.
  - **Bug: the panel didn't idle the game's pad.** The game calls `CloseHandle(NULL)` (log `the game closed controller
    0000:0000 (handle 0000000000000000)`): `find_device(nullptr)` matched a free slot, so `untrack` dropped
    `g_device_count` to 0 while the DualSense handle was still open. From then on every report passed through
    unedited and `set_neutral` returned early (no `made idle` line). Fix in `nativepad.cpp`: `find_device` never
    matches nullptr/`INVALID_HANDLE_VALUE`. Built and unit-tested; not yet verified in game.
  - **Camera (TDB, offline with `tdbdump.py`)**: same family as Requiem but different names. `app.CameraSystem`
    (`app.AppSingleton`1`) `_CameraBlenders` +0x28; `app.CameraBlender` `_Role` +0xC0, `_BusyCameraKind` +0xB8,
    `_BusyCameraController` +0x70 (a plain field, Requiem's is `<BusyCameraController>k__BackingField`). One player
    controller, `app.PlayerCameraController` (no TPS/FPS split, no `PlayerCameraRotationCalc`): `_Blender` +0x188,
    `_CameraPadInputUpdater` +0x190, `_CameraAngleLimit` +0xE8 (`_DefaultPitchLimit` via.Range +0x08),
    `_CameraInputGyro` +0xF0; from `app.CameraControllerRoot`: `<CameraObjectRotation>k__BackingField` (via.Quaternion)
    +0x10, `<CameraJointRotation>k__BackingField` +0x30, `_IsBusy` +0x66. Field offsets are from the managed field
    start. Pause: `app.PauseManager._CurrentPauseTypes` +0x58 (Requiem: `_CurrentPauseTypeBits`).
    Candidate to write: `<CameraObjectRotation>` (not tested live).
  - **Native gyro in the game**: `app.PlayerCameraController.CameraInputGyro` (`_EnableOption`, `_SensitivityHOption`,
    `_SensitivityVOption`, `_BaseOrientation`, `_RemainingDelta`). Check the game's options before adding a camera
    model: if it is enabled, Gyro4All's gyro would double the motion.
  - User: the game's own gyro works only while aiming (L2) and is a player option. Decision: the player turns the
    game's gyro option off to use Gyro4All's; don't run both (doubled motion while aiming). Gyro4All doesn't
    change the game's option itself.
- 2026-10-17 **Per-engine tool folders** (no code change, version unchanged). Engine-specific debug and camera-finding
  tools now live in `tools/<engine>/`, named like `src/engines/`, each with a README of step-by-step instructions:
  `tools/reEngine/` (`tdbdump.py`, moved from `tools/`; new `camera_finder.lua`) and `tools/unrealEngine/` (README
  only: the camera probe is built into Gyro4All). `camera_finder.lua` is a REFramework autorun script: from
  `app.CameraSystem` to the `_Role` 0 blender's controller in use (both field spellings), it lists every
  `via.Quaternion` on the controller and one object level below, with buttons that turn one by 20 degrees yaw / 15
  degrees pitch (world-up yaw, local-X pitch), and logs each press to REFramework's log. Not yet run in a game.
- 2026-10-17 **Pragmata: camera found with `camera_finder.lua`, camera model `pragmata_camera.cpp`** (not released,
  version unchanged). User test: the main blender's controller is `app.PlayerCameraController`; a write to its
  `<CameraObjectRotation>k__BackingField` turns the camera and stays (log: pitch -15 in the script's local-X convention,
  `x=-0.009 y=0.978 z=0.044 w=0.202 -> x=-0.036 y=0.964 z=0.172 w=0.199`). In `read_rotation`'s convention that is
  pitch +5.1 -> +20.2, which suggested RE7's pitch sign (+1); **wrong: user-verified in game, pitch was inverted with
  +1, so the sign is -1 like RE Requiem's** (the quaternion's "up" in `read_rotation`'s convention is down on screen
  in both; don't infer the sign from a quaternion diff, check on screen). The model: `app.CameraSystem` ->
  `_CameraBlenders` -> `_Role` 0 blender -> `_BusyCameraController`; free look when it is an
  `app.PlayerCameraController`; rotation `<CameraObjectRotation>` (declared on `app.CameraControllerRoot`); pitch range
  from `_CameraAngleLimit._DefaultPitchLimit` (via.Range, start/size; default -60..60); pause =
  `app.PauseManager._CurrentPauseTypes` != 0. `resolve` needs the plain `_BusyCameraController` field, so it never takes
  RE Requiem (tried after the Requiem model anyway). In game (user): the gyro turns the camera, yaw right; pitch fixed
  to -1 after the first test (not yet re-tested). Open: does aiming
  (L2) switch to another controller (then the gyro stops while aiming, leaving the game's own aim gyro), and the
  pitch direction on screen.
  - **User-verified with the -1 pitch build** (DualSense 054C:0CE6, report 0x01: USB or basic Bluetooth; Steam Input
    state not recorded; REFramework installed): the gyro turns the camera, yaw and pitch the right way round. Log:
    `re: camera model: Pragmata (app.PlayerCameraController)`, slots `app.CameraSystem` `PRAGMATA.exe+0xB9BCC90` and
    `app.PauseManager` `PRAGMATA.exe+0xB9AD318` (found ~6 s after start), controllers seen `TitleCameraController`
    (title), `ActionCameraController` (intro action), then `PlayerCameraController` (`gyro on`). Pause types seen:
    0x2 (brief moments in game, like Requiem's) and 0x40 (pause menu and while the F10 panel is open): gyro off in
    both. Pitch range from the game: -70..80. The F10 panel now idles the game's pad (`made idle` / `passed through
    again`, ~21,000 reports idled). **Aiming (L2) keeps `PlayerCameraController`**: Gyro4All's gyro stays on while
    aiming, so the game's own gyro option must be off (GAMES.md says so). Pragmata moved to Supported games.
- 2026-10-17 **`tools/reEngine/camera_finder.lua` is now an automatic camera finder** for any RE Engine game REFramework
  supports (the earlier version only listed the CameraSystem family's controller rotations, with buttons). Like
  the Unreal probe: (1) scan, a few thousand objects per frame (default cap 60000), from REFramework's managed
  singletons (camera-named ones to depth 9, others to 6) and the primary camera's GameObject, through managed fields,
  arrays (32 items), `List`1`, GameObject components (native chain: transform +0x18, next +0x18) and, for camera/player
  components, their GameObject (+0x10); keeps `via.Quaternion` fields and `System.Single` fields named
  yaw/pitch/angle/rot; (2) 3 captures with the right stick (camera yaw change > 25 degrees, then 30 still frames):
  keeps rotations whose yaw changed like the primary camera's transform (within max(2, 8%)), floats matching yaw or
  pitch in degrees or radians, either sign; (3) live test, writes queued to `re.on_pre_application_entry
  ("LateUpdateBehavior")` like Gyro4All's: +5 degrees yaw (Gyro4All's turn_rotation math), the camera must move 5 +-
  1.5; then +5 pitch with sign 1, and the camera's pitch direction gives `PitchSign`. Camera angles come from the
  primary camera's transform assuming it looks down -Z (consistent with RE Requiem and Pragmata, whose camera-object
  rotations needed PitchSign -1 while RE7's +Z controller rotation needs +1). Output: menu, REFramework log,
  `reframework\data\gyro4all_camera.txt`. Lua syntax checked (luaparser); **not yet run in a game**: first run should
  be Pragmata, where the answer is known (`<CameraObjectRotation>k__BackingField`, PitchSign -1).

- 2026-10-17 **RE8 (Village): no controller in the first session; core fix in auto XInput** (not released, version
  unchanged). RE8 (TDB 69, no profile, `XInput=1`) behaves like RE7: it creates DirectInput, imports HID.DLL, opens the
  DualSense (054C:0CE6) twice and closes it unread. Log: `xinput: ... not shown: it creates DirectInput`, then at +8 s
  `dualsense: ... closed the DualSense unread ...: profile created` (`DualSenseAsDualShock4=1`, `Learned=1`) and the
  notice "it works as an Xbox controller for now", but the pad stayed hidden: in `xinput::frame` the `Any` sign
  (DirectInput) was checked before the Sony branch, and only a *rejected learned* DS4 identity skipped it. User report:
  the DualSense didn't work, and XInput didn't either in the same session. Fix: a Sony-vendor pad with
  `nativepad::dualsense_ignored()` also skips the `Any` sign, so it is shown as XInput in that session. Built, unit
  tests pass; **not yet tested in game**. Expected next start (learned profile): the DS4 identity is rejected like in RE7
  (same DirectInput VID/PID lookup, presumably), `unlearn_dualsense` writes `=0` and XInput is shown. If so, ship a
  `profiles/re8.ini` like `re7.ini` (`XInput=2`, `DualSenseAsDualShock4=0`). The RE7 camera model resolves in RE8 only in
  part (`scene types incomplete`: no `ObjectManager`/`PlayerObj`, no `CameraController.rotation`): RE8 needs its own
  camera model (`camera_finder.lua` copied to its `reframework\autorun`, not yet run).

- 2026-10-17 **RE8: first `camera_finder.lua` run found nothing; RE8's camera classes (tdbdump)** (tool only).
  - Run (REFramework v1.5.9.1 +514, built 2026-10-11): `scan started: 0 singletons, primary camera found`, then
    `scan done: 82 objects visited, 0 rotations`, three times. `sdk.get_managed_singletons` gave an empty list (or
    failed; the script swallowed the error), so only the primary camera's GameObject was walked, and the camera
    rotations are not there: as in RE7, `app.PlayerCamera` is a component of the **player's** GameObject.
  - Script fix: `managed_singletons()` logs what `sdk.get_managed_singletons` returned; when it's empty it asks
    `sdk.get_managed_singleton` by name for a known list (`app.PropsManager`, `app.ObjectManager`, `app.CameraSystem`,
    ...) plus every top-level TDB type named `*Manager`/`*System`/`*Master`/`*Director`/`*Camera*` (`sdk.get_tdb`).
    A scan with no rotation logs the types it visited (`types visited: ...`). Syntax checked; not yet run.
  - RE8 (TDB 69) is RE7's camera with renamed fields. Player: `app.PropsManager.<Player>k__BackingField`
    (via.GameObject; RE8 has no `app.ObjectManager`/`PlayerObj`) -> component `app.PlayerCamera`:
    `<cameraRotation>k__BackingField` +0x30 (RE7: `<CameraRotation>`), `<IsControlledByAnimation>k__BackingField` +0x94,
    `<isGunAim>k__BackingField` +0x1c8, `BaseTransSolver` +0x548, `CurrentXRotateLower/UpperLimit` +0x278/+0x27c, also
    `<OperatedCameraRot>`, `<DesiredCameraRot>`, `PrevcameraRotation` quaternions. `app.PlayerCamera.BaseTransformSolver`:
    `<rotation>k__BackingField` +0x20, `<currentType>k__BackingField` +0x30, `<CurrentController>k__BackingField` +0x68
    (RE7: `CurrentController`). `app.PlayerCamera.CameraController`: `<Rotation>k__BackingField` +0x10 (RE7:
    `<rotation>`), `XRotateLowerLimit`/`XRotateUpperLimit` +0x78/+0x7c, and a `<gyroAdjustment>k__BackingField`
    (`app.PlayerCamera.CameraController.GyroAdjustment`) plus `BaseDeviceOrientation`: RE8 has gyro code of its own
    (probably for cloud/console builds; check the options menu). No `IsPause` field found yet. Also a third-person mode
    (`app.PlayerCameraTPS`, Shadows of Rose / TPS option). Likely path for a model: RE7's model with these names; the
    finder should confirm which rotation sticks (`CameraController.<Rotation>` expected) and its PitchSign.
  - Second run: `sdk.get_managed_singletons: error: attempt to call a nil value` and `0 types in the TDB`: this
    REFramework build's Lua has neither `sdk.get_managed_singletons` nor `sdk.get_tdb`. The name list found 2
    singletons; the scan visited 72 objects, all on the camera's GameObject (render controllers, `via.Camera`,
    `app.OnPauseCameraCaller`, `app.CameraDOFController`), 2 GameObjects, no `app.PlayerCamera`: 0 rotations. Script
    changes: each singleton found by name is logged; every GameObject reached is also asked for known camera
    components by type (`getComponent(System.Type)` with `sdk.typeof`: `app.PlayerCamera`, `app.PlayerCameraTPS`, ...),
    and the first 30 are logged with their name, components chained and why the chain stopped. Not yet run.
  - Third run (user switched to REFramework v1.5.9 +7, built 2025-03-05, the RE8 release build; same Lua API gaps):
    singletons `app.PropsManager` and `app.GameObjectManager`; GameObjects `MainCamera` (42 components chained) and
    **`pl1000`** at `app.PropsManager.<Player>k__BackingField` (65 components chained), but still `72 objects visited`:
    a **script bug**, the scan popped its queue by setting `queue[head] = nil` and tested `head <= #queue`; Lua's `#`
    on a table with nil holes at the start is undefined, so the scan ended early. Fixed with an explicit `tail`. The
    by-type lookup (`getComponent(System.Type)`) returned nothing on `pl1000` (cause unknown; the chain covers it).
  - Fourth run, **first complete automatic run of the finder**: 2351 objects, 526 rotations; captures (yaw -44.8,
    +104.7, -84.6) kept 10; live test: **CONFIRMED `primary camera <GameObject>{app.OnPauseCameraCaller}.playerCamera
    .BaseTransSolver.<CurrentController>k__BackingField.<Rotation>k__BackingField` (field of
    `app.PlayerCamera.MaximumOperatable`), PitchSign=1**: RE7's rotation with RE8's names, same sign as RE7.
- 2026-10-17 **RE8 camera model: `re7_camera.cpp` now covers Resident Evil Village** (not released, version unchanged).
  Each field is looked up by RE7's name, then RE8's (`either`): `<cameraRotation>`, `<isGunAim>`,
  `<CurrentController>k__BackingField`, `<Rotation>k__BackingField`. Player: `app.ObjectManager.PlayerObj`, else
  `app.PropsManager.<Player>k__BackingField` (RE8; `game()` then says Resident Evil Village). Free look: the controller
  is `MaximumOperatable` and not `IsControlledByAnimation`; RE7's `currentType == 0` test only when
  `app.PlayerCamera.CameraTypeEnum` exists (RE8's `currentType` is an `app.PlayerCamera.CameraType` object, an
  `app.Typedef` with static instances). Pause in RE8 (no `IsPause`): `app.HIDVibrationManager.IsOldPause` (+0x31) or
  `IsOldPartialPause` (+0x32), last frame's pause states kept by the rumble code; optional (logged when its slot isn't
  found). **Unverified**: that `app.PropsManager` and `app.HIDVibrationManager` are in the exe's singleton slots, and
  that those flags are set in the pause menu and inventory (alternatives: `app.PauseManager.<lastPauseBits>`, held only
  by the static `app.PauseProxy.<pauseManager>`; `app.WwiseManagerApp.IsInventoryPause`). RE8 also has
  `CameraController.<gyroAdjustment>` (its own gyro code) and a third-person mode (`app.PlayerCameraTPS`, not covered).
  Built, unit tests pass; not yet tested in game.
  - In game (first try): model and both slots found (`app.PropsManager` at re8.exe+0xC4C33B8,
    `app.HIDVibrationManager` at re8.exe+0xC4F3E30, 163 singleton slots), but `re: camera player without PlayerCamera`:
    in RE8 `app.PlayerCamera` is **not** a component of the player (`pl1000`), which is why the finder's lookup by type
    failed there and its confirmed path went through the main camera. Fix: RE8 reaches it from
    `app.PropsManager.<Camera>k__BackingField` (via.Camera) -> its GameObject (+0x10) -> component
    `app.OnPauseCameraCaller` -> `playerCamera` (checked to be an `app.PlayerCamera`; looked up again when the camera
    GameObject changes or the cached object is no longer one). Same session: the auto-XInput fix verified, `DualSense
    Wireless Controller shown as XInput controller 0` 5 s after the game closed the DualSense, and the game polls
    `XInputGetState` (controller in game not confirmed yet).
  - **User-verified in game (RE8, REFramework v1.5.9 installed, pad connection and Steam Input state not reported):
    gyro, pause and inventory work.** Log: `re: RE8 camera path: PropsManager.<Camera> +0x10 -> ... OnPauseCameraCaller
    13545 (playerCamera +0x8)`, `gyro on (target MaximumOperatable)`, applied yaw/pitch on both axes, `pause=1` / `gyro
    off: game paused` in the pause menu and the inventory (`app.HIDVibrationManager.IsOldPause`/`IsOldPartialPause`
    work), back on after. The controller worked as XInput (shown 5 s after start). The learned profile was rewritten to
    `DualSenseAsDualShock4=0`: RE8 rejected the DualShock 4 identity like RE7. Shipped `profiles/re8.ini`:
    `DualSenseAsDualShock4=0` only (the tested auto-XInput setup; unlike RE7's, not `XInput=2`). `docs/GAMES.md`: RE8
    moved to Supported. Not covered: the third-person mode (`app.PlayerCameraTPS`); RE8 without REFramework not tried.

- 2026-10-17 **Build: Gyro4All 1.1.0** (version chosen by the user). `build/release/Gyro4All-v1.1.0.zip` (winmm.dll,
  gyrolib.dll, licenses, 10 profiles incl. new `re8.ini`, readme + docs; no settings or logs). What's new since 1.0.0:
  Pragmata (`pragmata_camera.cpp`, with REFramework) and Resident Evil Village (`re7_camera.cpp` extended) supported;
  nativepad: the F10 panel idles the controller again after a game's `CloseHandle(NULL)` (Pragmata); auto XInput shows
  a DualSense the game's Sony library ignored in the same session even when the game also creates DirectInput (RE8);
  GAMES.md (RE9/Pragmata need REFramework, Pragmata and RE8 rows); per-engine research tools in `tools/<engine>/` with
  READMEs, including the automatic RE Engine `camera_finder.lua` (first complete run in RE8).

- 2026-10-17 **Monster Hunter Wilds (demo): TDB 81, not supported yet; first finder run found nothing** (tool change only).
  - Gyro4All 1.0.0 in the demo (REFramework v1.5.9.1 +514 installed): `re: TDB 81 header at MonsterHunterWilds.exe+0xCA613E0`,
    `re: TDB 81 is not supported yet; the gyro can't run in this game`. `tools/reEngine/tdbdump.py` also refuses TDB 81
    (reads 69/70/83). Both need TDB 81's header and type/field layouts (REFramework's `TDB_VER >= 81` paths).
  - `camera_finder.lua`, 2 runs: singletons by name `app.PlayerManager`, `app.CameraManager`; `scan done: 60000 objects
    visited (limit reached)`, 915 / 786 rotations; one rotation followed all 3 captures (yaw -99/+129/-139 and
    +150/-169/-71), but the live test found the camera didn't follow it: `No rotation turned the camera`. The script
    didn't say which rotation that was. Changes: camera roots (primary camera GameObject, camera-named singletons) are
    walked completely before the other singletons (`camera roots done: ...` is logged); every rotation that followed the
    captures but failed the live test is listed with its path and how far the camera moved. Syntax checked; copied to
    the demo's `reframework\autorun`, not yet run.
  - MH Wilds has online multiplayer: test offline / solo only (AGENTS.md ground rules).
- 2026-10-17 **TDB 81 (Monster Hunter Wilds) in `re_tdb.cpp` and `tools/reEngine/tdbdump.py`** (built and unit-tested,
  not game-tested; version unchanged).
  - **Layouts** (REFramework `RETypeDB.hpp` `tdb81`, `RETypeDefinition.hpp` `RETypeDefVersion74`): TDB 81 is TDB 83
    minus two things. Header (`tdb81::TDB`, 0xF0 bytes): numTypes 0x08, numMethods 0x14, numFields 0x18,
    **numStringPool 0x54** (83: 0x58; 81 lacks the TDB 82 counter but has a padding u32 before the pointers), so the
    pointers are where 83 has them: types 0x68, typesImpl 0x70, methods 0x78, methodsImpl 0x80, fields 0x88,
    fieldsImpl 0x90, stringPool 0xD8. **`RETypeImpl` names are two int32** (name 0x0, namespace 0x4), as in 69/70;
    83 packs them as 28-bit fields of one qword. Everything else is the 83 shape: type definitions 0x50 bytes, 19-bit ids,
    `member_field` bits 44..63 of the qword at 0x20, `RETypeCLR*` at 0x38, `num_member_fields` bits 33..56 of the
    impl's 0x10 qword, `REField`/`REFieldImpl` (offset in the impl record, 26 bits), `REMethodDefinition` 0xC bytes.
    `via.Application` entries use the TDB 74+ shape (0xC8 stride), already the case for `version >= 74`.
  - Code: `kHeader81` in `re_tdb.cpp`, `type_names` reads packed names only from TDB 83 (`wide` still covers 71+);
    `tdbdump.py` `HEADERS[81]`, `packed_names = version >= 83`. New in `tdbdump.py`: nameless array types (TDB 71+,
    parent `System.Array`, shared impl record) print as `<element>[]` (element type id = bits 19..37 of the type
    definition's second qword); before, 4% of MH Wilds' fields showed no type.
  - Offline check on the demo exe: TDB 81 at file 0xca5f9e0 (va `+0xCA613E0`, matches the in-game log), 293888 types,
    629970 fields, 1834223 methods; `--all` reads every type and field (no unreadable names). RE8/RE7/Requiem/Pragmata
    exes were not on disk, so 69/70/83 were not re-run (their code paths are unchanged). `test-gyro4all.sh`: 378 checks
    pass.
  - **MH Wilds camera classes** (field offsets from the managed field base; fieldptr not in the file):
    - `app.CameraManager` <- `ace.CameraManagerBase` <- `ace.GAElement`1` (static `_Instance`) <- via.Behavior:
      `_MasterPlCamera` +0x60 `app.PlayerCameraController`, `_PlCameraRequest` +0x58, `_IsEventCamera` +0x140,
      `_Setting` +0x50; base: `_CameraControllerList` +0x20, `_OverwriteCameraList` +0x38 (List`1).
    - `app.PlayerCameraController` <- `app.CameraController` <- `ace.CameraControllerBase` <- via.Behavior:
      `_Operator` +0x1C0 `app.cPlayerCameraOperator`, `_AutoRotator` +0x1C8 `app.cPlayerCameraAutoRotator`,
      `_Accessor` +0x208, `_ActionControllers` +0x190 `ace.cActionController[]`, `_OwnerCharacter` +0x228. Base:
      `_Camera` +0x0 `ace.VirtualCamera`, `BaseCamParam` +0x10, `AddCamParam` +0x18, `DirectCamParam` +0x20,
      `CurrentParam` +0x28 (all `ace.cCameraParam`).
    - `ace.cCameraParam`: **`RotationDegree` +0x20 `via.vec2`** (degrees), `RollDegree` +0x48, `FOV` +0x4C, `EyePos`
      +0x0, `LookAtPos` +0x10, `<ParamArgument>k__BackingField` +0x30 (`ace.cCameraParamArgumentBase`).
      The arguments also carry `RotationDegree` (vec2): `app.cCameraParamArgument_ThirdPersonAttach` +0x30,
      `_ThirdPersonDirectPos` +0x30, `ace.cCameraParamArgument_Free` +0x20, `_AttachObject` +0x40, `_FullConst` +0x40,
      `_BattleRide` +0x30.
    - `app.cPlayerCameraOperator` (stick/mouse/gyro input): `_RotAmount` +0x0, `_RotDir` +0x10, `_PadInput` +0x20,
      **`_GyroInputAmount` +0x30** (the game has its own gyro input path), `_MouseRotateAmount` +0x40 (all vec2),
      `_IsRotatePad` +0x7C. `app.cPlayerCameraAutoRotator`: `_AddRot` +0x0 vec2, `_LimitPitch` +0x48.
  - **In game (demo, Gyro4All 1.1.0 build with TDB 81, REFramework installed; no controller connected, so the gyro
    itself was not tested)**: `re: TDB 81 ready: 293888 types` 1.5 s after start, `via.Application` found with 343
    entries, gyro frame hooked before `LateUpdateBehavior`, then `re: no known player camera in this game` (expected).
    The GyroLib panel attached through Streamline's factory. ~5.5 min of play and a clean exit, no crash (anti-tamper
    did not react).
  - `camera_finder.lua` runs in the same session: the only rotation that followed the captures was the GUI map's
    `app.GUIManager.<MAP3D>._CameraCtrl._GUICameraParam._CurrentCameraParamArg._RotYawDeg` (a 5-degree write moved
    nothing). The finder keeps only `via.Quaternion` fields and yaw/pitch-named floats, so it never sees MH Wilds'
    camera rotations, which are `via.vec2` degrees (`ace.cCameraParam.RotationDegree` and the argument classes').
  - Next: a camera model for MH Wilds (candidates: `CameraManager._MasterPlCamera` -> a `cCameraParam.RotationDegree`
    or the operator's `_MouseRotateAmount`/`_GyroInputAmount` as an input to add to), then test in the demo (offline,
    with REFramework).
- 2026-10-17 **`camera_finder.lua` reads `via.vec2` angles** (tool only). Every `via.vec2` field (also `via.Float2`) is
  kept whatever its name, as two candidates, `.x` and `.y`, each tested like a float angle: captures accept degrees or
  radians, either sign, following the camera's yaw or pitch; the live test writes +5 degrees to that one component
  (`get_field`, change `x`/`y`, `set_field`) and the camera axis must move 5 +- 1.5. Results say `via.vec2 x`/`y`
  (path ends in `.x`/`.y`). Meant for MH Wilds' `ace.cCameraParam.RotationDegree`. Non-angle pairs (screen positions,
  UV...) are dropped by the captures. Syntax checked (luaparser); not yet run in a game.
- 2026-10-17 **MH Wilds camera model `mhwilds_camera.cpp`, from the TDB only** (built, 378 checks pass; not yet tested in
  game; version unchanged). Registered last in `find_camera` (no other model matches MH Wilds: it has no
  `app.PlayerCamera`, `app.CameraSystem` or `app.CameraBlender`).
  - Chain: `app.CameraManager` (static singleton slot) `._MasterPlCamera` -> `app.PlayerCameraController`
    `.BaseCamParam` (`ace.cCameraParam`) `.<ParamArgument>k__BackingField` -> its `RotationDegree` (`via.vec2`, offset
    looked up per argument type: `cCameraParamArgument_ThirdPersonAttach` +0x30 and others). Assumed x = pitch,
    y = yaw in degrees (the TDB names rotations `RotX`/`RotY`, e.g. `cCameraWorkPointUniqueFlag_SimpleEase`), signs as
    RE7's (positive about +X looks down, about +Y turns left): both to check on screen. `cCameraParam.RotationDegree`
    itself is treated as an output (there's a `CalcRotationDegreeSkipFlag` next to it).
  - Why the argument and not the action: the main camera actions (`app.CameraMainAction.cIdle` etc.) hold no rotation;
    the controller has `get_RotationDegree`, `addRotationDegree`, `overwriteRotationDegree` (methods; their code can't be
    found offline in TDB 71+: encoded offsets).
  - Pitch limits: `_BasicParam` (+0x68, `app.user_data.PlayerCameraBasicParam`) `._PitchDegreeRange` +0xF4 (via.Range
    start/size), default -80..80; never pushed further out. Event camera: `CameraManager._IsEventCamera` +0x140 (no
    gyro). Pause: `app.PauseManager` (`ace.PauseManagerBase._ActivePause` +0x0, `ace.Bitset._Value` UInt32[]): first
    word non-zero = paused (optional; the status line shows it).
  - Status line: `camera=<argument type> event= pause= rot=(x, y) range= kept=a/b`; `kept` counts the frames where the
    value written last frame was still there: with the stick still, a low count means the game rewrites the argument
    every frame and the rotation lives elsewhere. The log also names CurrentParam's argument when it differs.
  - To test: the gyro turns the camera on foot; yaw/pitch direction; `kept`; mounted (Seikret), event cameras, menus.
  - **First run (user: DualSense, Steam Input on, the game's native DualSense support)**: model resolved, `app.CameraManager`
    slot `+0x123D4108`, `app.PauseManager` slot `+0x123D12A8` (2 s after start; 254 slots). In play:
    `camera cCameraParamArgument_ThirdPersonAttach (current: none)`, `gyro on`; pause bits 0x2 / 0x4 seen in menus and
    loading, `event camera` in a scene; `range=-75..75` (`_PitchDegreeRange`). But `rot=(0.0, 0.0)` in every status line
    (to check whether the camera was turned meanwhile: if so, this argument isn't the rotation), and CurrentParam has no
    argument. **No controller in GyroLib** (`motion data lost (no controller with a gyro)`, `xinput: not shown: no
    controller`): SDL saw no pad at all, so GyroLib's isolated reader under Steam (starts on a Steam virtual pad) never
    started either. Change: `host.cpp` logs Steam's controller environment before GyroLib loads (`gyro: Steam
    environment: ...`: SteamAppId, SteamVirtualGamepadInfo, SDL_GAMECONTROLLER_IGNORE_DEVICES and related), read only.
  - **Second run (user restarted the controller; same Steam Input state)**: SDL found the DualSense (`motion source 1`,
    ~470 samples/s), but `xinput: DualSense Wireless Controller not shown: it creates DirectInput (DirectInput8Create)`
    (it also imports HID.DLL), and the game had no controller input at all (user: the game doesn't actually read the
    DualSense). The RE8 "closed unread" detection didn't fire. New shipped `profiles/MonsterHunterWilds.ini`:
    `XInput=2`, `DualSenseAsDualShock4=0` (as RE7). The camera turn is still untested.

- 2026-10-17 **Auto XInput: native-support signs from use, not from imports** (core `xinput.cpp`; built, 378 checks pass;
  not tested in game; version unchanged). MH Wilds showed the auto mode's two weak signs were wrong: it creates
  DirectInput and imports HID.DLL, but never opens a HID device (no `input: the game opens HID device` line in two
  sessions) and doesn't read the DualSense. RE7 and RE8 had the same false DirectInput sign (MODLOG above); no game
  in this log needed it.
  - **DirectInput**: `DirectInput8Create` alone no longer counts. Its interface's vtable gets `CreateDevice` (index 3)
    patched in place (per vtable, A/W, originals looked up by vtable); a created device whose `GetCapabilities` type
    is joystick..1st-person (`DI8DEVTYPE_JOYSTICK`..`DI8DEVTYPE_1STPERSON`) is recorded with its `DIPROP_VIDPID`, and
    its vtable's `GetDeviceState` (9) / `GetDeviceData` (10) patched. The sign (Any) is the game reading one of those:
    `it reads a DirectInput game controller (VID:PID, GetDeviceState)`. Logs: `the game creates DirectInput (counts
    once it reads a game controller)`, `the game opens a DirectInput game controller (...)`.
  - **hid.dll imported alone** (no libScePad imported/loaded): the Sony sign holds for 30 s after the pad appears
    (Hogwarts opened it at 15 s), then only while the game has opened a PlayStation HID device (`opened_vendor(054C)`,
    enumeration included). Logs: `the game imports the HID library but hasn't opened a PlayStation controller in 30
    s: it doesn't read it itself`, and `... its own PlayStation support is back` if it opens one later.
  - Profile `MonsterHunterWilds.ini` back to `DualSenseAsDualShock4=0` only, so the auto mode is what gets tested.
  - Risks to check: a game that reads a DualSense through Sony's library linked in (hid.dll only) and first opens it
    after 30 s would get the Xbox controller until then (doubled input when it starts); a game that reads pads
    through DirectInput is now translated until its first read.
  - **MH Wilds test of this build (user: DualSense, Steam Input on): no controller input in game.** Log: `Steam
    environment: SteamAppId=4379380 ... SDL_GAMECONTROLLER_IGNORE_DEVICES=(long) SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
    EnableConfiguratorSupport=4096` (GyroLib's SDL still found the DualSense, motion 476 samples/s); `the game opens a
    DirectInput game controller (054C:0CE6, type 0x15)`, never read; 30 s later `shown as XInput controller 0`, but the
    game **never called XInputGetState** (no `the game polls` line in 1.5 min). The auto-mode change worked as designed;
    XInput is just not how this game reads pads.
  - The exe has `SteamInput006` (Steam Input API interface version) and `xinput1_4`/`hid.dll` imports; the TDB has
    `via.hid.DualSenseDevice`, `via.hid.Dualshock`, `via.hid.GamePadDevice`. So with Steam Input on MH Wilds reads the
    controller through the Steam Input API (in-process via steam_api64.dll, untouched by Gyro4All, which doesn't use
    Steam Input), and probably its own HID DualSense code with Steam Input off. Open: whether the controller works in the
    demo without Gyro4All (baseline), with Steam Input on and off.
  - User: the controller works in game **only with Steam Input on** (the run above had Steam Input off for PlayStation
    pads: `EnableConfiguratorSupport=4096`). So for MH Wilds: Steam Input on, no XInput translation needed.
  - **Steam Input on (`EnableConfiguratorSupport=65535`): GyroLib saw no controller** (`no controller with a gyro` for
    the whole session). Steam lists the DualSense in `SDL_GAMECONTROLLER_IGNORE_DEVICES`; MH Wilds reads Steam Input
    itself, so Steam creates no virtual XInput pad, and GyroLib's isolated reader (starts on a Steam virtual pad) never
    starts. Camera: `ThirdPersonAttach` argument found, gyro gate on in play, but `rot=(0.0, 0.0)` for a minute of play:
    not the live rotation.
  - Changes (built, 378 checks pass; not yet tested): `host.cpp` `prepare_steam_environment` (before gyrolib.dll loads):
    logs Steam's controller variables (values cut at 120 chars) and removes `SDL_GAMECONTROLLER_IGNORE_DEVICES` from the
    process environment (`SetEnvironmentVariableA` and `_putenv_s`) unless the game imports or has loaded an SDL2/SDL3 of
    its own; the game's Steam Input API use is unaffected. `mhwilds_camera.cpp`: the status line adds `candidates:
    base= add= direct= current=` (each `cCameraParam.RotationDegree`), `stick=` (`_Operator._RotAmount`) and `mouse=`
    (`_MouseRotateAmount`), to find which one tracks the camera.
  - **Next run (Steam Input on)**: the ignore list was removed (`... removed, so GyroLib sees the physical controller`),
    yet SDL still found no controller: environment isn't the only thing hiding it (presumably Steam's in-process
    module, gameoverlayrenderer64.dll, filters device enumeration; GyroLib's isolated reader exists for that, but only
    starts behind a Steam virtual pad). Camera candidates: `base=(-10.0, 18.6)` -> `(-10.0, 348.7)` after a stick turn
    (user asked to turn right), `current` the same, `add=(0, 0)`, `direct=-`, operator `stick`/`mouse` (0, 0) in the
    snapshots. So **`BaseCamParam.RotationDegree` is the rotation**: x pitch, y yaw in degrees, yaw 0..360, turn right =
    yaw down.
  - Changes (built, 378 checks pass; not yet tested): `host.cpp` adds GyroLib's **Steam Input runtime reader**
    (`gl_steam_runtime_create`, `gyrolib/steam.h`: borrows the game's loaded and initialized `steam_api64.dll`; never
    calls Init/RunFrame/Shutdown or action sets), created once the game has `steam_api64.dll` loaded (checked once a
    second), polled every frame between `gl_sdl_poll` and `gl_update` (the SDL reader's combined `update` split into
    `poll` + `Context::update`), frame serial = our frame count; poll result and `gl_steam_runtime_error` logged on
    change. MH Wilds updates Steam (`UpdateSteam`, priority 197) before our hook (`LateUpdateBehavior`, 259).
    `mhwilds_camera.cpp` now writes `BaseCamParam.RotationDegree` (yaw wrapped to 0..360), still only while
    BaseCamParam has an argument; the candidates stay in the status line.
  - **Test (user: DualSense, Steam Input on): GyroLib reads the gyro through Steam Input.** `Steam Input reader
    started`, `ok`, `motion source 2` (Steam), `motion data available`; the panel lists the Steam Input controller;
    ~85 samples/s in menus (one per game frame; calibration `calib=5`). SDL still sees no pad (the ignore-list removal
    doesn't help; kept for now, harmless). **But the camera write doesn't stick**: in play `kept=4/2529`,
    `rot=(-10.0, 11.2)` unchanged while `applied yaw=2.3 pitch=-45.2`: the game recomputes `BaseCamParam.RotationDegree`
    every frame (it is an output, as `CurrentParam`). The state the camera keeps between frames is elsewhere (its eye /
    look-at positions, a mini component, or the stick input path: `cPlayerCameraOperator._RotAmount`, `_PadInput`,
    `_MouseRotateAmount`, `_GyroInputAmount`).
  - **Gamepad menu shortcut (`ui.gamepad_menu_shortcut=1`) doesn't open the panel**: GyroLib's Steam runtime reader
    supplies motion only (INPUT.md: no physical contacts from Steam's virtual buttons); the shortcut needs normalized
    buttons 4 + 6 from SDL, which can't see the pad. Same for gyro activation and recenter buttons. Possible fix:
    `gl_submit_controls(ctx, endpoint, ...)` for the Steam endpoint with buttons read from the game's own pad state
    (engine module), not done. F10 works.
  - **Change: automatic input test in `mhwilds_camera.cpp`** (built, 378 checks pass; not yet tested in game). The
    model no longer writes `BaseCamParam.RotationDegree` (now only read, as the camera's observed rotation). Once per
    session, when free look is active, not paused, no event camera, and the camera has been still with both stick fields
    at zero for 60 frames, it tests the operator's vec2 inputs (`_MouseRotateAmount`, `_GyroInputAmount`, `_RotAmount`,
    `_PadInput`), each axis x then y: writes value 1 (25 if the camera moved < 1 degree) for 20 frames, clears it,
    watches 20 more frames, and logs `re: input test <field> axis <x|y> = <v> for 20 frames: camera yaw .. pitch ..
    (after the writes: ..), the game resets / keeps the field`. A stick input, pause or event restarts the current
    test. The first field (that order) whose test turned the yaw >= 2 degrees is used: `re: input test done: the gyro
    turns the camera through <field> (yaw on axis .., <scale> deg per unit per frame; pitch ..)`. Turns are added to that
    field before `LateUpdateBehavior`; a field the game didn't reset still holding last frame's turn gets it taken
    out first (`undone=` in the status line). The gyro waits until the test is done (view `(input test pending)`).
    Status line: `input=<field> (yaw axis, deg/unit, pitch deg/unit) turns= undone=` or `input test: <state>`.
  - **Input test in game (user: DualSense, Steam Input on; this run SDL saw the pad directly, `motion source 1`)**:
    every field, both axes, values 1 and 25, 20 frames: `camera yaw +0.00 pitch +0.00`. `_RotAmount`/`_PadInput`:
    "the game resets the field every frame"; `_MouseRotateAmount`/`_GyroInputAmount`: kept (never written with a pad).
    Written before `LateUpdateBehavior`, no input reaches the camera: `app.cPlayerCameraOperator` (methods `update`,
    `padRotation`, `mouseRotation`, `gyroRotation`, `isNeautralGyro`: the game has its own gyro path) recomputes the
    amounts in the same pass that turns the camera. **Dead end**: the operator's input fields from our frame hook.
  - `addRotationDegree` / `get_RotationDegree` / `overwriteRotationDegree` (app.PlayerCameraController): their
    `REMethodDefinition.encoded_offset` (+8) is 0 for every method in the exe on disk (filled at startup), and
    REFramework's `get_encoded_pointer` pattern (`85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ? ? ? ? 48 03 C1 C3`) isn't in
    the demo exe: no offline disassembly. **Dead end offline.**
  - Mini components (`app.PlayerCameraController._MiniComponents` +0x90, a struct
    `MINI_COMPONENT_HOLDER`: Attach, AttachOffset, SmoothControl, Poplook, Lockon, Centering, Lookat, Interpolation,
    ..., `AutoRotate` +0x58 (`app.mcCam_AutoRotate`: `_CaptureRotationDegree` +0x20, `_TargetRotationDegree` +0x30,
    `_RotAmount` +0x40), `Limit` +0x78 (`app.mcCam_LimitRotation`: `_BeforeRotDeg` +0x30 vec2, `_TargetRotDeg` +0x98),
    `WorkerBase`/`WorkerAdd` (`app.mcCam_EaseWorker`), offsets Horizon/Combat/Pitch/...).
  - **Change: the test now tries state fields first** (built, 378 checks pass; not yet tested): `BaseCamParam.RotationDegree`,
    `mcCam_LimitRotation._BeforeRotDeg`, `CurrentParam.RotationDegree` each get +10 degrees on y (yaw) once; after 20
    frames the camera's yaw change says whether it kept the turn (`re: camera test <field> axis y += 10: camera yaw ..
    pitch .. after 20 frames; the game kept/rewrote the field`); a state that didn't take is put back. Then the inputs
    as before. The first candidate with >= 2 degrees of yaw is used (a state: degrees directly, pitch on x).
  - **State test in game (user: DualSense, Steam Input on; `motion source 2`, ~87 frames/s)**: `BaseCamParam.RotationDegree`,
    `mcCam_LimitRotation._BeforeRotDeg`, `CurrentParam.RotationDegree` += 10 on y: `camera yaw +0.00 pitch +0.00 after
    20 frames; the game rewrote the field` for all three; inputs as before, all +0.00. **Dead end**: none of these is
    the state; the game rebuilds them every frame from a rotation kept elsewhere.
  - The exe's code is **encrypted on disk** (anti-tamper): not even `85 C9 75 ?? 33 C0 C3` occurs in its .text, so no
    offline disassembly at all; the code is only readable in memory.
  - **Change (built, 378 checks pass; not yet run)**: `re_tdb` reads the method tables (`Tdb::methods` /
    `methods_impl`: header 0x70/0x78 in TDB 69/70, 0x78/0x80 in 81/83) and has `method_code(tdb, type, name)`: TDB
    69/70 the pointer at +8 of the 16-byte record; TDB 71+ `base + encoded_offset` (+8 of the 0xC record, filled at
    startup), base from REFramework's `get_encoded_pointer` helper found by scanning the exe's code in memory
    (`85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D <disp32> 48 03 C1 C3`, base = helper + 17 + disp). `mhwilds_camera.cpp`
    logs the first 256 bytes of `PlayerCameraController.get_RotationDegree`, `addRotationDegree`,
    `overwriteRotationDegree` once (`re: code of ...: <hex>`, a leading jmp followed), to disassemble offline and
    find the field they read and write.
  - **Run (game at the title screen)**: `re: code of PlayerCameraController.get_RotationDegree: not found` (all three):
    the helper isn't in this build either, even in memory (0 hits for it and for `movsxd rax, ecx; lea; add; ret`
    variants in the decrypted .text). **Dead end**: REFramework's `get_encoded_pointer` pattern for MH Wilds.
  - **Found the base another way** (read-only, from outside with ReadProcessMemory): the encoded offsets are filled in
    memory (PlayerCameraController: 144 methods, e.g. `get_RotationDegree` 0x8671e40, `addRotationDegree` 0x8672220,
    `overwriteRotationDegree` 0x8672150); every one is a multiple of 16. Scoring every 16-aligned base by how many
    `base + offset` addresses follow int3/ret padding: **base = exe+0x4BF2E0** (289/291; next best 77; later 1638/1649).
    New tool `tools/reEngine/methoddump.py` (read-only; README section 2b).
  - **Disassembly** (args: rcx VM context, rdx this, r8 &arg, r9 flag; raw offsets include the field base, 0x50 for
    the controller, 0x10 for the params):
    - `get_RotationDegree` (exe+0x8B31120): `[[[[this+0x60]+0x48]+0x78]+0x20]` (8 bytes) = BaseCamParam (+0x10)
      -> `<EyeAttachSetting>k__BackingField` (cCameraParam +0x38, `ace.cCameraParam.cAttachSetting`) ->
      `<AttachParam>k__BackingField` (cAttachSetting +0x68, `ace.cCameraParam.cAttachSetting.cAttachParam`) ->
      **`_AttachDeg`** (cAttachParam +0x10, via.vec2). Live: `(-10.0, 18.587)` = BaseCamParam.RotationDegree.
    - `addRotationDegree` (exe+0x8B31500): x = arg.x + _AttachDeg.x, y = arg.y + _AttachDeg.y, each through
      exe+0x9D41880 (angle wrap); unless r9, pitch clamped to `[[this+0xB8]+0x114]` .. `+0x114 + +0x118` =
      `_BasicParam._PitchDegreeRange` (start, size; BasicParam field base 0x20); then exe+0x532F520 (setter) with the
      result. `overwriteRotationDegree` (exe+0x8B31430): the same without the addition.
    - The setter (exe+0x532F520) writes the vec2 into both `[param+0x48]` (EyeAttachSetting) and `[param+0x50]`
      (LookAtAttachSetting) attach params.
  - **Change (built, 378 checks pass; not yet tested in game)**: first camera-test candidate is now
    `EyeAttachSetting.AttachParam._AttachDeg` (state; every write also goes to the LookAtAttachSetting's). The unused
    `method_code` (helper-pattern base) was taken back out of `re_tdb`.
  - **Checked live (user asked; written from outside with WriteProcessMemory, rotation only)**: `_AttachDeg` of both
    attach settings, y += 30: `BaseCamParam.RotationDegree` 18.59 -> 48.59 within 0.5 s and still there after 2 s; again
    48.59 -> 78.59. **User: the camera turned left about 30 degrees** (so +y = turn left). x += 10 (-10 -> 0), kept;
    **user: the camera looked up about 10 degrees** (+x = look up).
  - **Change (built, 378 checks pass; not yet tested with the gyro)**: `mhwilds_camera.cpp` simplified: no camera test,
    no input candidates. Turns write `BaseCamParam.<EyeAttachSetting>.<AttachParam>._AttachDeg` and the
    `<LookAtAttachSetting>`'s alike (as the game's setter), yaw right = y down (`kYawSign -1`), look up = x up
    (`kPitchSign +1`; the earlier guess was -1, inverted), pitch kept within `_PitchDegreeRange` (never pushed further
    out), yaw wrapped to 0..360. Status: `camera=<argument> event= pause= rot=(x, y) range=`.
  - **User-verified (DualSense, Steam Input on, Steam's gyro off; gyro through GyroLib's Steam Input reader): the gyro
    turns the camera**, but at moments it is lost (not investigated). **Left here by the user's decision**: MH Wilds
    depends on Steam Input entirely (no controller without it) and Steam Input already has full gyro support for it, so
    further work isn't worth it. Open if picked up again: the moments the gyro is lost (Steam reader motion gaps or
    source switches `motion source 0/1/2`, camera modes), the gamepad menu shortcut (no buttons from the Steam reader).
  - Cleanup (user's request; built, 378 checks pass): removed `host.cpp` `prepare_steam_environment` (the removal of
    `SDL_GAMECONTROLLER_IGNORE_DEVICES` didn't make SDL see the pad, and its `gyro: Steam environment` log line), and
    the `xinput: the game creates DirectInput (...)` line (creating DirectInput no longer decides anything). Kept: the
    Steam Input reader lines, `the game opens a DirectInput game controller`, the HID-import expiry lines.

- 2026-10-17 **Resident Evil 2 remake (Xbox/Microsoft Store GDK build, `re2.exe`, TDB 70): frame hook works, camera
  model `re2_camera.cpp` (RE2 + RE3), runtime TDB dump, singleton fallback** (built, 378 checks pass; version unchanged
  1.1.0; **gyro not yet verified in game**).
  - **Install**: the GDK exe can't be read from disk (`PermissionError`, the WindowsApps ACL/encryption), so
    `tdbdump.py` can't be used. In memory the exe is "protected" like RE9/Pragmata: import table without name tables,
    so no import hooks (`nativepad`/`xinput`: not available, XInput translation off). **`winmm.dll` never ran** (no
    Gyro4All.log in a session where REFramework logged `Loaded: WINMM.dll` 4 s after start, i.e. a late load from
    another path); **`version.dll` loads at start** (`PROXY=version.dll`). The exe's path reads as the package's WindowsApps path, mapped to the install folder; `Gyro4All\` next to it
    can be written. Launch: `shell:AppsFolder\F024294D.GAMEResidentEvil2biohazard2_8fty0by30jkny!re2`.
  - Log: TDB 70 header at `re2.exe+0x7578930`, 80296 types, 302 application entries (`LateUpdateBehavior`(215)), gyro
    frame hooked (original `re2.exe+0x58B1CF0`), first gyro frame on the game update thread, GyroLib and the DX12 panel
    start. **`0 singleton slots in the exe`**: the REGlobals code pattern isn't visible in the protected code.
  - New `managed_object_slots` (`re_tdb.cpp`): when the code pattern finds nothing, every qword in the exe's writable
    sections (then also writable+executable ones) that holds a managed object (its REObjectInfo's type definition lies
    in the TDB's types table) counts as a slot; rescanned every 2 s until the model's singletons are among them. RE2:
    1402 managed objects, 250 ms; `app.ropeway.camera.CameraSystem` at `re2.exe+0x93D9290`.
  - New `TdbDump=` (RE module setting, also written to new Gyro4All.ini files): comma-separated parts of type names;
    after start writes `Gyro4All\Gyro4All-tdb.txt` with every matching type (parent, own fields with offset and field
    type, statics) and each singleton slot's type. Generic instances only for parts with `<`. Without a setting it runs
    only when no camera model fits (filter `camera`). New TDB API `own_fields` (field type ids: TDB 70 REFieldImpl bits
    0..18 of the second u32, TDB 71+ REField bits 38..57, as in tdbdump.py).
  - RE2 camera classes (dump, field offsets from the field base): `app.ropeway.camera.CameraSystem` (singleton
    behaviour): `<BusyCameraController>` +0x48 (`CameraControllerRoot`, the camera in use), `<CameraControllers>` +0x8,
    `MainCameraController` +0x98. `CameraControllerRoot`: `<CameraRotation>` +0x40 (quaternion), `<IsBusy>` +0x1.
    `TwirlerCameraControllerRoot` (stick-turned cameras): `<IsControllable>` +0x90, `<TwirlSpeedYaw/Pitch>`
    (DampingFloat), `IsInputPitchLimit` +0xb0, **`<Pitch>` +0xb8, `<Yaw>` +0xbc (floats)**, `<PitchMin/Max>`
    (DampingFloat) +0xc0/+0xc8, `<YawMin/Max>` +0xd0/+0xd4. Its subclasses `PlayerCameraController` (free look;
    `<IsOpenPause>` +0x128, a `ValueTrigger`1`, value at +0x0 by name lookup; `PrevPitch`/`PrevYaw` +0x17c/+0x180) and
    `PlayerSightCameraController` (aiming). `CameraControlType` enum: PLAYER, PLAYER_SIGHT, FIXED, GIMMICK_FIXED,
    GIMMICK_MOTION, ACTION, EVENT, INVENTORY, INTERPOLATION, TITLE, GAMEOVER, RESULT, FIGURE, DEBUG... RE3 (not checked)
    should have the same classes under `offline.camera` (REFramework's RE3 namespace).
  - Model (`re2_camera.cpp`, tried after RE7's): free look = busy controller is `PlayerCameraController` or
    `PlayerSightCameraController`, `IsControllable`, not `IsOpenPause`; turn = add to `<Yaw>`/`<Pitch>` assuming
    **radians, yaw right = Yaw down (`kYawSign -1`), look up = Pitch up (`kPitchSign +1`): both guesses, to check in game**
    (status line prints `Pitch= Yaw=` raw and the controller's quaternion `rot=(yaw, pitch)` in degrees with RE7's
    convention, so units and signs can be read off a session). The game's own pitch limits apply to the floats.
    At the title screen the busy controller is already a `PlayerCameraController`.
  - REFramework v1.5.9 (+7) in this install stays at "currently initializing" (log ends at `[Hooks] Attempting to
    application entries...`); its menu is open with Input Passthrough off. Not ours; left as installed.
  - Next: in game, check direction and speed on both axes (if the camera turns ~57x too fast/slow, the floats are
    degrees, not radians), aiming, inventory/map (INVENTORY controller), pause menu (`IsOpenPause`), cutscenes; whether
    the game reads the DualSense at all (no XInput translation possible in this protected exe). Then a profile if needed,
    GAMES.md, and RE3.
  - **User-verified in game (RE2, Xbox/Microsoft Store build, `version.dll`, without REFramework): works perfectly**
    (user's words): gyro direction right on both axes with `kYawSign -1`, `kPitchSign +1` and radians. Pad, connection
    and Steam Input state not reported. From the session log: `<Yaw>` -1.908 = quaternion yaw -109.3 (radians, same
    sign as RE7's yaw convention); `<Pitch>` -0.400 with quaternion pitch +22.9 (opposite sign: the controller's
    quaternion looks down -Z, so read_rotation's pitch is inverted there; only the status line uses it). Gyro off in
    `InventoryCameraController`, `GimmickFixedCameraController`, `TitleCameraController` (not controllable; its
    Pitch/Yaw are garbage) and in the pause menu (`IsOpenPause` value at +0x0 works). GyroLib suspended the gyro
    (`enabled=0`) for part of the session in free look (activation/toggle; not investigated). REFramework v1.5.9 is not
    needed (the user tested without it). GAMES.md: RE2 moved to Supported (install as `version.dll`); RE3 not tried.

- 2026-10-17 **RE2: GyroLib's panel didn't idle the game's controller; new `dinputpad.cpp` (protected exes)** (built, 378
  checks pass; version unchanged 1.1.0). **User-verified in RE2 (Xbox build, DualSense, without REFramework): with the
  F10 panel open only the panel reacts, the game stays still, and it reacts again after closing.**
  - Cause: in a protected exe (inline-hook mode) `xinput` and `nativepad` were off, so nothing idled the game's pad.
    How RE2 (this build) reads the DualSense, found step by step: `re2.exe` calls `DirectInput8Create`; `DINPUT8.dll`
    and the exe open the DualSense (`054C:0CE6`); the exe only identifies it (HID IOCTLs 0xB01A8 collection info,
    0xB0193 preparsed descriptor, 0xB019C); **no `ReadFile` on any followed handle** (kernel32's or KernelBase's
    export), no `ReadFileEx`, no Raw Input registration or `GetRawInputData`, no `XInputGetState` call, in gameplay
    with the sticks moving. DirectInput reads below those exports. PCGamingWiki's "XInput only" doesn't hold for this
    build; no XInput emulator was running (ViGEmBus and HidHide installed, no DS4Windows/DSX/Steam, no virtual Xbox pad).
  - **Fix, `dinputpad.cpp`** (inline mode only, installed from `host.cpp`): inline hook on `dinput8!DirectInput8Create`
    -> MinHook on `IDirectInput8::CreateDevice` (vtable 3) -> for game-controller devices (not mouse/keyboard, not
    created by Gyro4All/gyrolib.dll/SDL3.dll) `IDirectInputDevice8::GetDeviceState` (9) and `GetDeviceData` (10).
    Per frame the host passes the panel state and GyroLib's selected pad: the game's own state buffer is remembered
    while SDL has seen the pad at rest for 5 frames (no buttons, sticks |v| < 4000, triggers < 2000), so no knowledge
    of the game's data format or axis ranges is needed; while the panel has the pad, `GetDeviceState` returns that
    buffer (fallback without one: DIJOYSTATE/2 buttons cleared, hats centred) and `GetDeviceData` returns no events.
    RE2: 3 devices of type 0x15 (gamepad), 6 axes, 15 buttons; ~1200 states idled in 7 s of open panel.
  - Also in this round (kept, as tested): `nativepad` now works in inline mode: inline hooks on `kernel32!CreateFileW`/
    `CloseHandle` and `kernelbase!ReadFile`/`GetOverlappedResult(Ex)`; opens by Gyro4All, `gyrolib.dll` and
    **GyroLib's `SDL3.dll` (unpacked to GyroLib's own runtime folder under the user's local app data, not
    `Gyro4All\`)** are never followed (following SDL's handle had idled the panel itself); no DualShock 4 identity in
    inline mode. Plus inline-mode diagnostics (each other module that opens HID devices, first reads, IOCTL codes,
    `ReadFileEx`, Raw Input registrations/reports with Sony reports idled while the panel is open, XInput polls).
    None of these paths carries RE2's input; they cover protected games that read Sony pads directly.
  - Not done: buffered DirectInput events of a button held while the panel opens are dropped (a release could be
    missed).
- 2026-10-17 **One DirectInput path for every game: `dinputpad.cpp` in both hook modes** (built, 378 checks pass;
  RE2 redeployed; **not yet tested in game**). Before, unprotected games had `xinput.cpp`'s DirectInput hooks, which
  only gave auto mode its sign ("it reads a DirectInput game controller") and were installed only with `XInput=1`;
  nothing idled DirectInput behind GyroLib's panel. Now:
  - `dinputpad::install()` from `host_configure` after `xinput::install`, in every game and XInput mode:
    `DirectInput8Create` through the game modules' imports (normal exes) or dinput8.dll's export inline (protected
    exes); then `IDirectInput8::CreateDevice` and the game controllers' `GetDeviceState`/`GetDeviceData` patched in
    their vtables (xinput.cpp's `VtableHook` moved here, up to 4 vtables for the A/W flavours; replaces the MinHook
    code hooks of the first RE2 version). Devices created by Gyro4All, `gyrolib.dll` or `SDL3.dll` are skipped (the
    vtables are shared with SDL's DirectInput backend; xinput.cpp's version counted SDL's devices too).
  - Panel idling as before (rest snapshot, no buffered events), now also in normal exes; `dinputpad::frame` runs every
    frame in every game.
  - xinput.cpp keeps only the sign: `dinputpad::on_read(&di_read)` in auto mode, called the first time the game reads
    each controller. Log lines moved from `xinput: the game opens a DirectInput game controller` to
    `dinputpad: the game opens a DirectInput game controller (VID:PID, type, axes, buttons)`.
  - No installed unprotected game has logged a DirectInput read yet (checked the Gyro4All/GyroUE logs of the installed
    games), so the normal-exe path is unverified.
  - **User-verified** 2026-10-17: the same build works in the other DualShock 4 / DualSense (native pad) and XInput
    games, and RE2's panel still leaves the game idle.
- 2026-10-17 **Resident Evil 3 (remake) works out of the box with RE2's camera model** (user-verified; Game Pass /
  Microsoft Store build, DualSense, installed as `version.dll`, same build as RE2). From its log: TDB 70 (87733 types),
  gyro frame before `LateUpdateBehavior`, 0 singleton slots in code, so `CameraSystem` was found through the exe's data
  (1357 managed objects); camera model `offline.camera.CameraSystem`. Field offsets differ from RE2 and are all
  resolved by name: `<BusyCameraController>` +0x50, `TwirlerCameraControllerRoot` `<IsControllable>` +0xa0, `<Pitch>`
  +0xc8, `<Yaw>` +0xcc, `CameraControllerRoot.<CameraRotation>` +0x40; `<IsOpenPause>` is an `offline.ValueTrigger`1`
  (value +0x0). Same signs as RE2 (`kPitchSign` +1, `kYawSign` -1). Like RE2 it reads the DualSense through DirectInput
  (`DINPUT8.dll` opens 054C:0CE6, 6 axes, 15 buttons); `dinputpad` idled it behind the panel (`reads idle ... passed
  through again`), and XInput translation is off (protected exe).
- 2026-10-17 **Build: Gyro4All 1.2.0** (version chosen by the user, for the new DirectInput controller handling).
  `build/release/Gyro4All-v1.2.0.zip` (winmm.dll, gyrolib.dll, licenses, 10 profiles, readme + docs; no settings or
  logs). What's new since 1.1.0: Resident Evil 2 and 3 remakes supported (Game Pass builds, `re2_camera.cpp`, no
  REFramework; install as `version.dll`); `dinputpad.cpp`: the game's DirectInput controllers read idle while GyroLib's
  panel has the controller, in every game and hook mode; nativepad's inline-hook mode for protected exes; RE Engine
  singletons found in the exe's data and the runtime `TdbDump=`; Monster Hunter Wilds groundwork (TDB 81 and a camera model, not supported yet).
- 2026-10-17 **RE Engine `CameraWatch=1` diagnostic; RE3 "camera the gyro can't turn" investigated** (built, 378 checks
  pass, deployed to RE3; not committed). `watch_fields` (`re_camera.cpp`): every 500 ms logs the member fields (with
  the parents') of the active camera object and of `CameraSystem`, one level into the managed objects they reference,
  that changed, by name (`watch: <type> changed: ...`); a full listing when the object changes; stops after 4000 lines.
  Models fill `CameraFrame.object`/`system` (only RE2/RE3 so far). Off by default (`CameraWatch=0` in `kSettings`).
  - RE3 run 1: `EventCameraController` (offline.camera, not a twirler: `<CameraRotation>` +0x40, `<CameraPosition>`
    +0x30, `<CameraParam>.FieldOfView`, `_RefActorController` = `offline.timeline.CameraActorController` with
    `_BeforeMotionId` = shot number, `_OwnerEventID`) is the cutscene camera: every change follows a shot change or a
    scripted drift/zoom; no look-around input on it. Its `<Pitch>`/`<Yaw>` reads in the status line are other fields
    (turn() never writes them: free_look needs a player twirler).
  - RE3 run 2 (user moved the mouse left/right then up/down in the scene): the scene is the normal
    `PlayerCameraController` (ctl=1, `gyro on` view) with narrowed limits: `<PitchMax>`/`<PitchMin>` (+0xe0/+0xd8,
    `offline.DampingFloat`, `<Current>`) damp from 1.204/-1.047 to +-0.262 rad, and `<YawMax>`/`<YawMin>`
    (+0xec/+0xe8, floats) +-0.262 rad around the scene's yaw (`<Yaw>` stayed within -0.585 +- 0.262). The mouse moved
    `<Pitch>`/`<Yaw>` only up to those limits; `IsInputPitchLimit` +0xc0 = 1. The gyro adds to the same floats, so the
    game clamps it the same way. In this run GyroLib never enabled the gyro (`enabled=0` throughout): RE3's
    `gyrolib.ini` has `context.1.gyro.activation=3` (GL_HOLD) with `activation.trigger=3`, i.e. only while the trigger
    is held, and the user used the mouse.
  - Correction (user): the first try already had the gyro always on, and it didn't move the camera. A per-frame write
    check (status line `writes=N changed by the game before the next frame=M (back X deg, our steps Y deg)`) settled
    it: in normal gameplay 0 of 213 writes changed; in the restricted scene **1487 of 1487** were undone, by as much
    as the gyro had turned (3752 vs 3743 deg). In that scene the camera is in forced-twirl mode:
    `PlayerCameraController.<ForceTwirlerParam>` (+0xf8, `offline.camera.ForceTwirlerParam`: Time +0x0, Yaw +0x4,
    Pitch +0x8, BeforeYaw +0xc, BeforePitch +0x10, Interrupt, PermitOverwrite, SetPitch, SetYaw) holds the forced centre
    (Yaw 5.698 rad = -0.585, the centre of the +-0.262 box), `<ForceTwirlerTime>` +0x100. The game rebuilds
    `<Pitch>`/`<Yaw>` every frame from that centre plus an input offset that is in no reflected field of the camera or
    of the objects it references (CameraWatch saw only Pitch/Yaw change while the mouse moved).
  - Experiment: writing the gyro to `ForceTwirlerParam.Pitch/Yaw` in restricted scenes. All 160 writes lasted, but only
    during the ~0.5 s swing to the scene's aim: then the game sets `<ForceTwirlerParam>` to null (`<ForceTwirlerTime>`
    1.0) and keeps rebuilding `<Pitch>`/`<Yaw>` (with a slow scripted sway) from state outside the reflected fields.
    User saw it move for an instant, then lock. Removed.
  - **Stick fallback (experiment, deployed to RE3, not tested yet)**. Core: `Engine::stick_fallback(x, y)` every frame
    after `apply`; `host.cpp` passes it to `dinputpad::set_stick`, which adds it to the right stick (Z right, Rz down,
    within the game's `DIPROP_RANGE`) of what `GetDeviceState` returns (standard joystick formats; not while the panel
    has the pad). RE2/RE3 model: a direct write counts as undone when next frame the angles are back within half the
    step of where they were before it (steps > 0.11 deg only; the player's own stick moves them elsewhere); 8 undone in
    a row start the fallback. Then `turn()` adds the gyro to an owed turn instead of writing; every frame the owed turn
    loses what the camera actually turned (closed loop), fades 3 %/frame and is kept within +-8 deg; deflection = 0.12
    + 0.88 * owed/5 deg (0 under 0.3 deg). A direct probe write every 90 frames ends it when kept; leaving free look or
    another camera ends it too. Status line: `stick fallback: owed (p, y) deg, stick (x, y)`. Known: the loop also
    counters the scene's scripted sway, within the owed-turn limits.
    The gyro's turn in the 8 undone writes that start it is owed from its first frame (logged as `starting with the
    (p, y) deg they lost`), so the detection doesn't swallow the start of the movement.
  - `StickFallbackTest=<seconds>` (RE Engine setting, test only, default 0): during every other period of that length
    the RE2/RE3 model skips the gyro's writes and reports them as undone, so the fallback starts and ends anywhere in
    normal gameplay; on-screen notices mark each switch. Set to 10 in RE3's `Gyro4All.ini` for the user's feel test
    (CameraWatch back to 0).
  - First test run (user: "good enough", small hiccup at the switch; DualSense, RE3 Game Pass): switches in after the
    8 undone frames and out on the next probe; the game's DirectInput axes are -32767..32767 (272-byte DIJOYSTATE2).
    Fast turns saturated the stick (`stick (-1.00, ...)` with 5.2 deg owed): RE3 turns at most about 81 deg/s at full
    stick, so the owed turn hit its 8 deg cap and the rest was dropped (one start owed 22.7 deg, cut to 8).
  - Speed matching: the stick's share of full deflection is now the gyro's speed (its turn this frame / frame time) over
    the game's speed at full stick, plus owed/5 deg as the correction. The game's speed is learnt during the fallback
    from what the camera turned for last frame's share (shares over 0.25 in the same direction only, 5 % per frame,
    bounded 20..720 deg/s, starts at 81, kept between fallbacks; status line and `stick fallback off` log show it).
    Below the game's top stick speed, stick and gyro turn at the same speed whatever the game's camera-speed setting;
    above it, only the game's setting helps.
  - Second test (user): speed matching made little difference, and **the right stick was almost dead while the
    fallback ran**. Cause: the loop took every camera turn off the owed turn, the player's stick turns included, and
    then deflected the stick the other way to make up for it. The learnt speed wandered 35..157 deg/s for the same
    reason. Fix: `Engine::stick_fallback(player_stick, x, y)`; host passes whether the selected SDL pad's right stick is
    past 6000; while it is, only the turn our last share should have made (share * learnt speed * frame time) is taken
    off the owed turn, and nothing is learnt.
- 2026-10-17 **Stick fallback made universal (core `stick_fallback.cpp`)** (built, 400 checks pass incl. 7 new
  simulated-game tests; deployed to RE3; not tested in game yet in this form). User verdict on the RE2/RE3 prototype
  with the stick fix: "Perfect". Now:
  - Core `StickFallback` (no game memory access, unit-tested): `frame(known, pitch, yaw, camera, player_stick, now)`
    before the turn, `direct()`, `turn(pitch, yaw, written)`, `stick(x, y)`, `status()`. Detection: a write is
    undone when next frame the angles are within half the step of where the camera would be without it, kept when
    within half the step of where it would be with it, both after the turn our own stick should have made (found by
    the tests: a probe write judged against the bare pre-write angles looked kept because our stick had moved the
    camera, ending the fallback); no verdict while the player's stick moves. Pitch and yaw deltas wrapped to +-180
    (Unreal keeps pitch 0..360). Give-up: 45 frames of deflection over 0.5 that turn the camera less than 1 deg end
    it for that camera ("the stick turns nothing": the game reads the pad through a path Gyro4All can't change).
    Learnt speed starts at 90 deg/s.
  - `Engine::view_angles(pitch, yaw, camera)` replaces the RE-only `stick_fallback`: RE models through
    `Camera::angles` (RE2/RE3 `<Pitch>`/`<Yaw>`; RE7/RE8, Requiem and Pragmata `read_rotation` with yaw negated, as
    its yaw grows to the left; MH Wilds the two floats read fresh, with `kPitchSign`/`kYawSign`); Unreal
    `read_view_rotation` of the active view (not `WorldRotation` views, nor `Match=Camera` ones, whose rewritten
    rotation is camera_match's cue to try the next member), camera = view object ^ view id.
  - Injection on every pad path: `dinputpad::set_stick` (Z/Rz), `xinput::set_stick` (the shown controller in
    `our_state`, a real XInput controller in `real_state` while it is the selected one), `nativepad::set_stick`
    (selected controller's right-stick bytes in the game's own reports, CRC recomputed).
  - Host: `turning` gate, `g_stick_fallback.frame` before `apply`, apply only when `direct()`, `turn` after, stick to
    all three paths (0 while the panel has the pad); `status: stick fallback: ...` line in the periodic status.
  - Removed: the RE2-only fallback and write statistics in `re2_camera.cpp`, and the `StickFallbackTest=` setting
    (deleted from RE3's `Gyro4All.ini`). Kept: `CameraWatch=` (off by default).
  - Docs: ADVANCED.md feature bullet and log rows (and the DirectInput log row renamed to `dinputpad:`).
  - Untested: the XInput and nativepad injection, and any engine other than RE3's; the Unreal readback sign matches
    `add_rotator` (both add pitch/yaw as stored).
- 2026-10-17 **Settings panel for D3D11 / D3D10 / D3D9 games: `Gyro4All\gyro4all.dll`** (built, 400 checks pass; NOT
  tested in any game yet). User's choice after an options analysis: GyroLib's static ImGui panel inside the game's own
  swapchain, in a separate DLL loaded only when needed, 1:1 with the DX12 panel, no conflicts with other ImGui panels
  (ReShade). Findings that shaped it:
  - GyroLib 1.3.1 SDK: `panel.h` (`gl_panel_create(ctx, NULL)`, `gl_panel_function_key`, `gl_panel_draw(w, h, dpi)`,
    `gl_panel_last_result`) in `lib/gyrolib_panel.lib` + `lib/gyrolib_imgui.lib` = ImGui **1.92.9b core only, no
    backends**; headers `include/gyrolib/imgui` identical (line endings aside) to upstream tag v1.92.9b. Both libs are
    **/MD** (`/FAILIFMISMATCH:RuntimeLibrary=MD_DynamicRelease`) while winmm.dll is /MT: they can't be linked into it
    (LNK2038), hence the DLL. Moving winmm.dll to /MD was rejected: Severed Steel's old MSVCP140 would then break the
    proxy itself before `deps.cpp` can warn.
  - API.md "Threading": one owner thread per context; only the DX12 frontend (snapshots) may draw from another one. So
    the static panel's ImGui frame is built on the game thread (`overlay::draw(pad)` in `host.cpp`, right after
    `gl_update` and the pad selection: MENUS.md says to refresh the model after acquisition and update) and only its
    draw data is rendered on the presenting thread; one SRWLOCK in the DLL keeps frame, window messages and renderers
    apart (ImGui 1.92 backends update the context's font textures while drawing). Closed panel: no ImGui frame at all.
  - OVERLAY.md: the DX12 panel and the static one share widgets/settings; the DX12 frontend adds the Steam Input mouse
    bridge (`gl_overlay_win32_message`; the `input.steam_mouse` row only appears with it), capture mask, gamepad
    navigation on the selected controller, HDR composition and the 250 ms heartbeat.
  - ImGui backends: `imgui_impl_dx11/dx10` honour `ImDrawData::FramebufferScale`, `imgui_impl_dx9` doesn't; the DX11
    one backs up most state but not render targets nor HS/DS/CS (its own comment); DX9 uses a D3DSBT_ALL state block and
    D3DPOOL_DEFAULT buffers and textures (released before Reset).
  - Third party: `third_party/imgui-1.92.9b` (git-ignored; `git clone --depth 1 --branch v1.92.9b
    https://github.com/ocornut/imgui third_party/imgui-1.92.9b`), only its `backends/` are compiled (win32, dx9, dx10,
    dx11); `imgui.h` comes from GyroLib's SDK. Its LICENSE.txt equals GyroLib's `ImGui.txt`, already shipped.
  What was built:
  - `src/panel/` (target `gyro4all_panel`, OUTPUT_NAME `gyro4all`, /MD): `gyro4all_panel.h` (C interface, one export
    `g4a_get_api(G4A_ABI_VERSION=1)`; `dumpbin /exports` shows only it), `panel_main.cpp` (ImGui context: no
    imgui.ini, `AddFontDefaultVector`, keyboard+gamepad nav, `NoMouseCursorChange`, `StyleColorsDark`, as GyroLib's
    demo; Win32 backend with `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`; gamepad keys from the selected SDL pad mapped like
    `imgui_impl_sdl3`; capture = mouse+keyboard+gamepad while open; Escape closes unless a popup is open, as the demo;
    input cleared on each opening; 250 ms present heartbeat closes the panel), `panel_dx11.cpp`
    (`SwapDeviceContextState` into a state object of its own, then `ClearState` + swap back; RTV made per frame, no
    backbuffer kept; HDR: `SetColorSpace1` value, else FP16 = scRGB, everything else SDR (see the Mortal Shell test below); panel drawn into
    an RGBA8 texture and composed at 203 nits, HDR10 against a copy of the scene with PQ decode/encode and
    Rec.709 -> Rec.2020; colour write mask RGB), `panel_dx10.cpp` (`D3D10CreateStateBlock` with all states, from
    System32 d3d10.dll), `panel_dx9.cpp` (state block + saved render targets/depth surface, own Begin/EndScene,
    `D3DRS_SRGBWRITEENABLE` off, `InvalidateDeviceObjects` before Reset/ResetEx). `d3dcompiler_47.dll` delay-loaded.
  - Core: `com.hpp` (vtable slot patching moved out of overlay.cpp, shared), `panel_host.cpp/.hpp` (loads the DLL with
    `LOAD_WITH_ALTERED_SEARCH_PATH` from the Gyro4All folder once a target exists; D3D9 hooks: `Direct3DCreate9(Ex)`
    imports, IDirect3D9 slots 16/20, device 16 Reset, 17 Present, 121 PresentEx, 132 ResetEx, swapchain 3 Present, nested
    present guard). `overlay.cpp`: non-DX12 swapchains go to `on_other_swapchain` (D3D11 or D3D10 by `GetDevice`;
    slots 8, 13, 22, 38 `SetColorSpace1`, 39); new import hooks `D3D11CreateDevice(AndSwapChain)`,
    `D3D10CreateDevice(AndSwapChain)`, `D3D10CreateDevice(AndSwapChain)1` (import mode only); F10 through
    `gl_panel_function_key`; `available()`/`capture()` cover both frontends; a consumed `WM_INPUT` gets
    `DefWindowProcW` for its cleanup; once gyro4all.dll is attached no DX12 overlay is created (one frontend per context).
  - ReShade/wrappers: on the first D3D10/D3D11 device (import mode, not `OverlayFactory=game`) the System32 DXGI
    factory vtable is hooked as well (`hook_system_factory(lazy)`), and the D3D9 hooks do the same with System32
    d3d9.dll. A swapchain/device created inside another hooked creation (a wrapper creating the real one) wins over the
    proxy around it (thread-local creation depth), so the panel draws after the wrapper's effects and overlay. DX12 is
    kept as it was: factories hooked only lazily (vtable not in `g_game_factory_vtables`) are ignored for D3D12
    swapchains and don't trigger `release_before_create`.
  - Scripts: package and deploy copy `Gyro4All/gyro4all.dll`. Docs: README, TROUBLESHOOTING, GAMES (DX11 rows: "not
    verified yet"), ADVANCED (feature bullet, `panel:` log lines), CREDITS, LiS profile comment.
  - Known gap vs. the DX12 panel: the Steam Input mouse row/bridge. Untried idea: a `gl_overlay` created without
    `gl_overlay_dx12_init` used only as the window bridge next to the static panel; unknown whether its 250 ms render
    heartbeat or `gl_overlay_process` would then close the static panel. Needs a test with Steam Input on, or a
    "bridge only" mode from GyroLib.
  - Not verified: everything in game. To test: DX11 SDR (Mortal Shell, LiS) side by side with a DX12 game's panel (tabs,
    every control type, tooltips, language, recalibrate, reset/recommended, autosave), F10 and Create+Options, mouse,
    keyboard and pad (DualSense USB and BT, Steam Input on and off), windowed/borderless/exclusive fullscreen, resize,
    alt-tab; HDR10 and scRGB; ReShade as dxgi.dll with effects and its UI open; a DX12 game must not load gyro4all.dll;
    `tools/bench.sh` with the panel closed.
- 2026-10-17 **gyro4all.dll panel, first game test: Mortal Shell (Epic, UE 4.25, DX11): works (user: "ha funcionado
  bien"; controller, connection and Steam Input state not reported).** Log: `overlay: D3D11 swapchain ... (dxgi.dll)`,
  `panel: gyro4all.dll loaded`, `panel: D3D11 renderer ready on device ... (feature level B000)` 17 ms after loading,
  three open/close cycles, `nativepad: the game's controller reports made idle (GyroLib's panel has the controller)`
  while open. perf: Present hook 1.2-1.4 us with the panel closed, 15-21 us open; the gyro frame ~190 us avg open
  (ImGui frame + gl_panel_draw) vs ~50 us closed. The System32 factory was hooked lazily after the first swapchain
  (`system DXGI factory vtable hooked for D3D10/D3D11 under any wrapper`), no wrapper in that folder.
  - **Finding: UE 4.25 DX11 presents RGB10A2 (format 24, 3840x2160) and never calls `SetColorSpace1`**; on an HDR
    desktop GyroLib's AUTO rule made the panel HDR10 (`panel drawn as HDR10`). DXGI shows a swapchain without a colour
    space as G22/P709 SDR whatever its format (FP16 aside, scRGB), so that was wrong (PQ-encoded panel on an SDR image).
    Fixed: no colour space set = SDR, except FP16 = scRGB; `output_is_hdr` removed. Rebuilt and redeployed to Mortal
    Shell; colours of the fix not checked in game yet.
  - The old `GyroUE\` folder was gone before the first start (no migration line in the log; not removed by the deploy
    script), so Gyro4All started with default `gyrolib.ini`/`Gyro4All.ini`. Backups: `um backup` `mortalshell-saves`,
    `mortalshell-gyroue` (with `winmm-GyroUE-0.8.0.dll`).
- 2026-10-17 **Jedi: Fallen Order (Steam, UE4, DX11): new build installed for the panel test.** The folder already had
  Gyro4All 1.2.0's winmm.dll (17:36) plus a copied gyro4all.dll: that winmm.dll predates the panel, hence its
  `overlay: swapchain is not D3D12; panel unavailable`. The folder also has ReShade as `dxgi.dll` with RenoDX
  (`renodx-ue-extended.addon64`, HDR), Streamline (`sl.interposer.dll`, DLSS/DLSS-G DLLs), OptiScaler leftovers and UE4SS
  (`dwmapi.dll`): the wrapper case (panel on the real swapchain under ReShade, HDR colour space from RenoDX) gets its
  first test here. Backups: `jedi-saves`, `jedi-gyro4all` (with the previous winmm.dll). Not tested yet.
- 2026-10-17 **gyro4all.dll: game thread froze on a mouse click on the panel's close button (Mortal Shell, user).**
  Log stopped at the last `overlay: panel state open` (no status lines after: the game thread hung, not the render).
  Cause: ImGui's Win32 backend calls `ReleaseCapture()` on button up when the window holds the capture (Unreal holds it)
  and `SetCapture` on button down when nothing does; Windows sends `WM_CAPTURECHANGED` synchronously, which re-entered
  `window_message` on the same thread while it held the panel's non-recursive SRWLOCK: deadlock. Fix: the lock is a
  `CRITICAL_SECTION` (re-entrant), and mouse buttons go to ImGui directly (`AddMouseButtonEvent`) instead of through
  the backend, so the panel never changes the game's mouse capture. Rebuilt (400 checks), redeployed to Mortal Shell
  and Jedi: Fallen Order; not re-tested in game yet.
- 2026-10-17 **gyro4all.dll: no mouse cursor over the open panel (Mortal Shell, user: hover and clicks worked).** The
  game hides the OS cursor in gameplay. Fix: each panel frame reads `GetCursorInfo`; when the cursor isn't showing (or
  has no image) ImGui draws its own (`io.MouseDrawCursor`), logged on change (`panel: OS cursor hidden: the panel draws
  its own`). The game's cursor state is never changed. Possible side effect to watch: two cursors in a game menu that
  hides the OS cursor and draws a software one. Rebuilt, redeployed to Mortal Shell and Jedi; not re-tested yet.
- 2026-10-17 **gyro4all.dll panel verified by the user in Mortal Shell and Jedi: Fallen Order** (both DX11, UE4; with the
  capture and cursor fixes; user: works as is in both; controller, connection and Steam Input state not reported).
  Jedi log, the wrapper case (ReShade as `dxgi.dll` with RenoDX, Streamline, UE4SS): the System32 factory was hooked
  lazily from the device-creation hook before any swapchain; the real swapchain was captured inside ReShade's creation
  and ReShade's proxy skipped (`overlay: swapchain ... not used: the panel draws on the real swapchain inside it`), so
  the panel draws after ReShade's effects. RenoDX makes the swapchain FP16 (format 10) and calls `SetColorSpace1(1)`:
  `panel drawn as scRGB` (colours fine per the user). Mouse clicks, close button and the drawn cursor worked. Still
  untested: DX10 and DX9 (user is looking for games), HDR10 (PQ) composition, exclusive fullscreen, Steam Input mouse gap.
- 2026-10-17 **Build: Gyro4All 1.3.0** (version chosen by the user, for the DX11/DX10/DX9 settings panel).
  `build/release/Gyro4All-v1.3.0.zip`: winmm.dll, new `Gyro4All/gyro4all.dll` (GyroLib's static ImGui panel with
  ImGui 1.92.9b Win32/DX9/DX10/DX11 backends, loaded only in non-DX12 games), gyrolib.dll 1.3.1, licenses, profiles,
  readme. Changes since 1.2.0: the panel for D3D11/D3D10/D3D9 games (verified in DX11: Mortal Shell, and Jedi: Fallen
  Order with ReShade/RenoDX scRGB; DX10/DX9 untested), drawn on the real swapchain under wrappers, HDR composition,
  mouse buttons without touching the game's capture, ImGui cursor when the OS one is hidden; plus the core stick
  fallback (commit 54fcbe2). Docs: GAMES (Mortal Shell and Jedi rows), README panel row. Panel code is in commit 8cb1274;
  the version bump and these doc updates are not committed yet.
- 2026-10-17 **RE4 remake (`re4.exe`, Steam): TDB 71 support and a camera model `re4_camera.cpp`** (built, 400 checks
  pass; user-verified in game: "correct functionality"; version unchanged). Installed with the deploy script as
  `winmm.dll`; REFramework was in the folder. Docs: GAMES (RE4 row, removed from the wanted list).
  - User report with 1.3.0: `gyrolib.ini` never created. Log: `re: TDB 71 header at re4.exe+0x97F1DD0`,
    `re: TDB 71 is not supported yet; the gyro can't run in this game`: the attach thread stops before the frame hook,
    so GyroLib never starts and never writes its ini. The folder also has REFramework (`dinput8.dll`), ReShade,
    OptiScaler (`dxgi.dll`).
  - **TDB 71 layouts, checked offline on the exe** (TDB at file 0x97f0bd0): the header is TDB 70's (numTypes 0x0C,
    numMethods 0x10, numFields 0x14, numStringPool 0x50; types 0x60, typesImpl 0x68, methods 0x70, fields 0x80,
    fieldsImpl 0x88, stringPool 0xD0). The table sizes give **RETypeDefinition = 0x48 bytes** (types table / 162644
    types; TDB 69/70/74+ are 0x50), REMethodDefinition 0xC, REField 8, RETypeImpl 0x30, REFieldImpl/REMethodImpl 12.
    Otherwise the TDB 74 shapes: 19-bit type ids, impl index at bit 38 of the second qword, member_field at bits 44..63
    of the qword at 0x20 (3000 of 3000 types with fields agree), names two int32 in RETypeImpl, num fields bits 33..56
    of RETypeImpl+0x10, field offset/name in REFieldImpl. RETypeCLR* assumed at 0x38 like 74+ (only filled at runtime;
    REFramework RETypeDefVersion71 is 74 without the last pointer). `via.Application` entries: the below-74 shape
    (0xD0 stride), not checked.
  - Code: `Tdb::type_size` (0x48 for 71), `header_layout(71)` = TDB 70's; `tdbdump.py` reads 71 (`typedef_size`).
    162644 types, 236556 fields, 1112853 methods.
  - **RE4 camera (TDB, offline)**: `chainsaw.CameraSystem` (AppSingleton) `_CameraControllerList`
    (`chainsaw.MainCameraController[]`, `_Role` +0x10 (`CameraDefine.Role`: only `Main` = 0), `_BusyCameraController`
    +0x28). Busy controllers derive from `chainsaw.CameraControllerRoot` (`_CameraRotation` +0x50, `_IsInputControl`
    +0x82); the player's are `chainsaw.PlayerCameraController` and `chainsaw.VehicleCameraController`, both
    `chainsaw.TwirlerCameraControllerRoot` (`_Yaw` +0x98, `_Pitch` +0x9C radians, `_YawMin/_YawMax` floats,
    `_PitchMin/_PitchMax` `chainsaw.DampingFloat`; also `<GyroStartPitch/Yaw>` and `_OldEnableGyro`, the game's own
    gyro plumbing). Others: Inventory, Merchant, ItemGet, ItemSearch, KeyTreasure, Gimmick*, PhotoMode, GameOver,
    MainMenu, FigureWindow camera controllers. Pause: `share.PauseManager` (AppSingleton) `_CurrentTargetPauseUpdate`
    +0x30 (u32). No `chainsaw.PauseManager`.
  - `re4_camera.cpp`: RE2's twirler model on those names. The gyro adds to `_Yaw`/`_Pitch` of a twirler busy controller
    while `_CurrentTargetPauseUpdate` is 0; signs RE2's (yaw -1, pitch +1). `_IsInputControl` only shown (view and
    status line), not required. Registered after RE2 in `find_camera`.
  - To test in game: TDB ready, `via.Application` entries found (0xD0 shape), both singletons found in the slots, the
    gyro turns on foot / aiming / boat, yaw and pitch directions, pause bits 0 in play and non-zero in the pause menu,
    `_IsInputControl` during play; doubled motion with the game's own pad handling and REFramework.

- 2026-10-17 **Frostbite module base: Dead Space (2023)** (`src/engines/frostbite/`, `gyro_frostbite`, `Engine=Frostbite`;
  verified in game, EA app build `v-1.1.14.17871`, DualSense, RivaTuner closed):
  - The game: Frostbite (`Data\layout.toc`, `Data\initfs_Win32`, the module's detection), no anti-cheat, DX12 only,
    `Dead Space.exe` ~400 MB with Denuvo + EA OOA, base 0x140000000, no ASLR. The EA app exe's code is encrypted on disk
    and its imports are resolved by the protector, so the core uses inline hooks (`Engine::inline_hooks()`).
  - Loading: `winmm.dll` in the game folder loads only with RivaTuner closed (`RTSSHooks64.dll` pulls System32's WINMM
    in first, even with the game excluded); `version.dll` never works (Defender's `MpOav.dll` loads the system one
    first). The EA app makes the install folder read-only on every launch, so Gyro4All writes its files to
    `%LOCALAPPDATA%\Gyro4All\<exe stem>\` when `<game>\Gyro4All` isn't writable.
  - Reflection (`fb_types.cpp`), layout from Frosty Tool Suite's `Bf2042` type classes (read for reference only, no code
    taken): TypeInfo (static, no vtable) +0 info data, +8/+0x10 prev/next; info data +0 name (packed, byte-aligned
    strings), +0x0C flags (kind = (flags>>4)&0x1F), +0x0E size, +0x20 namespace, +0x32 field count, +0x38 parent/fields.
    Found by shape (the string "DataContainer" -> its info data -> the TypeInfo whose links lead to other TypeInfos):
    23,885-23,889 types, ~5 s after start; `TypeDump=*` writes `Gyro4All-types.txt`.
  - The game reads the DualSense through DirectInput8 (`GetDeviceState`, DIJOYSTATE2).
- 2026-10-18 **Tooling used on Dead Space**: core `WriteWatch` (`src/core/writewatch.cpp`; `WriteWatch=on` at start, then
  an address in the ini while the game runs): hardware write breakpoint, `r:` read/write, `x:` execute (with
  `WriteWatchFields` sampled from rcx), real stack unwind (`caller #N`), `di+<off>` = the buffer of the game's last
  `GetDeviceState`, engine-named addresses (Frostbite `cam` = the newest camera behaviour's rotation interface,
  re-resolved every 500 ms), self-disarm after 100,000 hits; works on the protected exe with no Denuvo reaction.
  `dinputpad` logs each place the game reads a controller from. External read-only scripts in `tools/frostbite/`
  (README): `findimm.py`, `riprefs.py`, `funcof.py`, `statefollow.py` + `stateanalyse.py`, `forcefloat.py` (write test),
  `levelrate.py`, `hop.sh`, `disasm.py`; offline `exefile.py` (below).
- 2026-10-18 **Frostbite camera model and frame source** (`fb_camera.hpp`, `deadspace_camera.cpp`, `Engine::frame_source`;
  verified in game):
  - One model per game in code, like reEngine (no profile file). Dead Space's finds
    `HybridRotationInputChimeraBehavior` (the Chimera camera's pad behaviour) without exe addresses: TypeInfo by name ->
    its getType (`lea rax,[rip+TypeInfo]; ret`) -> the vtable slot holding it (slot 0) -> the update at slot 16 (EA
    exe+0xFF0040, Steam exe+0xFF00E0; code checked: `cmp byte ptr [rcx+119h],0` at +6). The slot is swapped for a hook
    (data in the exe, no code changed): the game runs fine with it.
  - Every camera mode change (aim on/off) makes a new behaviour object; the old one updates ~0.4 s more. All stop in the
    pause menu, menus and cutscenes, so "a behaviour updated in the last 100 ms" is the gameplay gate.
  - The game thread stops calling PeekMessageW ~10 s after start, so Gyro4All's frame (and GyroLib's panel) never ran in
    play. Frostbite supplies the frame from its own thread woken by each Present (`overlay::on_present`, 50 ms wait,
    8 ms timer without Present): status lines, ~150 frames/s, the panel opens with F10.
  - Turning the behaviour's rotation interface (`[this+0xF8]`, vf48 get / vf70 set, degrees) moved only the camera: on
    foot Isaac followed in jerks, aiming only the weapon camera moved inside a cone. Isaac turns from his own input.
- 2026-10-18 **What turns Isaac: the character's action levels** (in game, gyro off; EA app build addresses):
  - Global action map (buckets `[exe+0x5605FE8]`, 67 buckets, nodes {u32 hash, u32 index, next}): **EIAYaw -> 11,
    EIAPitch -> 8**. The input state holds the levels as floats at **state+0x500 + index*4** (EIAYaw +0x52C, EIAPitch
    +0x520), hold times at +0x780 + index*4, action bits at +0xA18/+0xA2C/+0xA40; its aim angles at +0x60/+0x64 are
    written from the final camera (outputs).
  - **Forcing state+0x52C to 0.5 from outside (`forcefloat.py`) turned Isaac and the camera together, like the stick,
    on foot and aiming (L2 held).** The levels are the injection point.
  - The player's input update (EA exe+0x1A55550, Steam exe+0x1A571C0) runs through a 16-byte delegate invoker
    (`mov rdx,[rcx+38h]; mov rcx,[rcx+30h]; jmp <update>`, one of 41) whose address is held in the exe's writable data
    (EA exe+0x5F68B08, Steam exe+0x5EF7B08): swapping that qword hooks the update. this = [context+0x30]; the
    player-input object P = [[this+0x90]+0x118]; the state = [P+0x148].
  - Turn speed per level (`levelrate.py`, level forced, angle sampled): **yaw** 0.25 -> 11, 0.5 -> 24, 0.6 -> 31,
    0.7 -> 43, 0.8 -> 63, 0.9 -> 105, 1.0 -> ~180 deg/s (capped at 1; the same aiming); **pitch** about 0.7x that,
    positive level = up. Positive yaw = right.
  - Writes from outside the update never count (tried after the update, before its copy out, into the copy): the
    mapper rebuilds each level from the controller every tick (`movss [r13+rax*4+0x500],xmm1`, EA exe+0x1CDC0D0) and the
    update uses it within microseconds. The outside forcing only worked because it wrote every ~7 us.
- 2026-10-18 **Dead Space Steam build: code plain on disk** (offline): a second install, the Steam build (link time
  2023-02-15 02:46 UTC; EA app build 02:36). Still the Denuvo layout (random section names, a 0x13B967FD-byte RWX
  section, base 0x140000000, no ASLR), but its code section is plain on disk (entropy 6.55 bits/byte; EA's 8.00) and it
  has a full `.pdata`, so it can be disassembled without running the game. Code layout differs from the EA app build by
  a few KB (level store exe+0x1CDDE20, EA +0x1D50 lower; input output exe+0x1A56C00, EA +0x1C70 lower): Gyro4All finds
  everything at run time. New `tools/frostbite/exefile.py` (function bounds, disassembly, call sites, rip-relative
  references, qword holders, byte search, call chains).
- 2026-10-18 **From the input update to the levels, traced offline** (Steam build addresses):
  - input update exe+0x1A571C0 -> exe+0x1A73A60([this+0x90] the input manager) -> exe+0x1A74150 (loops over the
    player-input objects in the vector at manager+0x540) -> **exe+0x1A73BE0(P, frame)** -> the mapper exe+0x1CDD2A0
    (direct calls only).
  - Mapper: first loop, each mapping entry's input concept value from exe+0x1CB46D0 (the device's stick value with the
    game's processing) into a value buffer; second loop over the action list, **level = clamp(base + value)** (base =
    a second state's level when P+0x258 bit 2 is set, unused in play), stored at exe+0x1CDDE20, then exe+0x1CDC9D0 only
    sets/clears the action's bits (not a consumer).
  - In exe+0x1A73BE0, branch with a controlled entity (the one in play): mapper, then at once **P's vtable slot 16 with
    (P, dt in xmm1, state in r8)**, then the post of the state (exe+0x119ACE0), exe+0x1AEDCA0, slots 12 and 14, and the
    per-player snapshots: every consumer comes after slot 16. Slot 16 (exe+0x1A6F150) walks the input components and
    calls their vf48(component, ?, state, dt).
- 2026-10-18 **Solution: the gyro as action levels from P's slot 16** (`deadspace_camera.cpp`, commit e0b52f8; **verified
  in game**, Steam build, DualSense; connection and Steam Input state not recorded):
  - The input update's invoker is swapped for one that, before the game's update, reads P and its state and, on the
    first call, checks P's vtable slot 16 (prologue up to `mov eax,[rip+..]`, `mov rsi,rcx; mov rdi,[rip+..]` at +0x1A,
    `sub rsi,rax; xor ebx,ebx` at +0x24) and swaps it for `hk_player`. `hk_player` adds GyroLib's queued turn to EIAYaw /
    EIAPitch, only for the player's state, through the curves (stick level -> speed, + gyro degrees / dt, -> level,
    capped at 1), then calls the game's. dt = the game's argument (seconds; 33.3 ms, a 30 Hz input tick). The behaviour
    hook stays only as the gameplay gate; GyroLib runs on the frame thread and only queues the turn.
  - **User: works as expected, smoother and correct**: Isaac, his arm and the camera turn together, on foot and aiming.
    Log: P's vtable exe+0x3F691E0 (derived class), slot 16 code check passed; every slot-16 call carried the player's
    state; 1954 level writes, 209 capped. P is replaced on level changes with the same class (still hooked).
  - Open: fast turns beyond full stick are capped (180 deg/s yaw, ~110 pitch); the start of small motions may lag if
    the hold-time ramp applies; to check on the EA app build.
  - **Crash after ~6 min** (the game's own minidump): access violation in game code exe+0x2480C43 (`mov rcx,[r15]; mov
    rax,[rcx]; call [rax+8]` with a garbage vtable), on a job thread in the character's update (reached through
    exe+0x1A57590, another player-component phase that Gyro4All doesn't hook), during a camera change. No Gyro4All
    address on the saved stack, the gyro had been off for over 2 minutes (no level writes), the faulting object isn't
    the input state. Not ruled out entirely (no full memory in the dump). Next: play longer with the gyro on; if it
    repeats, the same scene without Gyro4All.
- 2026-10-18 **DOOM (2016), id Tech 6: first engine module `src/engines/idTech/`** (Steam build 13954591, exe dated
  2024-03; no anti-cheat files, no packer, no Denuvo string). Findings: the exe imports `WINMM.dll` (the proxy loads),
  `XINPUT1_4.dll` (by ordinal) and `DINPUT8.dll`, and pumps with `PeekMessageA`, not W (core now hooks both).
  `DOOMx64.exe` is a launcher: with `r_renderAPI "1"` in `DOOMConfig.local` it relaunches `DOOMx64vk.exe`, and as first
  process it kept the session mutex (and the legacy GyroUE mutex and the locked log), so the real game stayed a plain
  proxy; the core now hands the session to a child exe of the same folder (`CreateProcessA/W` hooks). In
  `DOOMx64vk.exe` the DllMain thread never pumps: two other threads do (callers `DOOMx64vk.exe+0x15A34A2` and
  `+0x15A33F3`); before the first frame the core adopts the thread that gets messages for the visible game window.
  Then GyroLib starts, the DualSense is shown as XInput controller 0 and motion arrives at ~210 Hz (DualSense, Steam
  Input state not recorded). The Unreal fallback (no idTech module then) found no GUObjectArray and its heap scans
  stalled frames (~400 ms each). The module detects `base\gameresources.resources` next to the exe and sends the gyro
  as right-stick input, open loop (`Engine::stick_speed`, `StickSpeed=180` deg/s at full stick): not tested in game yet.
  The exe carries id's type-info names (`idPlayer`, `idPlayerAimAssist`...): the route to the view angles.
- 2026-10-19 **DOOM (2016): id's type info in the exe; view angle fields** (build 13954591, `DOOMx64vk.exe`). The exe
  holds id Tech's generated type info as static tables: `classTypeInfo_t { name, superName, size, 0, variables }`
  (5653 classes) and `classVariableInfo_t { type, ops, name, u32 offset, u32 size, ..., comment }`, stride 0x48; MSVC
  RTTI too (`.?AVidGameLocal@@`, `.?AVidPlayer@@`, `.?AVidUsercmdGenLocal@@`). Offline reader:
  `tools/idTech/typeinfo.py`. Fields: `idGameLocal.playerList` 0xa5d48 (`idEntityList<idPlayer>`, 32 bytes, layout
  not in the type info), `idGameLocal.playerControllers` 0x4a0000, `idGameLocal.gamestate` 0x29a1cc,
  `idGameLocal.gamePausedTime` 0x49c4c0; `idPlayer.physicsObj` 0x14e58 (`idPhysics_Player`, embedded);
  `idPhysics_Player.current` 0x1aac (`playerPState_t`), `.viewAngles` 0x2040 (`idAngles`, 3 floats);
  `playerPState_t.deltaViewAngles` 0x70; `idPlayerController.ucmdTracker1` 0x32c8 (`idUCmdTracker`: `usercmd` 0x3c,
  `viewAngles` 0xe8c, `cmdAngles` 0xea4); `usercmd_t.angles` 0x10 (`short[3]`). The `idUsercmdGenLocal` input
  generator isn't in the type info (engine side). The module now reads the tables at runtime, finds `gameLocal` (static
  object in .data, by its RTTI vtable) and the player through `playerList`, and logs `viewAngles` and
  `deltaViewAngles` once a second: not run in game yet. Debug cvars in the exe: `g_drawPlayerViewAngles`,
  `g_tracePlayerViewAngles`, `pm_minviewpitch`/`pm_maxviewpitch`. Stick cvars behind the stick mode's imprecision:
  `joy_edgeAcceleration*`, `joy_gammaLook`, `joy_smoothing*`, `joy_dampenLook`, `joy_deadZone`.
- 2026-10-19 **DOOM (2016): `idPhysics_Player.viewAngles` follows the camera; `deltaViewAngles` stays 0** (in game,
  mouse turns; first gyro module read only). `gameLocal` is a heap object (RTTI vtable `DOOMx64vk.exe+0x201E3D8`, 2
  candidates), not a static one; the local `idPlayer` (vtable `+0x22A1298`) is found in the heap by its vtable plus its
  embedded `physicsObj`'s vtable (`idPhysics_Player`, `+0x28589B0`), ~110 ms per scan once write-combined and
  uncached pages are skipped (Vulkan maps ~7 GB of GPU memory into the process; scanning it took ~35 s).
  `gameLocal.playerList` read all zero in the front end; the player object changes on level load. While turning:
  viewAngles yaw 0 -> 83 -> -70, pitch -73 .. +51, roll 0; `current.deltaViewAngles` 0,0,0 throughout. Senses
  assumed from id's convention (yaw positive = left, pitch negative = up), to confirm by writing.
- 2026-10-19 **DOOM (2016): native gyro works in game** (user-confirmed; DualSense, connection and Steam Input state
  not recorded; Vulkan `DOOMx64vk.exe`). Direct write, as in Unreal: `apply()` adds GyroLib's turn to
  `idPlayer.physicsObj.current.deltaViewAngles` (`-pitch`, `-yaw`: id's pitch is positive down, yaw positive left),
  pitch kept within +-89 degrees from `viewAngles`, yaw delta wrapped to +-180; `view_angles()` reads `viewAngles`.
  The turn holds (the game keeps the delta), 165 writes/s at 165 fps, gyro frame ~60 us (p99 < 0.2 ms) on the game
  thread. Right after spawning, the game undid 8 writes in a row (intro/spawn animation), which started the core's
  stick fallback; it gave up 3 s later ("the stick turns nothing") and direct writes went on. Without a player
  (front end, loading) there is no gameplay view; the pause menu and other menus aren't detected yet (calibration
  "any time", `Engine::sees_menus`). The open-loop stick mode (`Engine::stick_speed`) was removed.
- 2026-10-19 **DOOM (2016): the gyro's gate (pause, map, glory kills)**, matched in game to a known sequence (normal,
  pause menu, map/codex, upgrades?, glory kills, pause + quit). `gameLocal` is recreated with each level: it is
  re-confirmed (its `playerList` holds the player) whenever the player changes; the first heap candidate seen at
  start is another object whose fields stay 0. On the confirmed one: `frameParms` (`gameFrameParms_t`, +0x2bb398)
  `.isMenuActive` (+0x14) = 1 in the pause menu, `.isDialogActive` (+0x12) flickers on its quit dialog, `.advanceTime`
  (+0x18) = 1 in play; `gamePausedTime` (+0x49c4c0) = -1 while running, the pause's game time in the pause menu and
  the map/codex; `gamestate` = 3 in play; `skippableSync` = 0x1fffffe01fffffe throughout (no cutscene seen). On the
  player: `inhibitFlags` (+0x14c4c) = 0x1f for ~1 s per glory kill and while the map is open (with
  `shouldConsumeNonGuiInput` = 1), 0x18 for 3-7 s (probably the upgrade screen), else 0; `deathCamera` 0; the
  Windows cursor shows only in the front end and loading. Gate: paused = isMenuActive or gamePausedTime != -1
  (confirmed gameLocal only); no gameplay view = inhibitFlags != 0, deathCamera set, or `advanceTime` 0 (loading:
  the game undid the gyro's writes there and started the stick fallback; replaced a fixed 2 s after spawning);
  gameLocal's flags only from the gameLocal whose playerList holds the player, checked every frame (after a
  checkpoint load the old one read `gamePausedTime` 0, taken for a pause: ~4 s without gyro while the mouse already
  worked); the player and gameLocal scans run at once when needed, then every second until found; cursor =
  shouldConsumeNonGuiInput. `sees_menus()` is true again (calibration in menus). Not tested in game yet; no
  cinematic or death seen so far.
- 2026-10-19 **Settings panel (F10) in Vulkan games: works in DOOM (2016)** (`DOOMx64vk.exe`, 3840x2160, 2-image
  swapchain, format 44 = B8G8R8A8_UNORM; opened and closed several times from the front end and in game, also across a
  swapchain recreation). winmm.dll (`src/core/panel_vk.cpp`) import-hooks `vulkan-1.dll` (`vkCreateInstance`,
  `vkCreateDevice`, `vkGetDeviceQueue`, `vkCreateWin32SurfaceKHR`, `vkCreate/DestroySwapchainKHR`,
  `vkQueuePresentKHR`, `vkDestroyDevice`, plus `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`); gyro4all.dll
  (`src/panel/panel_vk.cpp`, panel ABI 2) draws with ImGui's Vulkan backend into the presented image (render pass
  load/store, PRESENT_SRC both ways), submits on the game's present queue waiting on its semaphores, and the present
  waits on the panel's. Headers: Khronos Vulkan-Headers v1.4.309 in `third_party/` (build only). Two dead ends on the
  way, both inside the game's own `vkQueuePresentKHR`: (1) ImGui's texture upload calls `vkQueueWaitIdle` on the
  queue it was given, which froze DOOM: the panel now records ImGui's texture requests into its own command buffer
  (`ImDrawData::Textures = nullptr` for the backend); (2) loading ImGui's functions through `vkGetDeviceProcAddr`
  first gave it a broken `vkGetPhysicalDeviceMemoryProperties`: its vertex buffers got a memory type that can't be
  mapped (`VK_ERROR_MEMORY_MAP_FAILED`), then it wrote through a null mapping (access violation, DOOM's crash
  report); now everything ImGui uses comes from `vkGetInstanceProcAddr`, and a start-up map test disables the panel
  instead of crashing. Colours: no sRGB handling yet. OpenGL (`DOOMx64.exe`, `r_renderAPI 0`) still has no panel.
- 2026-10-19 **Build: Gyro4All 1.4.0** (version chosen by the user). `build/release/Gyro4All-v1.4.0.zip`: same file
  list as 1.3.0 (winmm.dll, `Gyro4All/gyro4all.dll`, `gyrolib.dll`, licenses, profiles, readme). New since 1.3.0:
  the id Tech module with native gyro in DOOM (2016) (type info and RTTI read at runtime, the gyro added to
  `deltaViewAngles`, gate for pause, map, glory kills, death camera and loading); the settings panel in Vulkan games
  (panel ABI 2); core: `PeekMessageA` frame hook, a launcher hands the session to the exe it starts, opt-in
  adoption of the window-pumping thread (`Engine::adopt_pump_thread`), `Engine::sees_menus`. Tested in game: DOOM
  (2016) Vulkan with a DualSense (gyro, gate, F10 panel, checkpoint loads); Cronos: The New Dawn (no regression from
  the core changes). Not tested: DOOM's OpenGL exe (no panel there yet).
- 2026-10-19 **Settings panel (F10) in OpenGL games: works in DOOM (2016)** (user-confirmed: `DOOMx64.exe` with
  `r_renderAPI "0"`, NVIDIA OpenGL 4.6 driver; panel opened, used and closed several times from the front end and in
  game, by F10, Escape and the mouse; Vulkan `DOOMx64vk.exe` re-tested, still works). winmm.dll
  (`src/core/panel_gl.cpp`) import-hooks `gdi32.dll!SwapBuffers` (what `DOOMx64.exe` imports; it imports no
  `wglSwapBuffers`) and `opengl32.dll!wglSwapBuffers`; `WindowFromDC` gives the panel's window
  (`panel_host::set_gl_target`). gyro4all.dll (`src/panel/panel_gl.cpp`, panel ABI 3: new `g4a_api::gl_present(hdc)`)
  draws with ImGui's OpenGL 3 backend (its own imgl3w loader; no OpenGL import in gyro4all.dll) in the game's current
  context, before the original swap. Around the backend's own save/restore it sets and restores the draw framebuffer
  (0), the default framebuffer's draw buffer (`GL_BACK`), the pixel unpack buffer and unpack skips, the colour mask,
  and `GL_FRAMEBUFFER_SRGB`, rasterizer discard, logic op, alpha-to-coverage and clip distances (off). Nothing waits
  on the GPU; every GL call runs under SEH, and a fault turns the panel off for the session. A context change shuts
  the backend down in the old context (or in a throwaway one when the old one is gone), never in the game's.
  Two input bugs found in the test, both also in Vulkan: (1) clicks did nothing (the panel's cursor moved, from
  `GetCursorPos`): DOOM reads the mouse through DirectInput and its window gets no `WM_*BUTTON*` messages; the panel
  now also polls the buttons (`GetAsyncKeyState`, swapped buttons handled) every frame while open and the game window
  is in the foreground; (2) the panel's clicks and the mouse's motion also reached the game behind it: DOOM imports
  `DirectInput8Create` and no raw input; `dinputpad.cpp` now tracks the game's DirectInput mouse and keyboard, and while
  the panel captures them (`overlay::capture`, every frame from `host.cpp`) `GetDeviceState` returns zeros and
  `GetDeviceData` keeps only releases (a key or button held when the panel opens is released in the game too). Log:
  "dinputpad: the game opens a DirectInput mouse/keyboard", then "mouse idle, keyboard idle" while the panel is open.
  Not released (still 1.4.0 in `src/CMakeLists.txt`); colours: no sRGB handling, as in Vulkan.
- 2026-10-19 **DOOM Eternal (id Tech 7): native gyro through the idTech module** (Steam build 25216728,
  `DOOMEternalx64vk.exe`, no packer, no Denuvo/EAC strings; Denuvo anti-tamper removed in 2023). The type info parses
  with the same layout (6927 classes); id Tech 7 renames the player's physics: `idPlayer.physicsObjHavok` 0x8a50
  (`idHavokPhysics_Player`, 0x4870 bytes: `viewAngles` 0x3f10, its own `deltaViewAngles` 0x3f1c, `current` 0x3f28,
  `currentGameTime` 0x4298 int64), `playerPState_t.deltaViewAngles` 0x80, `idPlayer.inhibitFlags` 0x87fc. No
  `idGameLocal` class (the game state lives in the engine's `idMapInstanceLocal`, not in the type info;
  `idGameSystemLocal` has `state` 0x44 and `mapInstance` 0x50); `gameFrameParms_t` lost `isMenuActive`. The DllMain
  thread pumps (`PeekMessageA`), no launcher. Read-only in game: `viewAngles` follows the camera, the physics object's
  own `deltaViewAngles` stays 0, `current.deltaViewAngles` holds the spawn yaw (90) and is wrapped by 360 by the game
  itself (90 -> -270 -> 90). The gyro added to `current.deltaViewAngles` (same code as DOOM 2016) turns the camera
  and holds (user-confirmed; DualSense, Steam Input off for the game). Gate for id Tech 7: paused while the player's
  `currentGameTime` hasn't changed for 150 ms (confirmed in game: pause and map freeze it, it advances again on
  resume; also loads), no gameplay view while `inhibitFlags` != 0 (0x1f87f at spawn) and in a new player's first 2 s
  of advancing game time (the game undid the gyro's writes 0.05-1.2 s after its time started, three spawns in a
  row, and started the stick fallback; a fixed 1 s from finding the player wasn't enough).
  Then the stick fallback also started as the pause or map opened (time already stopped, the 150 ms threshold not
  reached yet, 8 frames of writes "undone" in ~50 ms): the threshold is now 60 ms (a 60 Hz tick changes the time every
  16.7 ms), `view_angles` reports nothing after 20 ms without a tick, both on the performance counter
  (GetTickCount64 moves in ~15.6 ms steps). Tested 5 min with mouse and gyro: 9 pauses/maps and 3 spawns, gyro
  applied on 99% of frames, ~84 us per frame; one stick fallback left, 0.3 s right after leaving a pause (27 degrees
  "lost": the game may reset the view on resume), not reproduced since. The F10 Vulkan panel works there (swapchain format 64,
  A2B10G10R10: colours not checked).
- 2026-10-19 **DOOM: The Dark Ages (id Tech 8): type info layout and the player's view, offline** (Steam 3017860,
  buildid 25755602, `DOOMTheDarkAges.exe` ~130 MB; `.bind` section = SteamStub; no Denuvo strings, Denuvo removed in
  July 2025; ReShade installed as `dxgi.dll` in the game folder). id Tech 8's tables: `classVariableInfo_t` stride 0x58
  (`type, ops, name, offset|size<<32, -1, 0, comment, ...`), `classTypeInfo_t` `{name, superName, hash, size, 0,
  variables, ...}` (0x58 apart): with that, 9969 classes (`typeinfo.py` and the module's reader now try both layouts
  and keep the one that finds more classes). The player: `idPlayer : idActorPlayer` (0x3a1a0); its physics is behind a
  pointer, `idPlayer.idPlayerPhysicsInfo` 0x2df8 -> `idPlayerPhysicsInfo` (0xa68): `viewAngles` 0x1f0, own
  `deltaViewAngles` 0x1fc, `current` 0x208 (`playerPState_t` 0xf8, `deltaViewAngles` 0xa4), `currentGameTime` 0x5f8.
  `inhibitFlags` is no longer on `idPlayer` (only in HUD/logic-node classes). The module detection marker is
  `base\common.resources` there (no `gameresources.resources`). The module handles id Tech 8 (heap scan validated
  through the physics pointer); see the next entry for the game.
- 2026-10-19 **DOOM: The Dark Ages: native gyro works** (user-confirmed: DualSense, Steam Input off, frame generation on
  and off). Findings in game:
  - `idTechLauncher.exe` (id's Electron mod launcher, in the game folder) loaded the proxy first, pumped messages, ran
    gyro frames and so kept the session from `DOOMTheDarkAges.exe` (no handover after a first frame). New
    `Engine::is_launcher(exe)`: the idTech module names `idTechLauncher.exe`, where Gyro4All now stays a plain proxy.
  - The game reads the DualSense itself (imports HID.DLL): Gyro4All doesn't show it as XInput.
  - View model, sampled at 10 Hz with the input command (`idPlayerPhysicsInfo.command` 0x160, `idUserCmd.angles` 0x1c,
    `short[3]`): `viewAngles = command.angles + current.deltaViewAngles` (mod 360), as in id Tech 6/7; the physics
    object's own `deltaViewAngles` stays 0. The gyro added to `current.deltaViewAngles` turns the camera.
  - But during some animations (some weapons' reloads, spawning) the game recomputes `current.deltaViewAngles` every
    tick to make the camera follow them: the gyro's writes are lost there (the mouse still works: it goes through the
    command). Dead end: holding the gyro 0.5 s whenever the delta changed (98 holds in a row: reload animations change
    the delta on top of the gyro's write), then whenever a write was undone (still chained holds after each
    expiry). Kept: no hold at all (option A: the gyro has no effect only while such an animation runs). Seen once:
    the core's stick fallback then turned the camera through the game's own pad reads (learnt pitch 126, yaw
    160 deg/s at full stick) and handed back to direct writes 1.1 s later. Not tried: the gyro as mouse input, or the
    engine's input accumulator (`idUsercmdGenLocal`, no type info).
  - Gate: `currentGameTime` frozen = paused (pause, loads), the first 2 s of a new player's running time; no inhibit
    flags on the player in id Tech 8.
  - **F10 panel: not working yet.** The game presents through Streamline's `streamline\production\sl.interposer.dll`,
    which exports 229 Vulkan functions: after the first frames no present reaches the import hooks (`renderer not
    ready` on F10, with frame generation on and off), and the surface is made there too (window now found by a
    fallback: the process's largest visible top-level window). Next: hook the interposer's Vulkan exports when it
    loads, as RE Requiem's DXGI factory (2026-10-16).
- 2026-10-19 **DOOM: The Dark Ages: F10 panel works through Streamline's Vulkan interposer** (user-confirmed: panel
  opened, drawn and closed with frame generation on and off, frame generation toggled in game without freezes or
  crashes; DualSense). `src/core/panel_vk.cpp` now hooks two paths with the same code (hooks templated on the path):
  the exe's `vulkan-1.dll` imports, as before, and `sl.interposer.dll`'s exports, inline (MinHook) as soon as it loads
  (`hooks::watch_dll`, the loader notification that `overlay.cpp` now also uses for Streamline's DXGI factory):
  `vkCreateInstance`, `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`/`2`, `vkCreateWin32SurfaceKHR`,
  `vkCreate`/`DestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkQueuePresentKHR`, `vkGetInstance`/`DeviceProcAddr`.
  Each hook calls its own path's original, so a swapchain made through the interposer has its images read through
  it; a hooked call made inside another one on the same thread is nested and passed through (`thread_local` depth),
  so the panel draws once per present. ImGui still loads everything from the loader's `vkGetInstanceProcAddr`.
  Findings from the log:
  - Instance and device are made through `vulkan-1.dll`. The first swapchain too, and its first presents also go
    through `vulkan-1.dll`; about 1 s later the game makes a new swapchain through the interposer and presents only
    there (`Vulkan presents of swapchain ... (made through sl.interposer.dll) come through sl.interposer.dll`). That is
    why only the first presents reached the import hooks before. Later swapchains came through either path
    (`vulkan-1.dll` once, then the interposer, with the same handle reused), as frame generation and settings were
    changed. Images: 2, 3840x2160, format 64.
  - The interposer's `vkGetInstanceProcAddr("vkCreateWin32SurfaceKHR")` returns a function of its own, not its export,
    so the surface is still not seen (logged once). The window still comes from the main-window fallback, which is the
    right one.
  - gyro4all.dll (`src/panel/panel_vk.cpp`): no device-wide wait inside a present any more. Rebuilding the panel's
    frames for a new swapchain waits only for the panel's own fences (it used `vkDeviceWaitIdle`), a renderer restart
    inside a present skips `vkDeviceWaitIdle`, and the frames are also rebuilt when the swapchain's images change under
    the same handle.
  - **Open bug (input):** with the OS cursor visible (menus) the panel works with the mouse and the d-pad. With the
    cursor hidden (in gameplay, panel opened by F10 or the controller shortcut) neither the mouse nor the d-pad
    navigates, though F10 and the shortcut still open and close it. In gameplay the game registers its raw-input mouse
    with flags 0x230 (`RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE`), and with 0x1 in menus. **Not reproduced** in a second
    session with a diagnostic build (user: main menu with and without the cursor, pause menu and gameplay, before and
    after enabling frame generation; mouse and d-pad worked everywhere). The log showed 14 openings: SDL pad buttons
    reaching ImGui, ImGui nav active and a window focused, the mouse position following `GetCursorPos` (the cursor starts
    at the window centre, 1920,1080), no `ClipCursor`/`SetCursorPos` calls from the game while the panel was open (a
    temporary hook counted them), clip rect = the whole screen. Cause unknown. gyro4all.dll now logs the panel's input
    state 1 s and 3 s into each opening (`input +1000ms: imgui mouse ... pad 1 buttons ... navActive ...`) to catch it
    if it comes back.
  Not tested after this change: DOOM 2016 and DOOM Eternal (Vulkan without Streamline), RE Requiem (DXGI Streamline path;
  only the shared loader-notification helper changed there).
- 2026-10-19 **Wolfenstein: The New Order (id Tech 5, PC Game Pass / Xbox app build 1.14.1.0): first load; the Xbox
  launcher kept the session** (recon in `games/WolfensteinTNO/MODDING_PLAN.md`). The package's entry point is
  `GameLaunchHelper.exe` (the Xbox app's generic launcher, in every PC Game Pass game's folder), which starts
  `WolfNewOrder_x64.exe`. `WolfNewOrder_x64.exe` can't be read from disk (Game Pass encryption): no offline exe analysis,
  only its memory once running. With `winmm.dll` in `Content`, both processes loaded the proxy (the game also loads
  `XINPUT1_3.dll` from the DirectX runtime package, `DINPUT8.dll`, `OPENGL32.dll`, `VERSION.dll`), but only
  `GameLaunchHelper.exe` logged (`engine: none detected ..., using Unreal`). It took the session mutex and started the
  game without our `CreateProcessA/W` hooks seeing it (no `session: handed to` line), so the game stayed a plain proxy:
  no frame hook, no F10 panel. Fix (`dllmain.cpp` `attach`): `GameLaunchHelper.exe` never takes the session (checked
  before the mutex). With that, `WolfNewOrder_x64.exe` runs Gyro4All (in game, same day) but no frame ever ran: the
  engine fell back to Unreal (not adopting the pump thread), and the game pumps `PeekMessageA` on two threads that
  aren't the DllMain thread (`+0x33117B` and `+0x3310C7`, ~60 calls/s each in play), so no GyroLib, no F10 panel and
  no XInput controller although `XINPUT1_3.dll!XInputGetState` (ordinal 2) was patched and polled. It also imports
  `GetMessageA`/`DispatchMessageA`, opens DirectInput mouse and keyboard, and swaps OpenGL buffers. Fix: the idTech
  module detects id Tech 5 (`base\master.index` + `base\chunk0.resources`) for the pump thread adoption, and its scan
  thread stops after the type info/RTTI log when the player's view fields aren't all found (no heap scans against an
  unmapped generation). Built, 400 checks passed. **In game (same day): `engine: idTech (detected)`; thread 33860
  (`PeekMessageA` from `+0x33117B`) adopted as the game thread; GyroLib starts; the OpenGL panel renderer is ready
  (GL 4.6 context); the DualSense (Steam Input not involved: not a Steam game) is shown as XInput controller 0 and the
  game polls it.** id Tech 5 **has id's type info**: 3328 classes, parsed with the id Tech 6/7 table layout, but none of
  the DOOM field names exist (`idPlayer.physicsObj`, `idPhysics_Player.viewAngles/current`,
  `playerPState_t.deltaViewAngles`, `idGameLocal.playerList`, all gate fields). RTTI vtables: `idGameLocal`
  +0xF2FEB0, `idPlayer` +0xF512B0, `idPhysics_Player` +0xCEA108. No F10 press was logged: the key is only read while
  the game's process owns the foreground window, and from 17:09:25 on (and 17:07:57-17:08:35) Windows reported
  `explorer.exe` (`XamlExplorerHostIslandWindow`) in front. After clicking into the game, F10 opens and closes the
  panel (user-confirmed, DualSense). Live memory (details in the game's MODDING_PLAN): the physics object is reached
  through `idPlayer+0x310` (a separate allocation); the view angles are at physics +0x488 (copies +0x3584, +0x3590),
  but writing them (one or all three) is undone by the game within 50-100 ms; a turn diff of all writable memory found
  no other float/double/short copy of the yaw. **Source found with WriteWatch** (same day, game restarted with
  `WriteWatch=on`):
  - physics+0x48c is written ~62/s by `exe+0x4119CB`, in `exe+0x411930` = `idPhysics_Player::SetPlayerInput(cmd, oldCmd,
    viewAngles)` (copies two 0x4c-byte commands to physics+0x43c / +0x3f0 and the angles to +0x488). Its caller
    (`exe+0x935280`, on the idPlayer) takes Y = `player+0x210`'s virtual +0xD0 (the physics object is embedded in Y at
    +0xd1a8), and the angles from `exe+0x613390` = a getter returning Y+0x1072c (= physics+0x3584).
  - physics+0x3588 is written ~62/s by `exe+0x60D1A8`, in `exe+0x60D130` = `SetViewAngles(angles)` on the object at
    physics+0x343c (called from `exe+0x61965C`, function `exe+0x618E10`): `delta[i] = angles[i] - SHORT2ANGLE(cmd
    .angles[i])` at +0x13c, `viewAngles = angles` at +0x148, the command's angles as shorts at +0x60 (scale 360/65536).
  - So, as in DOOM: **view = SHORT2ANGLE(cmd.angles) + deltaViewAngles**, with deltaViewAngles at **physics+0x3578**
    (checked live: yaw -143.849 + 270 = 126.151). Writing +20 to its yaw stuck (view 126.151 -> 146.151, the
    physics+0x488 copy followed, unchanged for 2 s with the game in front), **but the screen didn't turn** (user, then
    `um win shot` before/after: same view). Both tests ran in the opening plane cockpit, a seated scene whose camera is
    likely constrained to the seat; the game also pauses (pause menu) whenever it loses focus, and the earlier tests may
    have hit that menu. Delta restored to its value. **On foot (chapter 1 plane corridor) it works**: `um win shot`
    before/after a +20 write to the delta's yaw (-38.90 -> -18.90; view 77.76 -> 97.76) shows the view turned ~20
    degrees left (game focused with `um win drive focus`, no pause menu). The delta isn't fixed at spawn: it was
    270 in the cockpit and (5.52, -38.90) here, so the game sets it per scene; seated/scripted views ignore it.
  - **idTech module, id Tech 5 path** (`idtech_engine.cpp`, generation 5 when `detect` saw the id Tech 5 files): fixed
    offsets (physics = `*(idPlayer+0x310)`, view +0x488, delta physics+0x343c+0x13c, input command +0x43c, command
    angles state+0x60); the player from the heap by its RTTI vtable with the physics object's vtable checked; no
    idGameLocal scans (4 instances, nothing tells them apart); gate as id Tech 7, with a hash of the input command as
    the per-tick signal (paused or loading when it stops changing for 60 ms; not yet verified in game); on each new
    player the offsets are checked against view yaw = SHORT2ANGLE(command yaw) + delta yaw before any write (retried
    every second, a mismatch logged once). Built, 400 checks passed, deployed to TNO. **Works in game**
    (user-confirmed: the gyro turns the camera in the right directions on foot and stops in the pause menu; DualSense,
    connection not recorded, no Steam Input: not a Steam game). Log: `id Tech 5 check, view yaw -102.507 = command
    47.763 + delta -150.270: ok`, applied yaw/pitch up to ~45 degrees per 10 s. The gate's input-command signal held for
    long stretches, with a few 0.1-0.3 s "frozen" blinks at scene changes (not noticed by the user). GAMES.md row added.
  - **Plane turret (chapter 1): no gyro, parked.** The player is bound to an `idProp_PlaneTurret` (`idPlayer+0x198`,
    `+0x1a0`). Dead ends, all at the turret: a +20 deltaViewAngles write holds in memory but the screen doesn't turn
    (screenshots); no game code reads the player's view (physics+0x48c), the player state's command angles (+0x343c+0x60)
    or the physics copy of the input command (physics+0x448) there (WriteWatch r:); turret+0xd68 (written ~4/s by
    exe+0x68D1D1) falls steadily, ammo/heat; a user-driven aim-and-return diff of 1.57 GB of writable memory found no
    aim value in the player, its physics or the turret (only transients and render buffers). Untried: 3x3 rotation
    matrices that turn with the aim, the usercmd generator (`idUsercmdGenLocal`). Automated tests that focus the game
    while nobody plays got the player killed at the turret (checkpoint reload): user-driven runs only.
  - **Turret solved (same day, later): the gun follows the input angles, not viewAngles.** The input generator's
    accumulated angles are floats in the exe at **exe+0x1c1cb40** (pitch, yaw, roll; id Tech senses), turned into the
    command's shorts every tick (stick and mouse add up there). A user-aimed read-only watch showed the gun (the
    rendered camera, `*(idPlayer+0x210)+0x47ec`, an idMat3, forward row first) turning by ~0.4x the command's change
    from the same instant and stopping at its limits, while viewAngles went 1:1 and on past them. Writes to the input
    angles at the turret: +10 yaw -> command +10, gun +4 (to its limit), view unchanged; -20 -> gun -8, view -20; -60 ->
    the scene turned ~24 degrees right on screen (um win shot before/after; the 20-degree tries were too small to see
    with the plane moving). On foot the view follows the input angles 1:1 (view = command + delta). Change (idTech, id
    Tech 5): the gyro is added to the input angles instead of deltaViewAngles; `view_angles` reports them (a gun's ratio
    or the game's pitch clamp aren't undone writes); the build check also compares them with the command. The
    stick-only mode, its INI settings, the Turret class check and a "hold the view near the camera" attempt were tried
    and reverted. Built, 400 checks passed. **Works in game** (user-confirmed on foot and at the plane turret, DualSense;
    log: `id Tech 5 check ... input angles yaw 0.000: ok`, `first gyro write to the input angles at
    WolfNewOrder_x64.exe+0x1C1CB40`, the gyro kept turning across a player change, no stick fallback).
- 2026-10-21 **Wolfenstein: The New Order, GOG build: the input angles found through idUsercmdGenLocal.** The GOG exe
  (unencrypted, Galaxy64.dll) has the same player layout as Game Pass 1.14.1.0 (idPlayer vtable exe+0x10297D8,
  idPhysics_Player exe+0xE87B58; the check's view = command + delta holds), but not the input angles at exe+0x1c1cb40.
  The first GOG test ran a build from before the value search (`find_tech5_input`, dde8131); with it, the search found
  4-8 float triples in GOG's .data equal to the command's angles every time (copies), never the single one it needs.
  In the Game Pass dump the input angles are **idUsercmdGenLocal+0x10**: the one static object with that RTTI vtable
  (exe+0x1c1cb30, vtable exe+0xc90538), the same layout as Wolfenstein II. The id Tech 5 path now finds the generator
  that way (any build or store), falling back to the Game Pass address and then the value search. Built, 661 checks
  passed. **Works in the GOG build** (user-confirmed; log: `idUsercmdGenLocal WolfNewOrder_x64.exe+0x1CD5110: input
  angles at +0x1CD5120`, `id Tech 5 check ... input angles yaw 50.960 ...: ok`, gyro writes all session).

- 2026-10-19 **Stick camera (`StickCamera=1`, core, branch `stick-camera`): the right stick turns the camera in degrees,
  past the game's capped stick speed, without mouse emulation.** Some games cap the right stick's turn rate (also at
  full deflection); Steam Input's "joystick as mouse" gets past it but the game then sees a mouse and switches its
  prompts and UI. Options weighed: mouse emulation (`SendInput` / injected Raw Input: the UI switch, rejected);
  rescaling the stick the game reads (can't pass 100 %, only reshapes the curve); raising the game's own limit (CVars,
  `BaseTurnRate`, DOOM's `joy_*`: per game); an additive boost on top of the game's stick (kept as an idea);
  **full replacement**, chosen: the game reads a centred right stick and Gyro4All writes the stick's turn with the
  gyro's (`Engine::apply`, one write per frame). Design:
  - `src/core/stick_camera.hpp/.cpp` (`StickCamera`, no game memory, unit-tested): radial deadzone, the rest rescaled
    to 0..1 and raised to `StickCurve`, direction kept (diagonals no faster than the axes); edge acceleration from
    `StickSpeed` to `StickEdgeSpeed` over `StickEdgeRamp` ms while past `StickEdge` (both axes scaled alike); frame
    time from `SDL_GetTicksNS`, capped at 0.1 s, none on the first frame after a reset.
  - Settings (`Gyro4All.ini`, profile overrides): `StickCamera=0`, `StickSpeed=360`, `StickSpeedPitch=200`,
    `StickDeadzone=10`, `StickCurve=2.0`, `StickEdge=90`, `StickEdgeSpeed=720`, `StickEdgeRamp=400`, `StickInvertY=0`.
    The `Stick*` values are read again when `Gyro4All.ini` or the profile is saved (checked each second); `StickCamera`
    itself at start (it arms the native report hooks).
  - Hiding the stick: `xinput::frame(..., suppress_right)` zeroes `sThumbRX/RY` of the controller shown as XInput, as
    for flick stick; `nativepad::frame(pad, suppress)` centres the stick bytes in the game's own PlayStation reports
    once their layout was checked against SDL. `nativepad::install` is armed by `NativeFlickStick || StickCamera`;
    `GL_HOST_NATIVE_STICK_SUPPRESSION` still counts the native path only with `NativeFlickStick=1`. Only the axes
    change: the stick click (R3) still reaches the game on both paths. Steam Input and real Xbox controllers: not
    hidden, so the stick camera stays off there (log `stick camera: unavailable: ...`).
  - Gate (`host.cpp` `frame`): gameplay view, camera allowed (no cursor), focus, no pause, no panel, no `hold_gyro`;
    off while GyroLib's flick stick uses the right stick (effective `context.<view>.flick.mode` other than Off and
    touchpad only, or `suppress_native_right_stick`): flick keeps priority. Off while the stick fallback runs (the
    gyro's turn then goes to the game's stick); the fallback is given `player_stick=false` while the stick camera turns,
    so undone stick-camera writes start it like undone gyro writes. Log: `settings: StickCamera=1 (...)`, `stick
    camera: on (the controller shown as XInput)` / `on (the game's own controller reports)` / `off while GyroLib's
    flick stick uses the right stick` / `off while the stick fallback turns the camera` / `unavailable: ...`, and
    `status: stick camera <state>` every 10 s.
  - Lost by design: the game's aim assist, aim slowdown and its own invert-Y (`StickInvertY` instead). A right-stick
    weapon wheel without a cursor would turn the camera; `StickCamera=0` in that game's profile.
  - Built (no warnings), 414 checks passed (7 new `test_stick_camera_*`).
  - **First test, Wolfenstein: The New Order** (DualSense shown as XInput, Steam Input off): `stick camera: on (the
    controller shown as XInput)`, the game read `RX 0..0 RY 0..0` while it turned, no UI change. After about two
    minutes the stick fallback started (`the game undid the gyro's camera writes 8 frames in a row ... (-5.33, 1.25)
    deg`). That was the stick camera's turn, not the gyro's: about 40 deg/s of pitch, probably against the game's
    pitch limit. The fallback then gave that lost turn back through the game's own stick, and the camera ran away
    for about 10 s (the user saw drift) until a probe write lasted. Two causes:
    1. The fallback judged and owed the stick camera's writes. Fixed: it gets only the gyro's turn, and the stick
       camera's turn counts as the player's stick, so no verdict is made while it turns.
    2. id Tech's own stick speeds up while held, but the fallback assumes a fixed speed per deflection. It learnt the
       minimum (`game speed at full stick: pitch 20, yaw 20`), pushed harder, and overshot once the game's
       acceleration ramped up. A `StickFallback=0` setting to turn it off was added and then removed (the user: the
       fallback has to follow the gyro in every game, not be turned off).
  - **Stick fallback rewritten** (`stick_fallback.cpp`). It now learns the game's response per axis: speed at 9
    deflection points (0, 1/8 .. 1), kept monotonic, learnt from steady deflections only (change under 0.05 between
    frames, since games read the stick 1-2 frames late), and the axis's sense (3 degrees turned against the deflection
    and 3x more than with it flips it; logged). Each frame it asks for the gyro's speed plus the owed turn over 0.25 s
    (at most +180 deg/s), and the curve gives the deflection. The deflection stays under 0.88, below edge
    acceleration, unless more than 3 degrees are owed in that direction. Detection, probes, the "turns nothing" give-up
    and the player's-stick handling are as before.
  - Test mode: `StickFallbackTest=1` (not in the default INI) forces the fallback on in the first minute of gameplay
    and every other minute after: no probe writes, no stop on kept writes. A notice and a log line at each switch.
  - Simulations (`tests/profile_tests.cpp`, a game that undoes the writes, a gyro sweeping ±60 deg/s yaw and ±20 pitch).
    Worst follow error, new vs old: linear 1.1 vs 3.1 deg; gamma 2.5 curve 1.9 vs 3.6; gamma 1.5 with edge
    acceleration (x5 in a second past 0.9) and 2 frames of lag 3.2 vs 6.3; inverted pitch 1.1 vs **1624 (runaway)**.
    423 checks passed.
  - **TNO with `StickFallbackTest=1`** (DualSense as XInput, Steam Input off): forced on at the right times, but the
    camera ran away again. The log was full of `sense flipped` on both axes, sometimes twice in 33 ms, and showed
    `owed (7.95, -8.00)` (at the bound). TNO's look isn't inverted. Its stick reaches the camera late and smoothed, so
    right after the deflection reverses the camera still turns the old way. The sense check took that as an inverted
    axis, and with the wrong sense the loop ran away. The learnt curve was right otherwise (pitch 39/78/90, yaw
    16/80/80 deg/s at 50/88/100%: a strong response curve). Changes:
    - The curve learns only after 6 frames of held deflection (same sign, change under 0.05), the sense check only
      after 10.
    - A flip needs 10 degrees against and 4x more than with, then 3 s with no other flip.
    - Catch-up 0.35 s (was 0.25), at most +90 deg/s (was 180); past 88% only with more than 6 degrees owed (was 3);
      owed leak 0.99 per frame (was 0.97: it forgot real lag).
    - New simulations of an id Tech-like stick (gamma 2.2, edge acceleration, 2 frames late, smoothed over 0.1 s): no
      flip; worst error about 15 degrees with a stick fast enough, and it settles with no drift when the gyro stops
      with a stick too slow. The error is almost all the game's own lag (0.13 s at 60 deg/s), which only knowing the
      gyro's future would remove.
    - Dead end: subtracting the turn still "in flight" from the owed turn (Smith-predictor style) made every case
      worse, from 0.9-1.9 to 2.3-6 degrees and 15 to 20 in the id Tech case: it held back catch-up exactly when
      needed. Removed.
    - 427 checks passed.
  - **TNO, third try** (same setup): no more `sense flipped`, and yaw learnt well (98/101 deg/s at 88/100%). The user
    reported that it doesn't follow the gyro and that the camera goes up or down to the limit by itself. The log shows
    the pitch curve falling from 86/104 to 0/21/21 deg/s, owed `(-15.27, -7.58)`, and the pitch stick held at -1.00.
    Two causes:
    1. At the pitch limit a full deflection turns nothing, and the curve learnt that as a slow stick. The owed turn
       past the limit then pushed back once the gyro turned away.
    2. A vertical axis that turns the other way can't be caught by the "held deflection" sense check: the deflection
       keeps growing while the camera runs off.

    Changes:
    - Sense check by lagged correlation: per axis, a fading sum (0.97 per frame) of the camera's turn times the
      deflection given 0..9 frames before. When the strongest lag is below -2 (deflection x degrees), the sense flips,
      with a lock of 60 frames after.
    - A limit: a clear deflection (over 0.4, curve over 10 deg/s) with the camera stopped (under 15% of the curve) for
      8 held frames. Nothing is learnt there and no turn is owed that way until the camera turns that way again. A
      camera turning the other way is left to the sense check, not taken as a limit (that hid inverted axes).
    - New simulations: a pitch limit (push into it, then turn away: the camera leaves at once and the 88% speed stays
      over 50 deg/s), and the id Tech-like stick with inverted pitch and a limit (flips, follows within about 16
      degrees, the same as with normal pitch).
    - 433 checks passed.
  - **TNO, fourth try**: in one forced interval the fallback gave up after 2.3 s with `the stick turns nothing` (the
    camera was probably at the pitch limit with the gyro pushing into it), then stayed off on that camera. Fix: the
    "turns nothing" check skips frames of mostly pitch that is blocked or at a limit (yaw has none to tell from a game
    that ignores the stick), and never runs while forced. The user first reported "perfect now", then pointed out
    that the test hadn't run: after the give-up, the rest of that forced minute was direct gyro writes (the on-screen
    notice still said forced). **The fallback is still untested in game with this fix.**
  - **TNO, fifth try** (a full forced minute ran, without giving up): `sense flipped` about once a second on both axes
    (the 60-frame lock was all that spaced them), owed at the bound (`-8.40, 7.92`, `4.23, 12.64`), and the yaw curve
    dropping to 48 deg/s. TNO's look isn't inverted, so every flip was false. The correlation check (lags 0..9 frames)
    reads a lag longer than its window, or the loop's own oscillation (response about 180 degrees behind the
    deflection), as an inverted axis. Dead end: any sense check that correlates deflection and turn inside the closed
    loop.
    - Replaced by a runaway check. A run of frames with more than 4 degrees owed, a deflection over 0.3 pushing toward
      it, and the camera moving away from it: after 30 frames, more than 8 degrees away flips the sense (60-frame lock
      after). Lag only delays the camera, oscillation averages out over the run, and a limit stops the camera without
      moving it away, so none of them flips it. Inverted look (a player option) is caught later than before (about
      0.5-1 s), and the turn lost meanwhile stays lost.
    - New simulation: 6 frames of lag and 0.2 s of smoothing (longer than the old window). No flip, no runaway.
      `follow_error` can measure from a later frame (the inverted tests, after the flip).
    - 436 checks passed. Not tested in game yet.
  - **`StickCameraPassButtons`** (user request: TNO's weapon wheel opens on R1 and is picked with the right stick).
    Comma-separated buttons (the sensitivity profiles' names, `parse_button`). Any of them held over 200 ms gives
    the right stick back to the game (the stick camera idles) until all are released. Read with the other `Stick*`
    values, so it also reloads on save. A short tap doesn't interrupt the stick camera. Built; not tested in game.
  - Seen in the same log: `applied pitch` reached about -395 degrees. TNO's input angles have no pitch limit, so
    pushing past the view's limit stores turn that must be undone before the view moves back. This affects the gyro
    too, but the stick camera makes it much more likely (to check in game; a pitch limit for the input angles would
    be on the idTech side).

- 2026-10-19 **Stick fallback rewritten: open loop with the game's stick learnt automatically** (`stick_fallback.cpp`,
  new `stick_response.cpp`). User report on the closed loop (fifth TNO try): it doesn't respond to the gyro and the
  camera drifts by itself.
  - Dead end 1, the closed loop (all five TNO tries above): it owed the turn the camera hadn't made and learnt the
    curve and the sense while controlling. Any error in the read-back or in what it learnt kept the stick pushed, also
    with the gyro still.
  - Dead end 2, a fixed open loop like Steam Input's joystick camera (speed at full stick, dead zone, curve as
    settings; built and tested in TNO the same day): no drift from the stick, but the defaults (linear, 180 deg/s)
    were far from TNO's stick (curved, about 85 deg/s), so normal gyro motion gave deflections the game hardly turned:
    "the gyro doesn't move the stick". Tuning by hand is what the user doesn't want. That build also logged one
    forced minute with the stick at (0.00, 0.26) at rest in several status lines (about 15 deg/s up by its model); the
    trace build meant to explain it was never installed (TNO was running).
  - Review of the rest of the path (no bug found): XInput adds the deflection to the right stick (RY up), DirectInput
    to Z/Rz (Rz down), the PlayStation reports with y up; the stick camera is off while the fallback runs and the
    player's right stick goes to the game unchanged; TNO (id Tech 5) `view_angles` reads the input angles the game's
    own stick turns (no pitch limit there).
  - Now: the output stays open loop (the deflection comes from the gyro's speed alone: a still gyro is a centred
    stick). The model of the game's stick is identified online from what the camera does with the deflection given:
    speed = top speed per axis (signed: inverted look) x shape, `lag` frames late; shapes on a grid (axial/radial x
    dead zone 0..0.35 step 0.025 x curve 1..3 step 0.25), lags 0..10 frames; per candidate a least-squares fit of
    the camera's turn over 8-frame windows (cumulative sums, so every lag costs the same), weighted by relative error
    (the chosen model's turn, IRLS-style) and by a steady deflection; sums fade with a 20 s half-life; the candidate
    with the smallest residual wins (3% margin to switch). Used once there are 90 windows of clear deflection and
    the model explains at least 50% of the camera's motion; an axis with too little data of its own keeps the prior's
    speed scaled like the other. Left out: the player's stick (and 12 frames after), frames with a direct write,
    jumps over 30 degrees, and an axis stopped at a limit, found by position (the camera stopped while pushed toward
    the furthest it has been that way: a dead zone stops it anywhere, a limit only there). Before learning: the model
    saved for this game (`StickFallbackLearned=` in the profile, written when the learnt model changes, at most every
    10 s and when the fallback stops), else radial dead zone 0.2, curve 1.5, 160/110 deg/s. Turns under the smallest
    deflection past the dead zone (+0.04) go out as pulses; at most 2 degrees of the gyro's turn wait to be given.
  - Probe verdict (do the game's writes last again?): the stick's own turn in that frame is now taken as last frame's,
    and the stick keeps carrying the gyro on probe frames, so no lag or model is needed (with the learnt lag the old
    prediction judged a kept write as undone).
  - Removed: the `StickFallbackYawSpeed/PitchSpeed/Deadzone/Curve/InvertPitch` settings of dead end 2 (never released).
  - `MotionTrace=1` with `StickFallbackTest=1` starts the trace with the fallback, and adds `K,x,y,yaw,pitch,used`
    lines (deflection, camera turn, axes the learner took).
  - Simulations (`tests/profile_tests.cpp`), camera/gyro speed after learning at 8/20/45 deg/s yaw and 8/25 pitch:
    linear axial dead zone 0.12: 0.99-1.04; UE-like axial dead zone 0.25 at 220/150 deg/s: 1.00; radial, dead zone
    0.2, curve 2.2, 2 frames late and smoothed before the curve (the hardest): 1.05-1.11; with camera noise and a
    recoil kick every second: 0.97-1.04; id Tech-like edge acceleration: 0.96-1.03; 144 frames/s with uneven frame
    times: 0.98-1.07; inverted pitch learnt as such; a pitch limit doesn't lower the learnt pitch speed. A still gyro
    centres the stick and the camera stops in every case. 503 checks pass. About 9 us per frame while it runs.
  - **Not tested in game yet.**

- 2026-10-19 **TNO test of the learner: the game didn't take the fallback's stick changes** (DualSense as XInput,
  Steam Input off, `StickFallbackTest=1`, `MotionTrace=1`). User report: the camera drifts and runs fully up or down
  out of control, they had to correct it with the stick. The trace's `K` lines showed the cause, not in the model:
  stretches with the camera turning a steady ~120 deg/s in pitch (2.04 degrees per frame, TNO's full-stick speed)
  while our stick was 0, and stretches with our stick at +-0.7 yaw / +-1 pitch and the camera still. Our XInput
  slot's `dwPacketNumber` only changed with the physical controller's state, and id Tech 5 skips a state whose
  packet number didn't change: it kept the last deflection it had taken. The earlier closed-loop tests in TNO had
  the same hidden problem (the stick "sometimes" worked: whenever the real pad's state changed).
  - Fix (`xinput.cpp`): a changed fallback deflection bumps the packet number, like the long-press masks already did
    (`g_mask_changes`, now added to our slot's packets too, not only to real XInput ones).
  - The learner had learnt from that data and saved `radial,0.350,1.00,7.0,-5.0,0` (7 deg/s, inverted pitch) in a
    session before, then `axial,0.000,1.00,177.6,121.6,10` (explaining 13%). Starting the next session with the
    first model drove the runaway: a wrong sense makes the player tilt against the camera, which pushes the stick
    further (the human closes the loop). Guards: a top speed under 20 deg/s is not a stick (model rejected, not
    parsed from the profile); a model is saved only when it explains at least 70% of the camera's motion from 300
    windows of clear deflection. Test: a 10 deg/s "game" is never learnt nor saved. 505 checks pass.
- 2026-10-19 **Wolfenstein II: The New Colossus (id Tech 6, MachineGames; PC Game Pass / Xbox app build 1.5.0.0):
  recon and first load** (plan in `games/WolfensteinII/MODDING_PLAN.md`). `NewColossus_x64vk.exe` (Vulkan only) can't
  be read from disk (Xbox app encryption, as TNO); no anti-cheat; no community loader (only the New Colossus Tweaker,
  a `dinput8.dll` proxy). The Switch port has native gyro, PC has none. A dev build of 1.4.0 as `winmm.dll` in
  `Content` (installed by the user, run once, from the main menu): `engine: idTech (detected)` (by
  `gameresources.resources`), thread 5276 (`PeekMessageW` from `exe+0x8B3D36`) adopted as the game thread, GyroLib
  and the Vulkan panel renderer start, the DualSense is shown as XInput controller 0 and polled (no Steam Input: not
  a Steam game). **No scene**: the type info parses with the id Tech 6/7 layout (5895 classes) but none of DOOM's
  names exist (`idGameLocal.playerList`, `idPlayer.physicsObj`, `idPhysics_Player.viewAngles/current`,
  `playerPState_t.deltaViewAngles`, every gate field). RTTI vtables: `idGameLocal` exe+0x2A227B8, `idPlayer`
  exe+0x29F7858; no `idPhysics_Player` in the heap at that moment. Next: dump the decrypted image in play and run
  `tools/idTech/typeinfo.py` on it to see MachineGames' names. Saves backed up (`um backup`:
  `wolf2-saves-pre-gyro4all`, `wolf2-wgs-pre-gyro4all`).
  - Type info from the running image (same day; read-only dump of the mapped exe with ImageBase/section offsets
    rewritten to the image layout, in the scratchpad; `typeinfo.py` on it): 5895 classes, id Tech 6/7 layout, but **as
    in TNO only helper structs and templates carry fields**. `idPlayer`, `idGameLocal` and the player physics have no
    class entry at all; nested names exist (`idPlayer::playerExplicitMotion_t` 0x70, `idGameLocal::*`,
    `idHavokPhysics_Player::crusher_t/manifold_t/waterInfo_t`). Sizes without fields: `playerPState_t` 0x70,
    `usercmd_t` 0x40, `idUserCmdMgr` 0x300 (12 x `idUserCmdInfo` 0x40). No field named `viewAngles`/`deltaViewAngles`.
  - RTTI names in the image: `idPlayer`, `idGameLocal`, **`idHavokPhysics_Player`** (no `idPhysics_Player`: the
    player's physics is Havok, as in id Tech 7, so the module's generation-6 choice `idPhysics_Player` is wrong here),
    `idUsercmdGen`, `idUsercmdGenLocal`.
  - No gyro or motion-aim strings (the Switch port's gyro isn't in the PC exe). Input cvars: `in_joystick`,
    `in_invertLook`, `in_anglespeedkey`, `in_MarkJoystickInactiveOnMouseInput`, `in_joystickGUIDeadzone`.
  - So the route is TNO's: fixed offsets found live (RTTI player in the heap, its Havok physics, turn diffs, WriteWatch
    on the view to find `SetViewAngles` / the delta or the input generator's accumulated angles).
  - Live, read-only (2026-10-20, on foot in a campaign save; DualSense as XInput, no Steam Input): vtables
    `idPlayer` exe+0x29F7858, **`idHavokPhysics_Player` exe+0x2704268**, `idGameLocal` exe+0x2A227B8,
    `idUsercmdGenLocal` exe+0x27837F8 (its one instance is static, at **exe+0x3DB7980**). One live `idPlayer` in the
    heap (two more vtable hits are stale) and one `idHavokPhysics_Player`, **embedded in the player at +0xeb68**
    (as DOOM's `physicsObj`). Turn diffs of player+0..0x40000 (snapshots still / after a stick turn / still): the
    first try caught nothing (the game had lost focus to the terminal and pauses then). Second try (right stick to
    ~70% briefly): **player+0xd6ec went -21.43 -> -33.93** (12.5 degrees right; id Tech's yaw falls turning right),
    neighbours +0xd6e8 1.77 and +0xd6f0 0.98 (pitch/roll? not checked): the best yaw candidate, outside the physics
    object. Five floats went 36.6 -> 43.4 together (player+0x10120, +0x10154, +0x10d5c, +0x22ff0, +0x23024) next to
    position-like values: probably not angles. The input generator's 0x4000 bytes held no turned float. Not confirmed
    yet: the game closed (by the user or a crash, no crash report) before a longer right/left/up/down recording.
    Heap addresses change per run; the offsets above are relative to the player.
  - **idTech module, MachineGames id Tech 6 path** (`idtech_engine.cpp`, `Layout::mg6`, same day): detected when the
    type info (generation 6) has neither `idPlayer` nor `idPhysics_Player` and the RTTI has `idHavokPhysics_Player`.
    Fixed offsets: physics (idHavokPhysics_Player) embedded at idPlayer+0xeb68 (the heap player is the one whose
    +0xeb68 holds that vtable), view angles idAngles at **idPlayer+0xd6e8** (the yaw candidate +0xd6ec). The gyro is
    added there directly (pitch clamped to +-89, yaw wrapped); `view_angles` reads the same. No gameLocal, no gate
    fields (only the core's focus gate: the game pauses itself without focus); pause menu and cutscenes not gated
    yet. New WriteWatch name `player` (the local idPlayer): `WriteWatch=player+d6ec` traces the yaw's writers.
    Built, 505 checks pass, deployed to Wolfenstein II. **Not tested in game.** If the game rebuilds the view from
    the input command each tick (as TNO's copies), the writes will be undone: then WriteWatch for the source.
  - **In game (same day): the player is found** (`heap scan: 4 idPlayer vtables, 1 with a physicsObj`, `gyro on`), but
    **every write to +0xd6e8 was undone within 8 frames** (`the game undid the gyro's camera writes 8 frames in a
    row`); the core's stick fallback took over (DualSense as XInput, not a Steam game; `StickFallbackTest=1` was still
    on in that run). **WriteWatch=player+d6ec** (10 s, user turning): 72 writes by `exe+0x126B004`, in `exe+0x126AEA0`
    = a 4x4 float matrix multiply `out(r8) = A(rcx) x B(rdx)`, out = player+0xd6d0 (0x40 bytes). Its caller
    `exe+0x1008EF0` (this = player+0xd6c0) builds B with `exe+0x126A760` from a position (vec3 at +0x68) and a
    rotation (at +0x74) of the object in rdx, A with `exe+0x8003E0`; called from `exe+0x1005E00`. So **+0xd6ec is one
    element of a world transform of something attached to the player (bone/attachment), not the view's yaw**: a dead
    end (it changed with the turn because the transform turns with the body). The diff's |change| > 5 filter hid the
    rotation entries in [-1, 1]. Next: a 4 Hz recording through right/left/up/down with a wider analysis (floats in
    degrees and radians, matrix rows, shorts, the physics object and the input generator), then WriteWatch on the real
    angles. The `mg6` path keeps the player/physics detection; its view offset is wrong until then.
  - **Source found (same day): the input generator's angles, as in TNO.** A 15 s, 4 Hz recording of player+0x60000
    and the input generator (exe+0x3DB7980, 0x4000) during stick turns: `idUsercmdGenLocal`+0x10 = **pitch**,
    +0x14 = **yaw** (accumulated, unwrapped: 1075.8 -> 519.3), +0x18 roll 0 (+0x1c / +0x20 look like per-tick stick
    rates). The view is in the Havok physics at **physics+0xff8** (player+0xfb60: pitch, yaw, roll 0) and equals the
    input angles plus a constant delta over the whole recording (yaw -175.28, pitch +80.05). Other movers were not
    the view: player+0x5788/0x5790 (stepwise, a body/legs yaw), player+0x22f68/0x23088 (unwrapped, another follower).
    **Live write test** (WriteProcessMemory, game in front, pad still): input yaw +30 -> view yaw -42.434 -> -12.436
    and held; input pitch +15 -> view pitch 6.396 -> 21.393; the user saw the view turn ~30 degrees left, then
    look down (DualSense as XInput, not a Steam game).
  - Change (`mg6`): the gyro is added to the input angles at **exe+0x3DB7990** (TNO's code path, `input_angles`;
    pitch down and yaw left positive), checked at start: the qword at exe+0x3DB7980 must be idUsercmdGenLocal's
    vtable, else no scene. `view_angles` reads the input angles. `player_view` (physics+0xff8) is kept for logs only.
    Built, 505 checks pass, deployed; the temporary `Gyro4All.ini` (WriteWatch) removed. Gate: still only focus.
    Not tested in game with the gyro yet.
  - **In game (same day, user on foot; DualSense as XInput, not a Steam game): native gyro.** Log: `MachineGames id
    Tech 6 offsets ... input angles exe+0x3db7990`, `player heap ...`, `gyro on`, `first gyro write to the input
    angles at NewColossus_x64vk.exe+0x3DB7990`; **no stick fallback line in the whole run** (`gyro stick up to 0.00`
    in every XInput line), applied yaw up to a full turn (-366 degrees) in 10 s. A player change (load at 00:37:35)
    was picked up again 2 s later. The user's right stick still turned the view alongside. The user also changed
    GyroLib settings in the F10 panel (`gyrolib.ini changed, reloaded`). Pause menu, cutscenes and wheels: no gate
    yet (only focus), not checked.
  - **Tools** (`tools/idTech/`, README section 2 has the route step by step; research only, never shipped):
    `dumpimage.py` (read-only dump of the running, decrypted exe with ImageBase/section offsets rewritten, for
    `typeinfo.py` and `disasm.py`), `livemem.py` (RTTI vtables via the complete object locator, heap instances,
    static instances in the exe, pointers, `turn` snapshots; the shared reader for the others), `turndiff.py` (floats
    changed by a turn that then held; `--unit` for matrix/quaternion entries), `record.py` (spans at 4 Hz, waits for a
    live player checked by an embedded object's vtable), `analyse.py` (numpy: offsets grouped by identical change
    patterns in degrees, radians/unit and shorts), `series.py` (chosen floats per frame), `disasm.py` (capstone: the
    whole function around an exe offset from `.pdata`, e.g. a WriteWatch writer), `poke.py` (the live write test:
    adds degrees to a candidate and shows whether it holds; the only one that writes memory). Checked on this
    session's dump and recordings (`turndiff`, `series`, `analyse`, `disasm` reproduce the results above); the live
    ones were run here as scratchpad versions of the same code before being generalized.
- 2026-10-20 **Build: Gyro4All 1.5.0** (version chosen by the user). `build/release/Gyro4All-v1.5.0.zip` (the DLL logs
  `Gyro4All 1.5.0 in ...`; 505 checks pass). Since 1.4.0: the stick camera and the reworked stick fallback (open loop,
  the game's stick learnt automatically, the XInput packet-number fix), the idTech module's MachineGames id Tech 6
  path (Wolfenstein II, native gyro through the input angles, not gated in the pause menu yet), the `player`
  WriteWatch name, and the `Gyro4All.ini` template's spacing (`settings.cpp`). Not deployed to a game as 1.5.0 yet;
  no GAMES.md row for Wolfenstein II yet (pending the pause gate).
- 2026-10-20 **Indiana Jones and the Great Circle (MachineGames' id Tech 7 fork; PC Game Pass / Xbox app build
  1.16.1.0, package `BethesdaSoftworks.ProjectRelic`, build `20260527-113405-151882`, changelist 647310): recon**
  (plan in `games/IndianaJonesGreatCircle/MODDING_PLAN.md`). `TheGreatCircle.exe` (Vulkan through Streamline's
  `sl.interposer.dll`) can't be read from disk (Xbox app encryption); no anti-cheat, no Denuvo in retail; community
  route only Lyall's GreatCircleFix (console unlock, intros, ultrawide). Saves backed up (`um backup`:
  `indy-wgs-pre-gyro4all`, `indy-config-pre-gyro4all`).
  - First run of 1.5.0 as `winmm.dll` (installed by the user): loads, game thread adopted (`PeekMessageW` from
    `exe+0xE6DD39`), GyroLib, Vulkan swapchain captured, XInput hooked; the game also reads the DualSense itself over
    HID (64-byte reports). **`engine: none detected`**: the resources are `base\gameresources_pc.resources` (+
    `_patch1/2`), `meta_pc.resources`, `*.streamdb`; no `base\gameresources.resources` (the idTech marker). The Unreal
    module ran and found nothing.
  - Type info from a read-only dump of the running image: **id Tech 6/7 layout, 8951 classes**, but the game code is
    an entity-component system: **no `idPlayer`, `idGameLocal`, `idHavokPhysics_Player`, `idPhysics_Player`,
    `playerPState_t` or `usercmd_t`, neither in the type info nor in the RTTI** (so the `mg6` test can't match either).
    RTTI has **`idUsercmdGenLocal`** / `idUsercmdGen` (Wolfenstein II's view source) and player jobs
    (`idPlayerActorController*Job`, `idPlayerInputSensitivityJob`, `idPlayerControlJob`).
    Live, read-only (`livemem.py find`): `idUsercmdGenLocal` vtable exe+0x35FF948, **one static instance at
    exe+0x64A9A20** (none in the heap), as in Wolfenstein II (exe+0x3DB7980 there).
  - Components with fields: **`idUserViewAnglesComponent`** (0x50; `minViewAngles` 0x0, `maxViewAngles` 0xc,
    `forcedViewAngles` 0x28, `forcedViewAnglesMod` 0x34, **`viewAngles` 0x40**), `idActorControllerComponent`
    (`lookAngles` 0x450), `idLocalPlayerComponent` (singleton 0x90: `playerEntity` 0x10, `playerAvatar` 0x18,
    `viewUpdatedAt` 0x40 game time), `idPlayingFPSCameraComponent` (`active` 0x5c, `isMainView` 0x5d,
    `forcedViewAngles` 0x60), `idRigEntityComponent` (`viewAnglesDelta` 0xab4). Instances have no vtable (pools).
  - **The PC exe keeps the consoles' motion aim**: `g_setting_motion_aim` ("0: no motion controls, 1: motion controls
    enabled, 2: motion controls while aiming"), `joy_sixAxisSensitivity` (Min/Max), `joy_sixAxisAimMultiplier`,
    `joy_sixAxisInvertX/Y`, `joy_sixAxisShowRate`, the Switch 2 Joy-Con mouse `joy_mouseSensor*`, profile settings
    `lookInvertSixAxis` / `lookInvertHorizontalSixAxis`. No "gyro" string. Untested: whether the PC input path feeds
    motion from the DualSense (the options menu shows no motion setting).
  - Next: try `g_setting_motion_aim 1` with the console unlocked; detection on `gameresources_pc.resources`; a new
    layout (`mg7`) starting with Wolfenstein II's input-generator route (find `idUsercmdGenLocal`, record a turn,
    poke), else `idUserViewAnglesComponent.viewAngles` found by shape.
  - **Detection** (`idtech_engine.cpp`, same day): `base\gameresources_pc.resources` is a third idTech marker. Built,
    505 checks pass; not deployed yet (the game was running). The scan still finds no player layout for this exe.
  - **View source found, as in Wolfenstein II** (same day, DualSense as XInput, not a Steam game): 4 Hz recordings of
    the input generator (exe+0x64A9A20, 0x4000) through right/left/up/down stick turns. **`idUsercmdGenLocal`+0x9a8 =
    pitch, +0x9ac = yaw** (accumulated, unwrapped, held between turns; right: yaw -18.8 -> -116.8; left: up to 82.2;
    up: pitch 24.6 -> -53.7; down: up to 72.7). +0x9b4 / +0x9b8 are per-frame stick rates (pitch +-128, yaw +-289),
    zero while the stick rests. The first recordings (the game loading, then a menu) caught nothing: the angles at
    the Wolfenstein II offsets (+0x10/+0x14) are not angles here, and some recording frames hold stale values for ~1 s.
    **Live write test** (`poke.py exe+64aa3c8 1:30 0:-15`, game in front, pad still, twice): yaw +30 and pitch -15
    held 2 s each (49.44 -> 79.44, 31.97 -> 16.97); **user-confirmed: the view turned ~30 degrees left, then looked
    up, and stayed**. A copy of the view near exe+0x64D03A8 (about -184 yaw) moved by under 0.2 during the turns: not
    the view. Next: an `mg7` path in the idTech module (input angles at idUsercmdGenLocal+0x9a8, the object found by
    its vtable in .data, not by a fixed exe offset), then the gate (menus, cutscenes, third person).
  - **idTech module, MachineGames id Tech 7 path** (`Layout::mg7`, same day): chosen when the type info parses with
    the id Tech 6/7 layout and has no idPlayer, the RTTI has neither `idPlayer` nor `idHavokPhysics_Player` (so not
    `mg6`), and has `idUsercmdGenLocal`. The generator is the single qword equal to its vtable in `.data` (two or more:
    no scene); the gyro is added to its float angles at +0x9a8 (pitch down, yaw left positive, as `mg6`), and
    `view_angles` reads them. No player objects: the generator stands in for the player (`find_player` checks its
    vtable each frame; log `input generator ... found`, target `idUsercmdGenLocal`); WriteWatch's `player` is the
    generator. Gate: only the core's focus gate (menus, cutscenes and third person still to find). Built, 505 checks
    pass. **Not tested in game yet**; log lines to look for: `MachineGames id Tech 7: idUsercmdGenLocal at ...`,
    `first gyro write to the input angles at TheGreatCircle.exe+0x64AA3C8`.
  - **In game (same day, user on foot; DualSense as XInput, no Steam Input): native gyro works**, user-confirmed
    ("perfecto"). Log: `MachineGames id Tech 7: idUsercmdGenLocal at TheGreatCircle.exe+0x64A9A20`, `input generator
    ... found`, `gyro on (target idUsercmdGenLocal)`, `first gyro write to the input angles at ...+0x64AA3C8`.
  - **Cutscenes: the gyro moves the camera, and so does the right stick** (user-confirmed: the game lets the player
    look around a little in its cutscenes, through the same input angles). So the gyro there is the game's own
    look-around, like the stick, and no cutscene gate is needed for now. Dead ends while looking for one: input
    generator +0x1570 differed between cutscene (1) and play (3) but is a counter (3 -> 4 -> 5 -> 6, one step per
    sequence); a `.data` diff (two cutscene vs two gameplay snapshots) left 166k stable differing dwords; the
    singleton `idCutscenePlaybackComponent` (`isPlayingCutscene` +0xac, `scene` +0x0 an `idDeclMidnightScene*`,
    vtable exe+0x362D7B8, 33 decls) was not found by scanning the heap for qwords pointing at scene decls: no
    reference had +0xac = 1 during a cutscene (the references that appeared were logic-graph variables). Other
    singletons for a later gate: `idPauseGameComponent` (`requestPauseCount` +0x10, `pausedBecauseDialog` +0x17),
    `idUIMidnightInfoComponent.isRunning`, `idLocalPlayerComponent`; how singleton components are stored is unknown.
  - **Pause menu, journal and map: fine as is** (user-checked, same day): no gate needed beyond focus. GAMES.md row
    added (Game Pass build).
- 2026-10-20 **idTech: no store-specific exe addresses** (all three MachineGames/id Tech 5 games were found in Game
  Pass builds; Steam, GOG, Epic or other updates of the same game link differently). Detection itself (marker files
  next to the exe, type info, RTTI) was already store-independent; the exe offsets were not:
  - **Wolfenstein II (`mg6`)**: the input generator was read at exe+0x3DB7980 (Game Pass 1.5.0.0). Now found as
    Indiana Jones' is: the single qword in `.data` equal to `idUsercmdGenLocal`'s RTTI vtable, angles at +0x10. The
    Havok physics offset in idPlayer (+0xeb68) is tried first and otherwise searched for in the player's first
    0x20000 bytes (log `idHavokPhysics_Player at idPlayer+0x... (not +0xeb68)`); the gyro no longer needs it to write.
  - **TNO (id Tech 5)**: the input angles exe+0x1C1CB40 (Game Pass 1.14.1.0) are still tried first. When the check
    finds the player's structure right (view = command + delta) but the input angles wrong, the scan thread searches
    `.data` for the single float triple equal to the command's angles (SHORT2ANGLE, yaw modulo 360, within 0.01;
    only with a command yaw away from 0) and the check runs again on it. Log: `input angles not there (another build
    or store?): searching .data`, `input angles search ...: N matches`. The player offsets (idPlayer+0x310, physics
    +0x488/+0x43c/+0x343c, state +0x13c/+0x60) are still the Game Pass build's: in a build where they differ, the
    check says MISMATCH and the gyro stays off (no wrong writes). An id Tech 5 game without `idUsercmdGenLocal` RTTI
    (unknown for TNO) can't use the vtable route.
  - **Indiana Jones (`mg7`)**: already by vtable; the generator lookup is now shared (`single_static`).
  - **Sections**: `Image::load` took `.text`/`.rdata`/`.data` by name only; a DRM wrapper with renamed sections
    (Denuvo's random names, as in Dead Space) left no scene. A section missing by name is now the largest one of its
    kind (executable / writable / other initialized data). SteamStub's extra `.bind` section didn't matter.
  - Built, 505 checks pass. **Not tested in game** (no non-Game Pass copy of TNO, Wolfenstein II or Indiana Jones
    here); the Game Pass builds should log the same addresses as before.
  - **Other engines reviewed for the same problem** (same day): no exe addresses in code. RE Engine picks its camera
    model from the TDB and finds singletons at run time; Dead Space (Frostbite) finds its input update through the
    delegate-invoker pattern and checks the code bytes of what it hooks (the Steam addresses are only in comments);
    Unreal finds GUObjectArray/FNamePool by shape; UE, RE and Frostbite classify sections by their flags, not names.
    Detection markers (`re_chunk_000.pak`, `Data\layout.toc` + `Data\initfs_Win32`, `-Win64-`/`-WinGDK-Shipping.exe`
    or the project folders) are the same in every store.
  - **Found: profile names were store-specific.** Profiles are `profiles\<exe stem>.ini`, and all shipped UE profiles
    are `<Project>-Win64-Shipping.ini` (Steam/Epic/GOG); a Microsoft Store / Game Pass build runs
    `<Project>-WinGDK-Shipping.exe` and never found them. New `Engine::profile_aliases` (Unreal: -Win64- <-> -WinGDK-)
    and `settings::use_profile_alias` (DllMain, after the engine is picked): when the exe's own profile doesn't exist,
    the alias's is used (log `settings: no profiles\<stem>.ini; using <alias>.ini`). The probe still writes new
    profiles under the exe's own stem. Games renamed per store (Epic's `IndianaEpicGameStore-Win64-Shipping`) still need
    their own profile. Built, 505 checks pass; not tested in a game.
- 2026-10-19 **TNO with the packet-number fix: works, user-verified "acceptable"** (DualSense as XInput, Steam Input
  off, `StickFallbackTest=1`, from the default model with the bad saved one removed). Learnt within 3 s (explains
  99% at first); over two forced minutes it settled on `axial, dead zone 0.00, curve 2.75-3.00, yaw ~160-255, pitch
  ~95-110 deg/s, 0 frames late`, explaining 77-95% of the camera's motion, saved to the profile every 10 s. The yaw
  speed kept creeping up (160 -> 255) while pitch stayed ~105: probably id Tech's acceleration while the stick is held
  (the fit's 77-80% in the second minute). No drift or runaway reported.
- 2026-10-21 **Battlefield 4: first loading test** (EA app build 179547, game version 1.8.2.48475, all expansions;
  `winmm.dll`, Gyro4All 1.5.0 from the working tree; DualSense, connection and Steam Input state not recorded; menus
  only, the campaign not entered yet). Plan in `games/Battlefield4/MODDING_PLAN.md`.
  - The game: Frostbite 3, DX11 (or Mantle), `bf4.exe` x64 encrypted on disk (EA/Origin OOA: entropy 8.00 in `.text`,
    `.data`, `ctr`, `typeinfo`, `fieldinf`; `.ooa` section; only static import `Core/Activation64.dll`; delay imports
    `mantle64.dll` / `mantleaxl64.dll`), no Denuvo. **PunkBuster** installed (`pb\`, `pbsvc.exe`; `PnkBstrA.exe`
    starts with the game), no EA Javelin files: campaign only, and the proxy must be removed before multiplayer.
  - **Loads as `winmm.dll` with RivaTuner running** (unlike Dead Space): `engine: Frostbite (detected)` (layout.toc +
    initfs_Win32), all inline hooks `MH_OK`, `xinput1_4.dll!XInputGetState` hooked but `xinput: not available for this
    game (protected exe, no import-table hooks)`, so the XInput translation and the stick fallback are off here.
  - Input: the game opens the DualSense over HID (054C:0CE6) through dinput8 and reads it with DirectInput8
    `GetDeviceState` (DIJOYSTATE2); the first read logged comes from **`IGO64.dll+0x42E6A`** (EA app in-game overlay).
  - **Freeze with GyroLib's panel**: with the D3D11 panel on (`gyro4all.dll`, D3D11 swapchain 3840x2160, panel and
    renderer ready), the game stopped responding a few seconds after start (window not responding, ~8 cores busy):
    a thread sampler showed 8 threads spinning in `IGO64.dll` (+0x64054 in five, +0x1F5E4 in the Present thread whose
    stack runs through `gyro4all.dll`, +0x18EB2), and Gyro4All's frame never ran again after the first one (no status
    line). The EA overlay and our Present/panel hooks lock each other. **With the panel off in `gyrolib.ini`
    (`ui.menu_key=` and `ui.gamepad_menu_shortcut=0`, so no graphics hooks) the game runs normally**: frames on the
    8 ms timer (no Present wake without the graphics hooks), status every 10 s, motion ~450 samples/s. Untested: the
    panel with the EA app's in-game overlay turned off.
  - Reflection: `fb: 1 "DataContainer" names, 1 info data, 1 references, no linked TypeInfo`: the reader only knows
    the 2021+ layout (Dead Space / BF2042). BF4 needs the 2013 one (Frosty's BF4 type classes as reference). Its
    TypeInfo objects should be in the exe's `typeinfo` / `fieldinf` sections.
- 2026-10-21 **Frostbite 2013 type layout (Battlefield 4)** (`fb_types.cpp`/`.hpp`; **verified in game**, EA app build
  179547: `fb: DataContainer TypeInfo at bf4.exe+0x2BFDFA0, info data at +0x0, links at +0x8, info data layout 2013`,
  `10210 types (3765 from DataContainer on, 6445 newer)` in ~50 ms; `TypeDump=*` dump checked):
  - Worked out from the running game (read-only external scripts): **TypeInfo** +0 info data, +8 next TypeInfo (one
    single-linked list), +0x10 u16 runtime id + u16 flags; classes then +0x18 parent, +0x20 default instance, +0x28
    class id, +0x30 first derived, +0x38 next sibling. **Info data** +0 name, +8 u16 flags (**kind = flags >> 4 &
    0x1F**, same kind numbers as 2021; low bit always set), +0xA u16 size, +0x10 module record (name first, e.g.
    "Core", "Soldier", "Venice"), +0x18 u16 alignment, +0x1A u16 field count; classes +0x20 parent, +0x28 create
    function, +0x30 fields; structs +0x20 default value, +0x28 fields; enums +0x20 values; arrays +0x20 element.
    **Fields** 0x18 bytes: name, u16 flags (0 in this build), u16 offset, u32 pad, type (enums: the value).
  - Why the old reader failed: array types have an **empty name** (a pointer to a shared "\0"), so the walk from
    DataContainer stopped after 3 types. And the list's head (`WebBrowserSettings` exe+0x2C92D20, newest type) is
    newer than DataContainer: only 3765 of the 10210 types follow it.
  - Reader: names may be empty when walking; the layout is picked on DataContainer, 2013 first (info data at +0, a
    +8 link, and its shape: low flag bit, zero padding, plausible module pointer, power-of-two alignment, so the 2021
    layout's hash/guid/namespace can't pass), then 2021. With 2013 the newer types are found backwards in one pass:
    every image qword pointing into the image, sorted by value; a TypeInfo whose +8 is a known one is added, repeated
    on each new one. Arrays are named `<element>-Array` and the element's `array_type` set.
  - Only data classes (`...Data`, settings) carry fields; runtime classes (`Soldier::ClientSoldierEntity : 
    ClientCharacterEntity`, size 0x750) have size and parent only (alignment 0). `ClientPlayer` and
    `ClientGameContext` aren't reflected. Camera-related types present: `SoldierAimingSimulationData`,
    `SoldierAimAssistData`, `AimingComponentData`, `ClientAimingConstraintsEntity`.
  - Built, 505 checks pass (none cover fb_types). **Dead Space not retested** with the changed walk (empty names
    accepted, 2021 check now `is_type(parent)` instead of a table lookup).
- 2026-10-21 **Battlefield 4: XInput translation on a protected exe, the EA overlay freeze, and why the game never
  polls our pad** (EA app build 179547; DualSense, connection and Steam Input state not recorded; game launched by the
  user):
  - **XInput on protected exes** (`xinput.cpp`): translation was off for every inline-hook (protected) exe. Now the
    exports of `xinput9_1_0.dll` (loaded by Gyro4All first) and `xinput1_3.dll` (if loaded) are hooked inline;
    `xinput1_4.dll` is left alone because GyroLib's SDL reads real controllers through it. BF4 imports
    `XInputGetState` / `XInputGetCapabilities` from `XINPUT9_1_0.dll` (import slots exe+0x1B413E0 / +0x1B413D8, thunks
    exe+0x1184A1E / +0x1184A2A); `xinput9_1_0` calls `xinput1_4`'s exports itself. `XInputEnable` isn't exported by
    xinput9_1_0. Log: `xinput: protected exe: XInputGetState hooked inline (...)`.
  - **`WM_DEVICECHANGE` posted to 0 windows** on Frostbite: `announce_device_change` enumerated the current thread's
    windows, and the Frostbite frame runs on Gyro4All's own thread. Now: this thread's windows, else every visible
    top-level window of the process (`... posted to 1 game window(s) of the process`).
  - `profiles/bf4.ini` (new): `XInput=2`. BF4 reads non-Xbox controllers as generic DirectInput joysticks; with
    XInput=2 the DualSense's HID device is refused to dinput8 (`... access to HID device 054C:0CE6 refused`).
  - **Freeze = the EA app's in-game overlay** (`IGO64.dll`): both freezes (GyroLib's D3D11 panel, and the XInput
    watcher polling real XInput once a second from its own thread) showed ~10 threads spinning on one lock at
    `IGO64.dll+0x64054` (Present thread at +0x1F5E4, IGO's own thread at +0x18EB2), including Gyro4All's frame thread
    (through SDL's message pump) and the watcher (through `xinput1_4`). **With the overlay turned off in the EA app the
    game runs** (IGO64 not loaded, status every 10 s). Panel not retried with the overlay off yet.
  - **Why the game never polls the translated pad**: BF4 creates its XInput devices from **DirectInput enumeration**:
    the enum callback exe+0x946690 calls exe+0x94A090(&instance.guidProduct), Microsoft's WMI `IsXInputDevice`
    sample (`WbemLocator`, `\.\root\cimv2`, `Win32_PNPEntity`, "IG_" + VID/PID vs guidProduct.Data1). True: an
    XInput device object (constructor exe+0x943BD0, vtable exe+0x1D413E0, slot = count of XInput devices; is-connected
    exe+0x94B960, update exe+0x94C2B0: `XInputGetCapabilities(slot, XINPUT_FLAG_GAMEPAD)` while `[this+0x14]` is 0,
    then `XInputGetState`); false: a DirectInput joystick (exe+0x943B50). A virtual pad that DirectInput doesn't list
    never gets a device object: the hooks are in place but the game never calls them (no `the game polls` line).
    Possible fixes: make exe+0x94A090 answer true for the selected controller's VID/PID (a hook in game code, with
    XInput=1 so dinput8 still lists the DualSense; it then becomes XInput slot 0 instead of a joystick), or fake an
    XInput device in DirectInput's enumeration and in WMI.
- 2026-10-21 **Battlefield 4: DualSense as an Xbox controller, user-verified** (`fb_xinput.cpp`, `dinputpad::on_create`,
  `xinput::listed_as_xinput`; EA app build 179547 launched by the user, EA in-game overlay off, `profiles/bf4.ini`
  `XInput=1`; DualSense, connection and Steam Input state not recorded): **moving, looking and buttons work in the
  menus and in game**. Prompts: Xbox ones in the main menu (LB/RB on the tabs), keyboard ones in game even using only the pad (the game's own PC behaviour: a player with a real Xbox 360 pad
  reported the same on a forum; not checked with a real Xbox pad here).
  - Hook: from the game's first `DirectInput8Create` (code unpacked, devices not listed yet; retried while the
    exe's XInputGetCapabilities import slot isn't filled in), the import slot -> its `jmp` thunk -> the 3 functions
    calling it (unwind table, chained entries followed) -> their direct callers -> in the enumeration callback,
    `call <check>` followed within 16 bytes by `test al,al; je` (BF4 has `lea rdx,[..]` in between) before the
    device constructor's call: `IsXInputDevice at bf4.exe+0x94A090 hooked` in ~150 ms. It answers true for Sony and
    Nintendo controllers (DirectInput product GUID Data1 = MAKELONG(VID, PID)).
  - The device constructor asks `XInputGetCapabilities` once and the update never retries it (the device object,
    vtable exe+0x1D413E0, stayed slot 0 / not connected when the pad was shown 5 ms later). `listed_as_xinput` now
    waits up to 1 s for the frame thread to show the controller, and a listed controller skips auto mode's native
    signs (dinput8's HID open during the check) and the 2 s grace. Log sequence: `the game lists controller
    054C:0CE6 as an XInput device itself` -> `shown as XInput controller 0` -> `the game polls XInputGetState`; no
    DirectInput joystick is created for it any more.
- 2026-10-21 **Battlefield 4: looking for the camera** (in the campaign, EA app build 179547; external read-only scans
  plus one-off write tests, game focused; `tools/frostbite/fbmem.py` now takes `FB_EXE=<exe>` so the Dead Space tools
  run on `bf4.exe`):
  - `heapscan3.py`: hundreds of unit vectors follow the camera; a full camera transform (right, up, forward rows,
    then the world position, no roll) used as the reference. Heap addresses only (not reusable).
  - `deltascan.py` against it: 555 floats changed by exactly the camera's yaw in both turns. The game keeps **(yaw,
    pitch) pairs in radians**, copied into ~20 kinds of objects (e.g. `ClientSoldierBreathControlComponent`, ~6
    snapshot-like objects with the pair at +0x24C/+0x254); also -yaw and yaw - 2pi copies.
  - **Write tests** (+20 deg once, camera transform watched): every copy was rewritten within a frame (or kept the
    value without effect, unused buffers), except **one: the yaw at +0x130 of an object with vtable exe+0x1E60780
    (not reflected: no getType in its vtable; 21 objects share it, 4 carry the live pair at +0x130)**: the camera
    turned 20 deg and stayed, and every copy followed. Positive yaw = left in the camera transform's atan2(forward.x,
    forward.z) convention. **But it isn't stored state**: the test had put the old value back 1 s later and the
    camera kept the turn; a live write (30 deg/s, ~250 writes/s, 4 s, user in gameplay) and two repeats of the single
    write did nothing. It's a value the game writes and reads back within one frame: an outside write only counts
    when it lands in that window (1 hit in ~50 writes). Usable only from inside the frame (a hook on the code that
    writes or reads it).
  - The pitch next to it (+0x134) is **not** the source: a +8.6 deg write was undone and the camera didn't move. Its
    sign is opposite to the camera's (stored -5.6 deg while looking 5.6 deg up). The pitch's source is elsewhere.
  - The same object also holds a position (+0x60, +0x70) and an earlier copy of the pair (+0xE4/+0xE8).
  - Next: Gyro4All's write watcher on that yaw (`WriteWatch=on` at start) to find the code that writes it and its
    pitch counterpart, and from there a way to find the object at run time (it has no TypeInfo).
- 2026-10-21 **Battlefield 4: the soldier's aim update found** (write watcher, EA app build 179547, campaign):
  - `WriteWatch` on the per-frame yaw (+0x60 of a 0xD0-byte record, no vtable): one writer, `bf4.exe+0xFBA52E`, in
    the **aim update `exe+0xFBA340(aim, out_delta, input)`**: input `[r8+0xC]` / `[r8+0x10]` = the look input (clamped
    to a unit circle), times a speed and the frame time; current angles **`aim+0x14` (yaw) / `aim+0x18` (pitch)**,
    radians; yaw wrapped by `exe+0xFAFFD0`, pitch clamped by `exe+0xFAFF40`; writes the target at `aim+0x60/+0x64`
    (time at +0x68) and **returns the frame's turn as a delta** at `[rdx]` / `[rdx+4]` (new - aim+0x14/+0x18).
    Called directly (no vtable slot) from `exe+0xFA6BFF` in `exe+0xFA69C0` with `aim = [rbx+0x10]`; the owner (rbx)
    has no vtable either. Caller chain: 0xFA6C04 <- 0x1010072 <- 0xFA7B54 <- 0x867927 <- 0x820AE8 <- 0x828258.
  - Conventions: camera transform yaw (atan2(forward.x, forward.z)) = 180 deg - stored yaw; stored pitch has the
    opposite sign to the camera's (forward.y = -sin(pitch)).
  - Live outside write on aim+0x14 (30 deg/s, ~210 writes/s, 4 s, twice): the camera turned 20.5 / 21.4 of 129 deg,
    always the right way, with a juddering trail (user-seen): the writes race the game's update (read, compute, write
    back), about 1 in 6 kept. **The stored aim angles are the right target; the gyro must go in from inside the
    update** (hook exe+0xFBA340 and add the gyro's turn to the returned delta, or write aim+0x14/+0x18 right before
    it runs).
- 2026-10-21 **Battlefield 4: real gyro, user-verified "now it is good"** (`bf4_camera.cpp`, `sync_gyro_frame` in
  `fb_engine.cpp`, `xinput::right_stick_pushed`; EA app build 179547 started from Game Pass, EA overlay off, GyroLib
  panel on; DualSense, connection and Steam Input state not recorded; on foot in the campaign, vehicles not tested):
  - Hook: the aim update `exe+0xFBA340`, found by its first 29 bytes (unique in the exe) and its target stores at
    +0x1E9; inline (MinHook), the call is direct. The player's record is the one whose look input moves while the
    XInput pad's right stick is pushed (so the gyro starts after one stick move); log `aim record 0x... is the
    player's`.
  - **Where the turn goes** (three tries, user-judged in game): (1) into the stored angles aim+0x14/+0x18 before
    the update: about a quarter of the turn, judder (between ticks the update starts from the target +0x60/+0x64,
    and the 30 Hz tick `exe+0xFB9610` copies the target over the stored angles: `mov eax,[rbx+60h]` ... `mov
    [rbx+14h],eax` at exe+0xFB9943, only when time since the tick (+0x68) > 1e-6 and the input record's +0x188 flag is
    set); (2) into both pairs: the full turn but an unstable image (the update returns the frame's view offset as
    new - stored angles, and both pairs moving together cancelled in it, so the view moved only on the 30 Hz ticks);
    (3) **after the update, into the returned delta and the target**, the stored angles left to the tick (with the
    +0x188 flag off: into the stored angles before): smooth, as fluid as the stick.
  - **Signs**: the gyro's own (yaw right, pitch up positive) go straight in; the opposite was inverted on both axes
    in game, although the camera transform had suggested stored yaw grows to the left and stored pitch is negative up
    (the transform's basis or the delta's convention isn't what it looked like; not resolved).
  - **Timing**: Gyro4All's frame ran after each Present on its own thread, the aim update on a job thread later in the
    frame, so a frame's turn landed in one update or the next. `sync_gyro_frame` (Frostbite module): the player's aim
    update wakes the frame thread and waits (at most 4 ms) for a frame that started after the request; status counts
    the updates that went on without it (0 in play). Frames then ~290/s (Present + aim), every aim update turned.
  - The GyroLib panel is back on (F10): with the EA overlay off it no longer freezes the game, and its graphics hooks
    give the Present wake (without it the frame thread ran on an 8 ms timer, beating against ~150 fps).
- 2026-10-21 **Titanfall 2: recon and slice 0, Source module stub** (EA app, v2.0.11.0, binaries linked 2017-12-05, the
  last PC patch; plan in `games/Titanfall2/MODDING_PLAN.md`; built, 661 checks pass, deployed; not tested in game yet):
  - Files: `Titanfall2.exe` is an EA/Origin OOA stub (`.text` entropy 8.0, `.ooa` section, only import
    `Core/Activation64.dll`), loading `bin\x64_retail\launcher.dll`. The game DLLs are plain: `engine.dll` imports
    `PeekMessageW`, `ShowCursor`, `GetForegroundWindow` and `WINMM.dll` (so does `client.dll`); `inputsystem.dll`
    loads `XInput1_3.dll` at run time (`XInputGetState`, `XInputGetCapabilities`, `XInputSetState` by name), uses raw
    input / HID and `PeekMessageA`. No anti-cheat files (`um scan`); multiplayer out of scope. Pad ConVars in
    `client.dll`: `gamepad_aim_speed[_ads]`, `gamepad_custom_*`, `joy_inverty`, `joy_requireFocus`.
  - `src/engines/source/` (new, `Source`): detection `r2\gameinfo.txt` + `bin\x64_retail\engine.dll`, inline hooks
    (the exe's single named import keeps the import scan from seeing a protected exe, and the prefix rule can't cover
    `engine.dll`/`client.dll`), always `no_camera` (blind stick fallback), `sees_menus` false.
  - Core: inline mode now also loads `xinput1_3.dll` before hooking (was: only if already loaded), so the game's later
    load gets the hooked module.
  - **The proxy must go in `bin\x64_retail\`, not next to the exe** (user: neither `winmm.dll` nor `version.dll` in the
    root loaded, with or without RTSS; the running game had `C:\WINDOWS\SYSTEM32\WINMM.dll`). `launcher.dll` loads
    the engine DLLs by full path with `LoadLibraryExA(path, 0, 8)` (`LOAD_WITH_ALTERED_SEARCH_PATH`, launcher+0x1a629
    and +0x1a6e4), so their imports (`WINMM.dll` from `tier0.dll`, `engine.dll`, `client.dll`, `vgui2.dll`, Miles,
    Bink) are searched in `bin\x64_retail\` then System32, never in the exe's folder. Gyro4All's folder is then
    `bin\x64_retail\Gyro4All\`. `deploy-gyro4all.sh` takes `EXE=` for a folder without the exe:
    `EXE=Titanfall2.exe bash tools/deploy-gyro4all.sh "<game>/bin/x64_retail"`.
- 2026-10-21 **Titanfall 2: the view angles and the pause flag** (v2.0.11.0, campaign, read-only external reads of the
  running game; built, 661 checks pass, deployed; the gyro writes not tested in game yet):
  - `CreateInterface("VEngineClient013")` -> `engine.dll+0x763B78` (its create function engine+0x55030 returns it),
    vtable engine+0x5C9DB8. Slot 63 = **GetViewAngles** (engine+0x510F0: `or ecx,-1; call engine+0x78210` (the local
    client state), then copies client+0x101A4/A8/AC to [rdx]); slot 64 = **SetViewAngles** (engine+0x51150: rejects
    NaN/inf, `AngleNormalize`s each (engine+0x3F3E60), writes them back; on NaN it writes defaults from
    engine+0x14C68CD0). Slot 67 returns client+0x27C (GetMaxClients, as in the SDK's order after the view angles);
    slots 68-92 are key-binding functions.
  - engine+0x78210(slot): -1 = the active slot (virtual call on engine+0x7A64B0), clients at engine+0x7A64C0, stride
    0x1B638 bytes.
  - Angles: floats, degrees, Source's senses; yaw wraps at +-180, pitch seen -50..+75 with the mouse, roll 0 (user
    moved the mouse, recorded at 20 Hz).
  - **Pause**: client+0x160 (byte, engine+0x7A6620) 0 in play, 1 in the pause menu: the only one-byte flag of the
    client state that followed the user's pauses in both recordings (10 toggles in the first, 2 in the timed one).
    No client-state clock stops in the pause. engine.dll's `.data` is 0x145279C0 bytes in memory (a full dump every
    0.25 s made a 67 GB recording; stream it next time).
  - `source_engine.cpp`: turns through those two functions on the game thread (checked by their first bytes; the local
    client getter is read from GetViewAngles' call), gyro senses to Source's (pitch - , yaw -), pitch clamped +-89;
    `paused` from client+0x160; view_angles for the stick fallback. `profiles/Titanfall2.ini`: `XInput=2`
    (auto mode didn't show the DualSense: "it opens the controller's HID device itself", but the opener was
    dinput8.dll's enumeration; the game only reads XInput1_3).
- 2026-10-21 **Titanfall 2: SetViewAngles from the frame doesn't stay; the turn goes through client.dll's mouse step**
  (user-tested: DualSense shown as XInput works with `XInput=2`; the pause flag works in game, `source: paused` /
  `unpaused` with the menu; the gyro didn't turn the view):
  - With `SetViewAngles` called from the core's frame, `applied pitch` grew by 67 deg in 2 min while the view stayed
    near (31, 6), drifting slightly against the writes. The stick fallback didn't start: `view_angles` read back our
    own write in the same frame.
  - A diagnostic hook on engine SetViewAngles (callers by return address, every 10 s): one call per frame each from
    client.dll+0x250224, +0x2553AC (CreateMove, client+0x254D10: sets the engine from the user command's angles
    `[rdi+0xC]`), +0x256205 (client+0x2561E1: the command's angles `[rsi+0xC]` from the player's own angles
    `[rdi+0x23FC]`/`+0x2400`, through the local-to-world conversion client+0x2CB9A0), +0x256F13 and **+0x24F90C**
    (client+0x24F7A6: `GetViewAngles`, world-to-local client+0x2CBBC0, + the mouse's delta, `0x2CB9A0`, then
    `SetViewAngles`): ~144 calls/s each at ~144 fps. Their angles differ from the engine's by the frame's input, so the
    world angles are rebuilt every frame from the player's local angles; only the mouse step feeds a change back.
  - Now: SetViewAngles hooked (MinHook); `apply` accumulates the turn, and the hook adds it to the angles the mouse
    step's call (return address client+0x24F90C, checked by its `FF 90 00 02 00 00`) sets. A turn not taken by the
    next frame (pause, menus) is dropped. Status: `mouse step N calls (M with gyro)`.
  - User test: it turns, but limited. Log: mouse step 1437 calls in 10 s, only 337 with gyro: the "dropped if the step
    didn't run since the last frame" rule threw most of the turn away, because the core's frame runs several times per
    game frame (PeekMessageW). And the stick fallback started ("undid the gyro's camera writes 8 frames in a row"):
    `view_angles` didn't include the turn the step hadn't taken yet. Fixed: dropped only after 100 ms without a step
    call; `view_angles` includes the pending turn.
  - User test after the fix (DualSense as XInput, `XInput=2`; connection and Steam Input state not recorded): the log
    shows the mouse step taking the gyro on nearly every call (1438 of 1439 in 10 s at ~144 fps), the pause gate
    working, frame cost avg ~45 us (p99 ~190 us). The user's verdict on feel and directions not recorded yet. One stick
    fallback at the start (main menu: no mouse step, the dropped turn looked undone); now the gameplay view requires a
    mouse step in the last 100 ms (menus and loading are no gameplay).
  - **User-verified: "gira entero y las direcciones son correctas"** (full turn, both axes the right way; DualSense
    shown as XInput, EA app build; connection and Steam Input state not recorded). Not checked yet: the menu gate of
    the last build in the main menu, titan cockpit, wallrunning (the mouse step's local/world conversion should
    carry the gyro like the mouse), cutscenes.
- 2026-10-21 **Core: auto XInput no longer hides a DualSense the game closed unread, whatever opened it** (built, 661
  checks; deployed to Titanfall 2 with `profiles/Titanfall2.ini` no longer setting `XInput=2`; not tested yet). Titanfall 2's
  log: `xinput: the game reads controllers itself: it opens the controller's HID device itself (vendor 1356)` from
  dinput8.dll's enumeration (open, two IOCTLs, close, eight times; never read). Of `xinput::frame`'s three hiding
  branches, the `Any` sign and the Sony branch already skipped a DualSense with `nativepad::dualsense_ignored()`
  (opened, closed unread, none open 5 s later; RE8 fix 2026-10-17), but the last one (the game opening the
  controller's HID device) didn't, and hid it again. It now skips `ignored` / `rejected` too, so the pad is shown about
  5 s after start. RE7 / RE8 keep their `XInput=2` profiles (they work); with `DualSenseAsDualShock4=0` they might now
  work in auto as well (not tested).
  - User-tested in Titanfall 2 with `XInput=1`: `not shown: it opens the controller's HID device itself` at +2.3 s,
    then `DualSense Wireless Controller shown as XInput controller 0` at +7.0 s (4.7 s later); the game read its sticks
    and the gyro turned the view.
  - To test: the proxy loads from the exe folder (`hook: PeekMessageW` lines), the game polls XInputGetState, the
    DualSense works as an Xbox pad, the gyro turns as right stick in the campaign; menus (gyro moves the stick there
    too unless the cursor shows); Steam Input off.
- 2026-10-21 **Build: Gyro4All 1.6.0** (version chosen by the user). `build/release/Gyro4All-v1.6.0.zip`. What's new
  since 1.5.0: the Source engine module (`src/engines/source/`, Titanfall 2: gyro through client.dll's mouse step,
  pause gate, install in `bind_retail`), `profiles/Titanfall2.ini`, inline mode also preloads `xinput1_3.dll`,
  auto XInput no longer hides a DualSense the game closed unread after a DirectInput enumeration (not yet tested in
  game), `deploy-gyro4all.sh` `EXE=` option.

- 2026-10-21 **Battlefield Hardline (EA app, Game Pass; build 612926): the BF4 route, real gyro on foot** (Frostbite
  module, user-verified with a DualSense):
  - Protection like BF4 (Origin OOA `.ooa`, Activation64.dll, packed `.xtext`/`.xdata`/`.text1`): inline hooks.
    Settings folder `Documents\BFH` (not `Battlefield Hardline`).
  - Type reader: 2013 layout works unchanged, DataContainer at `bfh.exe+0x2C38760`, 12326 types.
  - Camera: the same soldier aim update as BF4, `bfh.exe+0x4388620`, same offsets (stored angles `+0x14/+0x18`, target
    `+0x60/+0x64`, time `+0x68`, look input `input+0xC`, extrapolation flag `input+0x188`). BF4's pattern missed it
    twice: `mov r11,rsp` is encoded `49 89 E3` (BF4 `4C 8B DC`), and the target stores use other registers and store
    pitch first (`movss [rbx+64h]; movss [rbx+60h]; mov eax,[rdi]; ..; mov [rbx+68h],eax` at +0x265; BF4's at +0x1E9).
    `bf4_camera.cpp` (log name now "Battlefield") takes either `mov r11,rsp` before the rest of the 29-byte start and
    checks the stores by shape (+0x60 and +0x64 in either order, then +0x68, on one base register, in the first 0x300
    bytes). User: real gyro on foot works; in a car cutscene (stick look only) the update doesn't run and the gyro is
    off, as wanted. After a cutscene the soldier gets a new aim record: the right stick has to move once again.
    (Before this, with no camera model, the blind stick fallback ran and worked as right stick.)
  - Race: the camera model's pattern scan (attach thread) and the XInput check's `"IG_"` scan (the game's
    DirectInput8Create) ran together once and the check found 0 functions, so the DualSense stayed a DirectInput
    joystick (the failed attempt only retries on another DirectInput8Create, which didn't come). `scan_image` shares
    one static buffer: scans are now serialised with a mutex.
  - Controller: the exe has **no XInput import at all** (no slot holding any XInput export of xinput9_1_0/1_3/1_4,
    no "xinput*.dll" text in the running image): XInput is resolved at run time, so `fb_xinput.cpp`'s import route
    found nothing and the game made the DualSense a DirectInput joystick (left stick turned the camera, no other
    input). The WMI `IsXInputDevice` sample is there (wide `Win32_PNPEntity`, `DeviceID`, `IG_`, `VID_` at
    `+0x1F45510..0x1F45560`), loaded by `lea r64,[rip+..]` in one function, `bfh.exe+0x3CB9140`, called once with
    `call; lea rdx; test al,al; je` as in BF4. New fallback route: the wide `"IG_"` string -> `find_lea_refs`
    (fb_types) -> its function = the check; hooked 250 ms after the types, the game lists 054C:0CE6 as an XInput
    device and polls XInputGetState. User: buttons and gyro work in the campaign.
  - No freeze with the EA overlay state the user had (not checked whether it was on).

- 2026-10-22 **Ghostrunner** (UE 4.26.2) and **Ghostwire: Tokyo** (UE 4.27.2; Denuvo removed 2024-04-17), engine
  versions from PCGamingWiki: the user reports both work with the generic view (no profile). Added to `docs/GAMES.md`.

- 2026-10-22 **Kena: Bridge of Spirits** (`Kena\Binaries\Win64\Kena-Win64-Shipping.exe`, UE4; this copy also has
  ReShade `dxgi.dll` + RenoDX and an Ultimate-ASI-style fix `KenaCore.dll`): no panel, no gyro. `Gyro4All.log`
  ended at the PeekMessageW hook line: the first process (the one that owned the session) exits ~70 ms after
  start and a second `Kena-Win64-Shipping.exe` (the real game) starts at the same time, which then found the
  mutex taken and stayed a plain proxy (the two ReShade logs show the two PIDs). No `session: handed to` line:
  the relaunch comes from `CODEX64.dll`'s own `KERNEL32!CreateProcessW` import, and the CreateProcess hooks
  were import-table hooks on game modules only (`kena-` prefix + exe). Fix: `dllmain.cpp` now hooks
  `KERNELBASE!CreateProcessA/W` inline (MinHook, kernel32's only jump there), so a relaunch from any DLL hands
  the session over. In game (DualSense, report 0x01 = USB or basic Bluetooth): the relaunched process now runs
  Gyro4All, the F10 DX12 panel opens and closes; motion data arrives (~460 samples/s). Only the main menu was
  reached (`target=CineCameraActor`, no pawn) in that log. Later the user reported the gyro works in gameplay
  (DX12, generic ControlRotation view, no profile); `docs/GAMES.md`: ✅ Works.

- 2026-10-22 **Build: Gyro4All 1.6.1** (version chosen by the user). `build/release/Gyro4All-v1.6.1.zip`. What's new
  since 1.6.0: CreateProcessA/W hooked inline in KERNELBASE, so a game exe relaunched by any DLL (Kena's
  `CODEX64.dll`) gets the Gyro4All session; Kena: Bridge of Spirits in `docs/GAMES.md`.

- 2026-10-22 **Ghostrunner 2** (`Ghostrunner2-Win64-Shipping.exe`, UE 4.25+ FField layout, D3D11, Epic build): works
  with the generic ControlRotation view, no profile (user). **False "camera ignores the gyro" notice at start:** in the
  main menu the game possesses `BP_MenuPawn_C` (PC `PlayerControllerOML`), not a `DefaultPawn`, so the menu counted as
  a gameplay view; its camera is fixed, the follow check saw 321 degrees of gyro and 0 of camera, warned and started
  AutoProbe in the menu (snapshots of `BP_MenuPawn_C` / `BP_CinematicCamera_C`, useless). Valor Mortis has the same
  `BP_MenuPawn_C`. Fix (`ue_engine.cpp` `base_view`, new `ue::is_a_like`): a pawn whose class chain has a name
  containing "MenuPawn" (case-insensitive) is "no gameplay view", like `DefaultPawn`: gyro off in that menu, no follow
  check. Built and unit tests pass (no version bump); not tested in game yet.

- 2026-10-22 **Build: Gyro4All 1.6.2** (version chosen by the user). `build/release/Gyro4All-v1.6.2.zip`. What's new
  since 1.6.1: Unreal menus whose pawn class contains "MenuPawn" (Ghostrunner 2, Valor Mortis) count as "no gameplay
  view" (gyro off there, no false "camera ignores the gyro" notice / AutoProbe); Ghostrunner 2 in `docs/GAMES.md`.

- 2026-10-22 **Mass Effect Legendary Edition (ME1): crash loop at start, `.ooa` = protected exe, UE3 detection** (Steam,
  `MassEffect1.exe` 2.0.0.48602; plan in `games/MassEffectLE/MODDING_PLAN.md`). Gyro4All 1.6.2 as `winmm.dll` in
  `Game\ME1\Binaries\Win64`; the user also has LEBinkProxy (`bink2w64.dll` + `bink2w64_original.dll`, `ASI\` with
  AutoTOCLE and LE1AutoloadEnabler), ReShade (`dxgi.dll`) with the Luma addon.
  - The exe: BioWare's UE3 fork, packed by EA's activation wrapper (`.ooa` section; static imports only
    `Core/Activation64.dll`, `PhysXLoader64.dll`, `d3d11.dll`, `dxgi.dll`, all with name tables). Cooked packages in
    `BioGame\CookedPCConsole`. `um scan`: "Unknown native engine".
  - **The proxy loads, but the game hangs at start** (both runs, 19:36 and 20:03): `Gyro4All.log` stops after
    `engine: none detected (...), using Unreal`, no `Gyro4All.ini` created. The process stays alive at ~34 MB in a WER
    loop (Application log: 1841 events 1000, `ntdll.dll+0x1a12d`, 0xc0000005) and keeps the session mutex, so a second
    `MassEffect1.exe` the user started ran without Gyro4All. Stack (cdb, non-invasive attach): the `.ooa` stub's
    unpacked code (heap address) calls `LoadLibraryExW` → our `DllMain` → `attach` → `hooks::init` → `register_hook`
    (LoadLibraryW) → `patch_module` → `patch_slots` → **`strcmp` access violation**: the exe's import tables are walked
    while the stub is still filling them, and a name RVA points outside the image. The fault in `DllMain` then recurses:
    `__CxxFrameHandler4` → `__vcrt_getptd` fails → `abort` → the CRT lock faults again (608 rounds on the stack).
  - Why import-table mode: the stub's small import table has name tables, so `scan_imports` doesn't see a protected
    exe. BF4/Hardline (Frostbite) and Titanfall 2 (Source) only worked because their modules force `inline_hooks()`.
  - Fix (`hooks.cpp`): an exe with an `.ooa` section is a protected exe (inline hooks, import tables untouched), log
    `imports: exe has an .ooa section (EA activation wrapper): ...`. And every import walk (`patch_module`,
    `patch_slots`, `scan_imports`) now checks each RVA it follows against `SizeOfImage` (`in_image`), so a table that is
    still encrypted is skipped instead of read wild.
  - New engine module **`Unreal3`** (`src/engines/unrealEngine3/`, detection only): a `<root>\<Game>\CookedPC*` folder
    two levels above the exe's folder (ME1: `BioGame\CookedPCConsole`); after Unreal in the detection order (UE4/UE5
    have `Content\Paks`). No scene reader yet: never a gameplay view, the gyro stays off. Log `ue3: cooked packages in
    BioGame\CookedPCConsole; no UE3 scene reader yet, ...`.
  - Built, 661 checks pass. **Not tested in game yet** (no version bump, not deployed).
- 2026-10-22 **Mass Effect LE (ME1): the fix works, XInput translation didn't** (working tree build deployed as
  `winmm.dll`; DualSense, connection not recorded; Steam overlay loaded, no per-game Steam Input override; game launched
  by the user from Steam):
  - The game starts normally: `engine: Unreal3 (detected)`, `imports: exe has an .ooa section ...`, every inline hook
    `MH_OK`, PeekMessageW frame on the main thread (first call from `gameoverlayrenderer64.dll+0xB1E26`), D3D11 swapchain
    3840x2160 (the static panel via `gyro4all.dll`), motion ~460 samples/s.
  - Input seen: dinput8 enumerates HID devices at start (054C:0CE6 opened and closed many times, IOCTLs 0xB01A8, 0xB0193,
    0xB019C); the game creates DirectInput mouse, keyboard and a game controller for the DualSense (`054C:0CE6, type
    0x15, 6 axes, 15 buttons`). XInput auto: "not shown: it opens the controller's HID device itself", then 4 s later
    **"shown as XInput controller 0"**, WM_DEVICECHANGE to 3 windows; the sticks moved in our reads. But the game never
    polled: no `nativepad: ... polls XInput` line from nativepad's diagnostic hook on `xinput1_4!XInputGetState`.
  - Cause: the exe imports **`XINPUT1_4.dll`** (string in the exe), and protected exes only had xinput9_1_0/xinput1_3
    translated (xinput1_4 was left to GyroLib's SDL). Likely the game asks `XInputGetCapabilities` first (real answer: no
    Xbox pad) and so never polls the state (not confirmed).
  - Fix (`xinput.cpp`): in inline mode xinput1_4's `XInputGetState/SetState/GetCapabilities/Enable` are hooked too, with
    their own originals (`hk14_*`, `g14_*`). Calls whose return address is in gyrolib.dll (SDL, by name too since SDL
    polls before `g_our_sdl` is known), Gyro4All itself, gyro4all.dll or xinput9_1_0 (forwards to xinput1_4) pass
    straight through; every other caller gets the translation. The hook bodies are shared (`get_state`, `set_state`,
    `get_caps` take the real function). nativepad's diagnostic hook on the same export is gone (MinHook can't hook
    one function twice); the translation logs `xinput: the game polls XInputGetState`. Also applies to BF4, Hardline
    and Titanfall 2 (inline mode): their xinput1_4 calls from other modules now get the translation too.
  - Built, 661 checks pass. Not tested in game yet.
- 2026-10-22 **Mass Effect LE (ME1): why the game still never saw the XInput controller** (in game, user; DualSense,
  connection not recorded; Steam overlay on; read-only analysis: cdb non-invasive attaches, a dump of the unpacked
  image kept outside the repository, offline disassembly with capstone):
  - The xinput1_4 hooks were installed (`MH_OK`, "with GyroLib's calls passed through") but still no `the game polls`.
  - The exe's resolved import table (protector-filled, not in the PE headers) has `xinput1_4!XInputGetState` at
    **`MassEffect1.exe+0xF9DE58`** and `XInputSetState` at `+0xF9DE60` (`.rdata`), used through thunks at `+0xDAE85A` /
    `+0xDAE860`. No `XInputGetCapabilities`.
  - UE3's controller list: globals `+0x1B9B250` (array of 0x1F4-byte entries) and `+0x1B9B258` (count). Entry: +0
    IDirectInputDevice8 (0 for XInput), +8 type (3 = XInput), +0xC XInput slot, +0x1E0 DirectInput instance GUID,
    **+0x1F0 "connected"**. Start-up (`+0x98CF60`) adds 4 XInput entries (slots 0-3, flag 1). The DirectInput enum callback
    (`+0x989F40`) adds an XInput entry for devices `+0x98E280` (the WMI "IG_" check) calls XInput; other pads only
    with whitelisted caps (axes/buttons/POVs 5/10/1, 5/16/1, 4/16/1, 4/12/1, 7/24/1, 8/16/0 + type 0x10215): the
    DualSense (6 axes, 15 buttons) isn't one, so no doubled DirectInput input. Per-frame poll: `FWindowsViewport`
    (vtable `+0x120E7B8`, second vtable `+0x120E7D0`; no RTTI; class from the `IG_` / `Win32_PNPEntity` strings
    before it) slot 23 = `+0x9943D0` ProcessInput, called for every viewport from `+0x98F210`: gates `[this+0x3C]`
    viewport client, `[[this+0xBC]+0x248]`, **`GetFocus() == [this+0xC4]`** (the window, `+0x992820`), then per entry:
    DirectInput `GetDeviceState(0x110)` or, if flag +0x1F0, `XInputGetState(slot)`; a failure clears the flag for good
    (+0x68CE90, "any controller connected", does the same). The window and the engine loop are on the main thread
    (the PeekMessageW game thread); GetGUIThreadInfo while the game was in front: active = focus = the game window.
  - After playing, all four flags were 0: XInputGetState **was** called and failed for every slot, yet Gyro4All's
    hook never ran (`g_game_polls` false, read from our globals with the build's PDB). The export's first `jmp` goes
    to a relay page whose target is **`gameoverlayrenderer64.dll`** (the Steam overlay also hooks PeekMessageW
    there): Steam hooked the export after Gyro4All and answers XInputGetState itself (Steam Input) without calling
    the original. Import-table mode never had this problem (the exe's slot is in front of any inline hook).
  - Fix: `hooks::redirect_slots` (new): one sweep of the exe's non-executable sections (region by region, readable
    pages only) for pointer slots holding given exports, pointed at Gyro4All and kept in `g_patched` (verify()).
    `xinput.cpp`: in inline mode the watcher points the protector's xinput1_4 slots (GetState, SetState,
    GetCapabilities, Enable) at `hk_imp14_*` (up to 5 passes; log `hooks: xinput1_4.dll!XInputGetState slot at
    MassEffect1.exe+0x... (the protector's import table) now points to Gyro4All`). Their real function is the export
    itself, through whatever hooks it, so Steam Input keeps its own controllers. A thread_local guard (`Inside`) lets
    an XInput call nested in a translated one (Steam's hook calling through to our inline hook, xinput9_1_0
    forwarding) pass straight on, so nothing is translated twice.
  - Built, 661 checks pass. Not tested in game yet.
- 2026-10-22 **Mass Effect LE (ME1): XInput translation works** (user, in game; DualSense, connection not recorded; Steam
  overlay on, Steam Input off for the game according to the user): `hooks: xinput1_4.dll!XInputGetState slot at
  MassEffect1.exe+0xf9de58 (the protector's import table) now points to Gyro4All` (and XInputSetState at +0xf9de60) on
  the first pass, `xinput: the game polls XInputGetState` 7 s later; sticks and buttons work in game. So the Steam
  overlay's hook answers XInputGetState without calling through even with Steam Input off (why not checked).
  - Next slice: the gyro as right stick. `Unreal3` now reports `no_camera` (blind stick fallback with the default stick
    model, view 1, `sees_menus` false), so the gyro turns the camera through the game's own stick handling; no pause or
    menu gate yet. Built, 661 checks pass; not tested in game yet. Then: the UE3 scene reader (GObjects/GNames,
    `Controller.Rotation` as int32 rotator) for direct camera writes.
- 2026-10-22 **Mass Effect LE (ME1): UE3 reflection and the camera, first reader** (read-only external reads of the
  running game with a ReadProcessMemory script outside the repository; level BIOA_STA00, on foot):
  - **GObjects** `MassEffect1.exe+0x1770670` (TArray<UObject*>, 183762 objects), found by shape; matches ME3Tweaks'
    LE1-SDK (`SdkInitializer.h`), which also gives **GBioNamePools** `+0x16A2090` (array of chunk bases: chunk 0
    `MassEffect1.exe+0x1338FA0` static, chunk 1 heap). A copy of chunk 0's base sits at `+0x169A080` (decoy for a
    search). **FName** (LE1-SDK): `Offset:29` (bytes into the chunk), `Chunk:3`, then int32 Number; name entry `u32
    FlagsAndMeta`, packed 8-byte `HashNext`, text at +12. Dead ends first: a flat pool base, a GNames-style
    TArray<FNameEntry*> indexed by the value.
  - **UObject**: +0x08 index, +0x40 Outer, +0x48 Name, +0x50 Class, +0x58 ObjectArchetype. **UProperty**: +0x70
    ArrayDim, +0x74 ElementSize, **+0x94 Offset** (found from FMatrix's planes at 0/0x10/0x20/0x30).
  - Offsets by reflection: `Engine.Engine.GamePlayers` +0x498, `Engine.Player.Actor` +0x68, `Engine.Actor.Rotation`
    +0x114, `Engine.Actor.Location` +0x108, `Engine.Controller.Pawn` +0x3DC, `Engine.Actor.WorldInfo` +0x1AC,
    `Engine.WorldInfo.Pauser` +0x644, `Engine.PlayerController.PlayerCamera` +0x6B0, `Engine.Camera.CameraCache`
    +0x45C (POV rotation at +0xC in it), `Engine.Camera.ViewTarget` +0x3EC. No `bShowMouseCursor` in this UE3.
    `GEngine` candidates holding the GameEngine pointer: `+0x172E5C0`, `+0x17813E8`.
  - Scene: `Transient.GameEngine_0` → `LocalPlayer_0` → `BIOA_STA00.TheWorld.PersistentLevel.BioPlayerController_1`
    (pawn `SFXPawn_Player_1`, camera `SFXPlayerCamera_1`). Controller Rotation (65087, -48115, 0) = pitch -449 units,
    yaw -48115 ≡ 17421 = the pawn's yaw and the camera cache's yaw: the camera follows the controller's Rotation
    (unwrapped yaw, pitch stored 0..65535).
  - **Live write test (user's request; WriteProcessMemory of the controller's Rotation, game running, on foot):** yaw
    +45° (8192 units) stayed and the camera cache's yaw followed 1:1 (95.7° → 140.7°). Pitch: -10, 3, -1, 5..40, -20..-60
    degrees all stayed and the camera's pitch followed exactly; -80 was clamped to **-60** by the game (lower limit;
    upper limit not reached at +40). A first +7.5° write right after the yaw write went back to the old -2.5° within
    50 ms and didn't reproduce (cause unknown). Other pitch copies (effects, not sources): `SFXGame.BioPawn.m_vAimDirection`
    (+0x760, Z = sin(pitch)), the camera's `ViewTarget` POV (+0x3F8), `CameraCache` POV (+0x468) and
    `SFXGame.SFXPlayerCamera.CameraDesiredPosition` (+0x5A4, rotation at +0x5B0).
  - `Unreal3` module rewritten: all of the above found at run time (GObjects by shape with the index check, the chunk
    array by the class Class's name plus sample names from every chunk in use, the property offset field from FMatrix,
    the members and GameEngine by name; retried every 2 s, the stick fallback after 60 s). Each frame: gameplay view
    when the controller has a pawn and `Pauser` is empty; apply adds the gyro to Rotation (65536/360 units per degree,
    fractions kept; pitch wrapped to 0..65535, the game limits it); `view_angles` reads it back. No dialogue/cutscene
    or menu (GFx) detection yet. Built, 661 checks pass; not tested in game yet.
- 2026-10-22 **Mass Effect LE (ME1): the direct gyro works; the pause menu** (user, in game; DualSense; the UE3 reader
  installed): the gyro turns the camera through `BioPlayerController.Rotation` "perfectly" (user), but kept turning it
  behind the pause menu: `WorldInfo.Pauser` stays empty there.
  - A 2-minute recording of the scene objects (100 ms) didn't catch it: `WorldInfo.TimeSeconds` never stopped (the menu
    doesn't stop game time, or it wasn't opened in the window); what toggled was noise: `BioWorldInfo.BioRegisteredMusic`
    count (+0x998/+0x9A0), `LocalPlayer` +0x25C = the current PostProcessVolume, pad state in `LocalPlayer` past +0x500.
  - Snapshot menu closed vs open (user holding the menu open): new `BioSFPanel` x5 under
    `SFXGameViewportClient_0.MassEffectGuiManager_0`, `GFxMovie` x4, `BioSFHandler_Journal`, `BioSFHandler_SharedIGG`
    x2 (`LE1CP_MainWheel` too: a Community Patch class); **`WorldInfo.bPlayersOnly`** (+0x790, mask 0x80) set, read back
    live with the menu open. (`Actor.bTicked`, +0x270 mask 0x10, flips with the frame: noise.) UBoolProperty's bitmask
    is at **+0xD0** in this UE3 (+0x98 is the next-property link).
  - `Unreal3`: paused while `Pauser` is set or `bPlayersOnly` is on; no gameplay view while `WorldInfo.bIsMenuLevel`
    is on (meant for the main menu, `entrymenu`, whose controller also has a pawn: the gyro ran there; not checked that
    entrymenu sets it). Both bools found by reflection (offset + mask, the mask checked as a power of two), log
    `ue3: world is a game level, frozen (bPlayersOnly: a menu)`. Built, 661 checks pass; not tested in game yet.
  - **Verified in game** (user: "funciona"; DualSense; Steam overlay on, Steam Input off): `ue3: GObjects at
    MassEffect1.exe+0x1770670 (122796 objects), names at MassEffect1.exe+0x16A2090, property offset field +0x94, 21 ms`;
    the main menu reads `world is a menu level` (entrymenu sets bIsMenuLevel: no gyro there), the save loads as
    `world is a game level`, every pause logs `frozen (bPlayersOnly: a menu)` and back. Gyro on foot 1:1 through the
    controller's Rotation, off in the pause menu. Still open: dialogue/cutscene detection (the gyro still turns the
    controller there), the Mako, other full-screen menus (inventory, map, journal) not checked one by one, ME2/ME3.
- 2026-10-22 **`tools/unrealEngine3/ue3tool.py`** (new, with README; the useful parts of this session's scratch scripts,
  nothing fixed per game; standard library only): `find` (GObjects by shape; UObject index, then Outer/Name/Class as
  three consecutive pointers, Class = the field where the class Class points to itself; names as BioWare SFXName chunks
  or UE3 GNames, whichever reads "Class" plus identifier samples; UProperty Offset/ElementSize/ArrayDim from FMatrix's
  planes; UBoolProperty mask from Actor's bools sharing one offset; UStruct.SuperField from PlayerController → Controller),
  `objects`, `props` (fields only, not function parameters), `scene` (with WorldInfo bPlayersOnly/bIsMenuLevel/Pauser),
  `snap`/`diff` (object counts, new objects, changed fields named through the object's class chain) and `write-test`
  (the only writer). Layout cached per process in %TEMP%. **Tested on ME1** (Steam, in game): `find` in 2.7 s gives
  GObjects exe+0x1770670, names exe+0x16A2090 (skips the +0x169A080 decoy), Outer/Name/Class +0x40/+0x48/+0x50,
  Offset +0x94, ElementSize +0x74, ArrayDim +0x70, bool mask +0xD0, **SuperField +0x60** (new); `scene`, `objects`,
  `props`, `snap` (~7 s) / `diff` and `write-test` (pitch ±15° held and restored) all work. Two fixes during the
  test: the Class field search started misaligned; the name cache was created after the search used it.
- 2026-10-22 **Mass Effect LE (ME1): ReShade stopped loading with Gyro4All (our fault)**: `ReShade.log` (ReShade 6.8.0.1
  as `dxgi.dll` next to the exe, with the Luma addon) was last written at 20:08, before the first run where Gyro4All
  started properly (20:20); no ReShade in any run since. Cause: on protected exes (inline mode) the overlay's
  `hook_system_factory` loads `System32\dxgi.dll` by full path on its thread right after DllMain, before the game's own
  dxgi import (resolved later by the .ooa stub); the loader then gives every "dxgi.dll" request that module, so the
  wrapper in the game folder never loads. Fix (`overlay.cpp`): if no dxgi.dll is loaded and one sits next to the exe,
  load it first (log `overlay: the game's own dxgi.dll (a wrapper, e.g. ReShade) loaded first`); it loads System32's
  itself, whose factory vtable is then hooked as before. Built, 661 checks pass; not tested in game yet. Possibly the
  same with Battlefield 4 / Hardline / Titanfall 2 with a dxgi.dll wrapper (not checked).
- 2026-10-22 **Build: Gyro4All 1.6.3** (version chosen by the user). `build/release/Gyro4All-v1.6.3.zip`. What's new since
  1.6.2: Mass Effect Legendary Edition (ME1) support with the new `Unreal3` engine module (direct gyro through the
  controller's Rotation, off in the pause and main menus); `.ooa` exes treated as protected and bounds-checked import
  walks (no crash at start); XInput on protected exes through xinput1_4 and the protector's own import slots (ahead of
  the Steam overlay's hook); a dxgi.dll wrapper next to the exe (ReShade) loaded before System32's on protected exes
  (ReShade fix not tested in game yet); `tools/unrealEngine3/ue3tool.py`.
- 2026-10-23 **Mass Effect LE (ME2): no camera, two causes** (Steam; 1.6.2 installed in `Game\ME2\Binaries\Win64`;
  log `ue3: property Engine.GamePlayers not found`, then the stick fallback). Found with `ue3tool.py` on the running game:
  - **Decoy name array**: `MassEffect2.exe+0x16609F8` has chunk 0's base right and junk (unaligned) after it; the real
    chunk array is **`+0x1668A10`** (chunk 1 on the heap). Both the module and the tool only sampled names from the first
    objects, all in chunk 0, so the decoy passed and every chunk-1 name read wrong. Fix: samples spread over all of
    GObjects (module: ~20000; tool: ~4000, 24 per chunk).
  - **The engine is `Transient.SFXEngine_0`** (class `SFXEngine`, a GameEngine subclass); only `Default__GameEngine` has
    the class GameEngine. Fix: the engine is the first non-default object whose class chain (UStruct.SuperField +0x60,
    as in ME1) reaches `GameEngine`; the tool's `find_object` matches subclasses too.
  - ME2 layout (same UObject/UProperty fields as ME1): GObjects `+0x173CC48` (153373 objects), Offset +0x94, mask +0xD0;
    `Engine.GamePlayers` +0x498, `Player.Actor` +0x68, `Actor.Rotation` **+0x124**, `Controller.Pawn` **+0x294**,
    `Actor.WorldInfo` +0x1B0, `WorldInfo.Pauser` +0x684. Scene: `SFXPlayerController_3`, pawn `SFXPawn_PlayerSoldier_1`,
    camera `SFXPlayerCamera_3`, `BioWorldInfo_0` (level Biop_Procer). Not checked yet: that the camera follows the
    controller's Rotation in ME2, and that bPlayersOnly marks ME2's menus. Built, 661 checks pass; not tested in game.
  - First in-game try (1.6.3 working tree): `ue3: engine not found (GObjects found, names missing ...)`. My bug: the
    spread samples also looked for the class Class only at every 8th object and missed it. Fix: the class Class from
    every one of the first 20000 objects again, the stride only for the samples. Built, 661 checks pass; not tested.
  - **Verified in game** (user: "fixed"; DualSense): `ue3: GObjects at MassEffect2.exe+0x173CC48, names at
    MassEffect2.exe+0x1668A10, property offset field +0x94, 7 ms`; bPlayersOnly +0x7D8/0x80, bIsMenuLevel +0x7D8/0x4000;
    the main menu reads `world is a menu level`, the save `world is a game level`, pauses log `frozen (bPlayersOnly)`.
    The gyro turns the camera through the controller's Rotation, as in ME1. Connection and Steam Input state not recorded.
- 2026-10-23 **Build: Gyro4All 1.6.4** (version chosen by the user). `build/release/Gyro4All-v1.6.4.zip`. What's new since
  1.6.3: Mass Effect Legendary Edition 2 support (the UE3 name table checked with names from all of GObjects, past
  ME2's decoy array; the engine object found as any GameEngine subclass, ME2's SFXEngine); `ue3tool.py` the same.
  `docs/GAMES.md`: ME2 row added, removed from the not-yet-supported list.
- 2026-10-23 **Metro 2033 Redux recon (4A Engine)**, plan in `games/Metro2033Redux/MODDING_PLAN.md`. Steam 286690,
  buildid 26162904, "Next-Gen Update" (PC 2026-10-22): `metro.exe` FileVersion 1.0.0.4, linked 2026-10-23, x64,
  **SteamStub** (`.bind` section, `.text` entropy 8.0; import directory readable). Imports: `USER32!PeekMessageA` (no
  W), `WINMM.dll` (timeGetTime etc., so the `winmm.dll` proxy applies), `XINPUT1_3.dll` **by ordinal** (#2 GetState,
  #3 SetState), `DINPUT8!DirectInput8Create`, `HID.DLL` + `SETUPAPI.dll` (the game's own DualSense path, adaptive
  triggers), `amd_fidelityfx_dx12.dll` (DX12 renderer). Window class `_uengine_`. No anti-cheat. Pad cvars in
  `user.cfg`: `joy_sens_x`, `joy_sens_y_scale`, `joy_sens_aiming_x`, `aim_assist`, `invert_y_axis`; renderer `r_api`.
  No 4A engine module yet: the core falls back to the Unreal module (none detected). Slice 0 installed (winmm.dll).
- 2026-10-23 **Metro 2033 Redux slice 0 run** (DualSense USB, Steam Input: the game's Steam Input service reported no
  controllers): the `winmm.dll` proxy loads under SteamStub; `PeekMessageA` hooked through the exe's import table, the
  frame runs on the main thread (called from `metro.exe+0x656273`); `XINPUT1_3` ordinals 2/3 patched. The game opens
  the DualSense (054C:0CE6) over HID itself, 64-byte USB reports 0x01 (overlapped reads), so XInput translation stays
  off and `nativepad` follows its reports (buttons match SDL; long-press blocking available); it also opens
  DirectInput keyboard and mouse. GyroLib: SDL reader, ~475 samples/s. The Unreal fallback found nothing (heap scans
  of ~3 GB, ~0.5 s each, every 30 s): wasted work, gone with the 4A module.
- 2026-10-23 **4A Engine module** (`src/engines/4aEngine/`, namespace `foura`, `Engine=4A`): detection by
  `content.vfx` + `content*.vfs0` next to the exe; window class `_uengine_`. No camera hook yet: `no_camera` (the gyro
  as right stick through the blind stick fallback, via the game's own DualSense reports); menus = mouse cursor shown
  (`GetCursorInfo`), unverified for this game. Builds, unit tests pass.
- 2026-10-23 **Metro 2033 Redux, 4A module in game** (DualSense USB, Steam Input reporting no controllers): `engine: 4A
  (detected)`; the blind stick fallback turns the view (user confirmed). The cursor gate works: the cursor shows with
  the Steam overlay (focus to steamwebhelper) and the gyro stops, and resumes when it hides. **No settings panel**:
  `perf ... present: -`, no swapchain seen. Cause: `metro.exe` imports no `dxgi.dll`/`d3d11.dll`/`d3d12.dll` (it
  loads them itself, LoadLibrary + GetProcAddress), so the import-table `CreateDXGIFactory*` hooks never fire. Fix
  (unverified): `profiles/metro.ini` with `OverlayFactory=system` (the System32 factory's vtable, as in protected exes).
- 2026-10-23 **Metro 2033 Redux + `OverlayFactory=system`: panel up, then a crash before the main menu** (3 runs). The
  first D3D11 swapchain is captured and the static panel draws (~300 presents in 10 s, 2 us each); ~12 s later the game
  makes a second swapchain (same window) and crashes: `0xC00000FD` (stack overflow) on the game thread, ~19,900 cycles
  of `overlay.cpp hk_present` -> `gameoverlayrenderer64.dll+0x9b72d` (Steam's Present hook at `+0x9b700`: for
  `DXGI_PRESENT_TEST` it calls its saved original, `mov rax,[rip+0xe4c45]; call rax`, which is our hook). Without the
  profile (import-table DXGI hooks, which never fire here) there is no crash.
  **Dead ends** (both reverted): (1) a loop check in the four swapchain hooks (re-entered through the original already in
  flight) that switched the slot's original to its value in the vtable module's file: it fired at every level but found
  no file value; (2) the same with the hook's first saved original as fallback: still looped. So the vtable slot isn't in
  `dxgi.dll` and/or the saved originals are Steam's, or Steam detours code rather than vtable slots: the minidumps
  can't tell (no heap, no Steam code, two `winmm.dll` modules in the list).
- 2026-10-23 **Steam overlay off (game Properties) changes nothing**: `gameoverlayrenderer64.dll` is still injected and
  the same loop (~25,900 cycles, clean build) crashes the game. Offline check (a test program, plain D3D11 device +
  flip swapchain): the swapchain vtable is in `dxgi.dll` (+0x841352, shared by IDXGISwapChain3), and the file values
  of slots 8/13/22/39 equal the live ones, so dead end (1) did find dxgi's real Present: it was the very function
  already in flight. **Conclusion**: Steam inline-detours the function the swapchain's Present slot points to, each
  time the game creates a swapchain, keeping one saved original. At swapchain 1 that was dxgi's Present (Steam hooks
  before our capture patches the slot); at swapchain 2 the slot points to our `hk_present`, so Steam detours our hook
  and its one original becomes our hook's trampoline, also for its dxgi detour: hk_present -> dxgi Present (Steam's
  jmp) -> Steam -> hk_present -> ... Vtable patching can't be fixed from our side once that happens. Options: keep the
  slot's target unchanged while the game creates a swapchain (restore our slots around CreateSwapChain*, re-patch
  after), or hook dxgi's Present code inline instead of the slot (depends on how Steam checks for its own detour).
- 2026-10-23 **Why other games don't crash**: they make one swapchain per session (mode changes go through
  ResizeBuffers), so Steam hooks once, on dxgi's Present, and our slot hook just sits in front of it. Mass Effect LE runs
  the same system-factory D3D11 path with `gameoverlayrenderer64.dll` loaded and works. Any D3D10/11/12 game that
  recreates its swapchain under Steam can hit the loop (a latent core bug, not Metro's).
- 2026-10-23 **Fix: swapchain slots set aside during a creation** (`overlay.cpp`, `com::unhook_slot`/`rehook_slot`): every
  swapchain slot we patch is tracked (`patch_swapchain_slot`); around the game's CreateSwapChain, CreateSwapChainForHwnd,
  D3D10/D3D11CreateDeviceAndSwapChain (nested creations count once) they hold their saved originals, so a hooker that
  hooks at creation sees the same functions as the first time; afterwards our hooks go back in (a value written there
  meanwhile becomes our original, logged). Log: `overlay: N swapchain hook(s) set aside while the game creates a
  swapchain`. Builds, 661 checks pass; works only if Steam hooks during the creation (not at the new swapchain's first
  Present). Not verified in game yet.
- 2026-10-23 **Slot fix verified: no crash, panel works** (Metro 2033 Redux, DualSense USB, Steam overlay off in the game's
  properties, `gameoverlayrenderer64.dll` still injected): `5 swapchain hook(s) set aside while the game creates a
  swapchain` at the second swapchain (+12 s, 3840x2160), captured, no loop; F10 opened the D3D11 static panel (SDR, the
  game sets no colour space), closed from the panel.
- 2026-10-23 **ReShade's overlay open: our panel draws but ignores mouse and keyboard** (user report). ReShade
  (`source/input_windows.cpp`) blocks input process-wide while its overlay is open: its GetMessage/PeekMessage hooks turn
  mouse and keyboard messages into WM_NULL before dispatch (after TranslateMessage, so WM_CHAR still arrives),
  GetCursorPos returns the last position, GetAsyncKeyState/GetKeyState return 0, raw input and Windows hooks are filtered.
  Our panel reads all three (window procedure, GetCursorPos, GetAsyncKeyState for buttons). Not fixed (user's call);
  documented in TROUBLESHOOTING: use the controller (GyroLib reads it through SDL/HID, untouched by ReShade; not tested)
  or close ReShade's overlay. A possible fix if needed: GetCursorInfo for the cursor and win32u!NtUserGetAsyncKeyState for
  the buttons while the panel is open (no wheel or navigation keys; clicks would reach both overlays).
- 2026-10-23 **Metro 2033 Redux: camera angles found** (read-only scanner + write tests; scripts not kept in the repo yet).
  - Dead ends: Cheat Engine scans found only copies: the yaw in the NVIDIA driver's constant buffers (written by
    `nvwgf2umx.dll+0x5ddc62`, radians 0..2pi, right turn = decrease), camera direction-vector components (|v| <= 1), and
    a static renderer object at `metro.exe+0xF4C430` (vtable `metro.exe+0xE53C88`) whose view x projection matrices
    (`+0x150..0x180` x `+0x1A0..0x1C0` -> `+0x8500..0x8530`, written at `metro.exe+0x81F3D6`/`+0x81F896`) include an
    element (~-1544) that follows the yaw: writing any of them reverts.
  - Method that worked: snapshot all writable memory (4.4 GB, 64 MB batches), turn with SendInput relative mouse (+D,
    -2D, +D, idle) and keep values whose change is proportional and returns; then turn ~99 deg and keep only values
    whose rate per count is unchanged (an angle; sines and matrix elements change rate). 6465 -> 26 candidates.
  - **Master view angles** (heap, this session `0x195cab8bf60`): `float pitch, yaw, roll(0)`, radians, both 0.000823 per
    mouse count at the game's current sensitivity; yaw grows turning right and doesn't wrap (seen -2.09 .. -0.36);
    pitch grows looking down. Proof: writing it and nudging the mouse moves every copy (two more yaw copies at +0xC/+0x18,
    an 8-float history at another object, two negated copies) and the view; the copies alone revert.
    Seen in game (user): yaw +0.5 / -0.5 and pitch -0.3, each held 2 s, turned the view right, left and up, and back.
  - Pointer scan (2 levels, pointers <= 0x1000 below the target): 291 chains from `metro.exe` statics, e.g.
    `[[metro.exe+0xF41CE8]+0xC60]+0x5B0`, `[[metro.exe+0xFBD820]+0x30]+0x650`, `[[metro.exe+0xFBE028]+0x338]+0x5F0`.
    After a checkpoint reload only one of the 20 printed chains still reaches the angles (a +100-count turn moved its
    yaw by exactly 0.0823, pitch unchanged): **`[[metro.exe+0xFBD820]+0x30]+0x650`** = `float pitch, yaw, roll`. The
    global at `metro.exe+0xFBD820` points to an object with vtable `metro.exe+0xB22D20`; its `+0x30` to the object
    holding the angles, vtable `metro.exe+0xA81F08` (no RTTI in the exe: class names unknown). The yaw is repeated at
    +0x654, +0x660, +0x66C in that object. Not yet checked across a game restart; offsets belong to this build
    (metro.exe 1.0.0.4 of 2026-10-23): a code signature that finds the global is still to do.
- 2026-10-23 **4A module: direct camera** (`foura_engine.cpp`): reads `[[global]+0x30]+0x650` (pitch, yaw, roll; valid
  when finite, |pitch| <= 1.6 and roll 0) and turns it in memory (yaw += gyro yaw, pitch -= gyro pitch, radians; pitch
  kept within +-1.48 until the game's own limit is measured); view 1 while readable and no cursor, no view otherwise
  (menus, loading); recenter levels the pitch; `view_angles` for the stick fallback. The global: from a code pattern
  (`kGlobalCode`, empty until taken from the running game, since the exe is encrypted on disk), else this build's
  `metro.exe+0xFBD820`, used only while both objects carry this build's vtables (`+0xB22D20`, `+0xA81F08`); neither:
  the blind stick fallback as before. Builds, 661 checks pass, deployed; not run in game yet. Open: whether the view
  follows a write without mouse input (in the write tests the copies followed only after a mouse nudge).
- 2026-10-23 **4A direct camera verified in game** (user: "yaw and pitch ok"; DualSense; log `view angles at heap ...`,
  `gyro on (target view angles)`, 120-165 camera updates/s, 98-99% turned): the view follows the writes without any
  mouse input. Issues found: (1) pushing the pitch to the limit started the stick fallback for a second, several times
  (the module's clamp at 85 deg stopped the turn, judged as "the game undid the writes"; the game's own limit is 90.0
  deg), and its learning saved nonsense models (`StickFallbackLearned=axial,...,-20.9` "yaw inverted") into the
  installed metro.ini; (2) in the pause menu the camera kept turning behind it.
- 2026-10-23 **Fixes** (built, 661 checks pass, deployed; not verified in game yet):
  - New `Engine::limit_turn(pitch, yaw)` (default: unchanged), called by host.cpp before `apply`: trims the turn to what
    the view can take; the stick fallback judges the gyro's share of the trimmed turn. 4A: pitch limit +-pi/2.
  - Code signature for the view global: `48 8B 1D ?? ?? ?? ?? 48 8B 5B 30 48 85 DB 74` (`mov rbx,[rip+global]; mov
    rbx,[rbx+30h]; test rbx,rbx; je`, metro.exe+0x1F4A4B, unique in .text; one of 1126 loads of the global, 35 of them
    followed by +0x30). The global is used all over the code (probably the game/level object).
  - Pause: automatic scan (snapshots in gameplay, Esc sent as a DirectInput scan code, paused, Esc again) over the
    exe's .data and the path's objects. Game clocks (float seconds) that stop while paused: `metro.exe+0xF58420` (real
    rate) and `+0xF58428` (half rate), both inside the static renderer object `metro.exe+0xF4C430` (+0xBFF0, +0xBFF8).
    Many other values change with the pause too (menu animation). The object by `lea rcx,[rip+object]; jmp qword ptr
    [rax+290h]` (`48 8D 0D ?? ?? ?? ?? 48 FF A0 90 02`, metro.exe+0xEE1A7, unique), else this build's offset when its
    vtable is `metro.exe+0xE53C88`. Paused = the clock (+0xBFF0) stood still 200 ms while frames go on.
  - Verified in game (user; log): the code signatures find both (`view's global at metro.exe+0xFBD820 (found by its
    code)`, `game clock at metro.exe+0xF58420`), `paused`/`unpaused` follow the pause menu, no stick fallback at the
    pitch limits.
- 2026-10-23 **Automatic system DXGI factory** (`overlay.cpp` `loads_graphics_api_itself`, new `hooks::modular()`):
  when no game module imports (regular or delay-load) dxgi, d3d9, d3d10, d3d10_1, d3d11, d3d12, opengl32 or vulkan-1,
  the build isn't modular, the exe isn't protected and no `OverlayFactory=` is set, the overlay hooks the System32
  factory's vtable as `OverlayFactory=system` does (log: `the game imports no graphics API (it loads DXGI itself)`).
  `profiles/metro.ini` is gone (not needed). Offline import check of the installed exes: only metro.exe matches; the
  Unreal games here (Banishers, Jedi: Fallen Order, Out of Sight, Visage), RE2 and Revelations 2 import D3D/DXGI and
  keep the import-table path; RE Requiem's re9.exe not installed (not checked). Verified in Metro 2033 Redux (DualSense
  USB): automatic path, D3D11 swapchain captured, 5 swapchain hooks set aside at the second swapchain, no crash, F10
  panel opens.
- 2026-10-23 **Build: Gyro4All 1.7.0** (version chosen by the user). `build/release/Gyro4All-v1.7.0.zip`. What's new since
  1.6.4: Metro 2033 Redux support with the new `4A` engine module (direct gyro through the player's view angles, found by
  code signature; pitch limit; pause by the game clock; cursor gate); `Engine::limit_turn`; the system DXGI factory
  picked automatically for games that load DXGI themselves; swapchain hooks set aside during a swapchain creation (no
  stack overflow with Steam's overlay when a game recreates its swapchain); `docs/GAMES.md`: Metro 2033 Redux row;
  TROUBLESHOOTING: the panel and ReShade's open overlay.
- 2026-10-24 **Metro: Last Light Redux (Epic Games Store) with 1.7.0: "no panel, no gyro, no stick fallback"** (user
  report, DualSense USB). The 4A module works unchanged: detected, the same code signatures find the view's global at
  `metro.exe+0xFBD820` and the clock at `metro.exe+0xF58420` (same offsets as 2033 Redux's exe), view angles readable and
  following the game, pause and cursor gates right; D3D11 swapchains captured (system DXGI factory path, 5 hooks set
  aside at each new swapchain, no crash). Epic's overlay (`EOSOVH-Win64-Shipping.dll`) rewrites some of our imports
  (LoadLibrary*, DirectInput8Create left to it; XInputGetState patched again): harmless here. **Cause**: the game window
  never had the foreground: `focus: lost to brave.exe` from the first frame until the user clicked the game ~5 min
  later (`focus: the game window is in front`, `GyroLib enables the gyro` at once). Launched from the Epic launcher
  while the browser was in front, Windows' foreground lock kept the browser foreground behind the fullscreen game; the
  game reads the DualSense over HID, which works without focus, so it played normally. Our focus gate
  (`host::game_has_focus`, foreground window's process) keeps the gyro, the stick fallback and the panel key off.
  Workaround: click the game window or Alt+Tab to it once. Not fixed in code.
- 2026-10-24 **Metro: Last Light Redux crashes at start (not Gyro4All)**: 4 runs, `0xC0000005` at `metro.exe+0x9420F8`
  on a game worker thread (started at `metro.exe+0x829a0d`; no Gyro4All frame on the stack), ~0.3 s after the second
  swapchain: a file lookup (`metro.exe+0x65df90`) returns null and the code reads the fd from it for `_lseek`
  (import at `metro.exe+0xa21e68`). Cause: the intro videos renamed to skip them (`intro.ogv-off`, `legal.ogv-off`);
  this build doesn't tolerate missing startup videos. Names restored (user's OK).
- 2026-10-24 **Metro: Last Light Redux: the gyro turned the main menu's camera** (user report; DualSense USB). The main
  menu is a live scene: the view's global and angles are readable (pitch 0, yaw 87.7 at start), the game clock runs and
  no cursor shows, so the module took it for gameplay. Same classes as in a level (global vtable `+0xB22D20`, view
  owner `+0xA81F08`; both objects are new per level), so the class can't tell them apart.
  - Snapshot diff (read-only, ReadProcessMemory; scripts not kept): `.data` of the exe, the global's object (0x400) and
    the view owner (0x2000), 4-6 samples each in main menu -> level -> level (alt-tabbed) -> main menu again; kept the
    values constant in each state, equal in both menus and both level samples, and different between them. In `.data`:
    **`metro.exe+0xF4B838`, dword, 1 in the main menu, 0 in a level**; also `+0xF49424..0xF49434` (-1 menu, 0 level).
    The global's `+0x2E4` (13 menu / 0 level) and `.data +0xF44FCC` did not survive the second menu.
  - Code (Epic exe, not encrypted on disk, so read offline with pefile + capstone): set to 1 at `metro.exe+0x73C1AA`
    (`mov dword ptr [rip+flag],1; mov word ptr [rip+..],di; test rsi,rsi`, unique in .text), to 0 at `+0x73C0EB`,
    to esi at `+0x73AAAD`; read at `+0x591538`, `+0x5930C5`, `+0x593BBE`, `+0x5A5C52`, `+0x5AEB80`, `+0x5AEF9E`.
  - `foura_engine.cpp`: `kMenuCode` (the setter) finds the flag on the first frame (log `4a: main-menu flag at ...`);
    while it is 1 the module reports no gameplay view (`gyro off: no gameplay view (target main menu)`, log
    `4a: main menu` / `left the main menu`). Not found (e.g. another exe): logged, the main menu turns as before.
    Builds, 661 checks pass. Not checked in Metro 2033 Redux's exe yet.
  - Verified in game (user: "todo cuadra"; DualSense USB; log): `4a: main-menu flag at metro.exe+0xF4B838`, `4a: main
    menu` at start (gyro off, applied 0), `4a: left the main menu` on loading a level, gyro on in gameplay.
  - `docs/GAMES.md`: Metro: Last Light Redux row (Epic version; don't rename the intro videos), removed from the
    not-yet-supported list.
- 2026-10-24 **Build: Gyro4All 1.7.1** (version chosen by the user). `build/release/Gyro4All-v1.7.1.zip`. What's new since
  1.7.0: Metro: Last Light Redux support (Epic Games Store; the same 4A module and code signatures); the 4A module
  finds the game's main-menu flag by code signature and keeps the gyro off in the main menu (a live scene with its own
  camera); `docs/GAMES.md`: Last Light Redux row (don't rename the intro videos). Deployed to Last Light Redux.
- 2026-10-24 **Metro Exodus Enhanced Edition (Steam), 4A module with 1.7.0: no gyro, no stick fallback, panel OK** (user
  report; DualSense USB). `MetroExodus.exe` (TimeDateStamp 0x6a9046b0; `.bind` section, but `.text` is plain on disk,
  entropy 6.5), DX12 (system DXGI factory path, panel works). None of the three code signatures match (view global,
  clock, main-menu flag), and the module then took 2033's fixed offset `+0xFBD820` (wrong objects in Exodus): angles
  never readable, so no view, and since a global "existed" the blind stick fallback (only for `no_camera`) never ran.
  - Fix (`foura_engine.cpp`, `exe_is_metro()`): the fixed offsets (view global, clock object) only for `metro.exe`;
    otherwise `no camera hook (gyro as right stick)`. Built (661 checks pass), deployed to Exodus (as 1.7.1, no bump);
    not run in Exodus yet.
  - Camera search (read-only scans + write tests; scripts not kept): the only float `pitch, yaw, 0` blocks that followed
    turns were rows of the camera's 4x4 matrix (row 3 = forward, row 4 = position, 1.0), in a ring of ~10 copies in
    4 MB frame blocks; the game rewrites them within 50 ms (writing all of them at once too). Yaw from the forward row
    (atan2(fx, fz), grows turning right) tracked the turns; no float anywhere matched it in any form (radians +-2pi k,
    degrees, negated) across two turns, with or without the pitch beside it: the master is probably a vector or
    quaternion. Exodus pauses when not in front (writes stop), so tests must run with the game in front.
  - **Dead end / incident**: a cdb hardware write breakpoint on a matrix slot. First attach without `-pd`: no hit (the
    game was paused in the background), cdb killed by my timeout took the game with it (user had saved). With `-pd`
    the attach/detach is harmless; 100 s in the background and 60 s in front: no hit on 4 slots (slots probably moved
    after the resume). The game was gone afterwards (no crash event or dump).
  - Stick fallback verified in game (2026-10-24, DualSense USB): the log shows `no camera hook (gyro as right stick)`
    and the gyro turns the view through the game's right stick; the cursor menus stop it.
  - Camera search, second round (read-only; snapshots of the ~9 GB of RW memory, user turning in place on request):
    - The 4x4 "camera" ring of the first round is the player's **body**, not the view: its yaw follows turns, but
      its pitch stays near a fixed ~4 deg whatever the view pitch (looking 20 deg down, or at either limit).
    - No float or double anywhere holds the view yaw as an angle: whole-memory deltas equal to the turn (radians mod
      2pi or degrees mod 360, with sign) over two turns (76 and 137 deg) left only random matches; the same for
      values stored next to a pitch, for a non-wrapping yaw (any number of turns), and for (sin, cos) pairs.
    - Pitch limits: looking fully down gives a view forward 60 deg down (sin -0.866). No float or double mirrors
      between the up and down limits outside the per-frame 4 MB blocks.
    - The view rotation itself (3x4, eye position at height ~1.44) was found only in a typed value block owned by
      the NVIDIA user-mode driver (header pointer into `nvwgf2umx.dll`): the driver's copy, not the game's. No other
      copy of its axes (stride 1/2/4) or quaternion (any order or sign) exists in RW memory.
    - Next idea (not tried): start from the code instead, e.g. the uses of the game's stick/mouse sensitivity
      settings, to find what the turn input is added to.
- 2026-10-24 **Metro Exodus EE, camera found from the code** (offline disassembly of `MetroExodus.exe`, `.text` is
  plain on disk; scripts in the temp workdir, not kept).
  - **Ref-scanner bug (trust nothing before this):** the original `find_refs.py` computed instruction RVAs as
    `chunk_index + text_rva` but the chunk index is a *file* offset within `.text`, so every reported ref was off by
    `text_file` (0x400) and most were false positives. Correct: `insn_rva = i + text_rva`, `tgt = insn_rva + insn_len
    + disp`. **All camera/joy ref addresses from earlier notes are unreliable; re-derive with the fixed scanner.**
  - The real camera debug-print: string `"Camera. Position: {%f;%f;%f}, Angles: {%f;%f;0}"` at `.rdata+0x114e908`,
    referenced at `0x5850e9` (`lea rcx,[rip+0xbc9818]`), in function **`0x585050`**. It reads the camera state as
    **global floats in `.data`** and prints `yaw=atan2(P0,P2)`, `pitch=atan2(-P0,sqrt(P0^2+P1^2))`, both *57.29578
    (const at `.rdata+0x147c610`; atan2 helper `0x4df306`).
  - **Camera state = a 4x4 float matrix in `.data` at `0x17033d0..0x170340f`** (four 16-byte vectors): Position =
    `0x17033d0/d4/d8`, orientation vector (P0,P1,P2) = `0x17033e0/e4/e8`. This matches the earlier "the master is a
    vector" finding.
  - **Writers:** exactly 5 `movaps` (16-byte) writes, all in one cluster `0x62ccxx..0x62e7xx`: to `0x17033d0` at
    `0x62cd2a/0x62e1c8/0x62e74d`, to `0x17033e0` at `0x62ccac/0x62e1da`.
  - **Camera-update function `0x62cac0`:** takes the camera object in `rcx`, spins a lock on `[camera_obj]`, calls
    **`0x62ce30`** to build the view, then writes the result both to the `.data` matrix and to `[camera_obj+0x18..0x58]`
    (a 4x4 **double** matrix — the object's copy is what the renderer uses; `.data` is a debug copy).
  - **`0x62ce30` builds an orthonormal matrix** from `(rdx=position, r8=basisA, r9=basisB)`, all as **doubles** (3
    each): row1=r9, row2=r8 (both normalized), row0=cross(row1,row2), row3=(position,1). So the master orientation is
    the two basis vectors + position passed in as arguments.
  - **Caller of `0x62cac0`: `0x81f790`** (the only direct caller). It derives the position + two basis vectors from
    **`[actor+0x9d0]`**, a pointer to an object holding a **3x4 float matrix at `+0x50`** (col0->position, col1->basisA,
    col2->basisB; converted float->double on the stack before the call). It also reads two `(double,int)` pairs from
    `[actor+0x2348/0x2350]` and `[actor+0x2354/0x235c]` (passed as the 4th/5th args). The camera object (1st arg to
    `0x62cac0`) is a **two-level deref: `[*.data+0x1696ca0 + 0x10]`** (right before the call; a flag byte at
    `[cam+0x54]` skips the update when set). `0x81f790` is called directly at `0xa32b88` and `0xa33079`; in both, the
    `actor` (1st arg) is the result of a **virtual call at vtable offset `0x860`** on a manager object (a "get local
    player/camera" method).
  - **Key implication:** the `.data` matrix is **recomputed every frame from `[actor+0x9d0]`**, so writing the `.data`
    floats directly will not stick. The clean gyro hook is at the camera update: rotate the two basis vectors (or
    post-multiply the built matrix) before/inside `0x62cac0`/`0x62ce30`.
  - **Open question (needs in-game):** is `[actor+0x9d0]` the **view** (head) or the **body**? The debug print is named
    "Camera" and the earlier pitch-limit finding (full-down ~60 deg) suggests view, but the 4x4 ring in the frame
    blocks was the body. Must confirm in game which one tracks head yaw/pitch before hooking.
  - Next: read-only in-game dump of the `.data` matrix + the camera object's `[+0x18..0x58]` (user turns the head, see
    which tracks it), then implement the hook in the 4A module (Exodus-specific signatures for the matrix globals).
- 2026-10-25 **Metro Exodus EE: the view angles found and write-tested in game** (DualSense USB, stick fallback build
  running; game restarted since 2026-10-24, so addresses below are this run's, offsets are the exe's).
  - **The 4 MB "frame blocks" are worker-thread stacks**: a hardware write breakpoint on a body-matrix "ring" slot hit
    inside the stack probe (`__chkstk`, rsp right at the slot). Every matrix copy found there was a local variable;
    this is why all the earlier whole-memory searches (MODLOG 2026-10-24) kept landing on copies.
  - Breakpoint chain (cdb `-pd`, `ba w4`, one hit then `.detach`; the game survived all three):
    1. Static view matrix at `exe+0x30021e0` (inside a static camera object at `exe+0x3002190`), built at
       `exe+0xE66369` by `cross(fwd, up)` from three vectors passed in.
    2. Those come from `exe+0x30049b0` (pos/fwd/up), filled by a "set camera" routine (`rcx+0x2820/0x2830/0x2840`)
       from the engine's camera globals at `exe+0x17033d0` (pos, fwd at +0x10, up at +0x20; read all over the code).
    3. The globals are written by `exe+0x62cac0` (double-precision build via `0x62ce30`, converted to float), called
       from `exe+0x81f790` with float vectors from the camera object `[player+0x9d0]` (+0x50 pos, +0x60 fwd, +0x70 up).
  - The camera object `[player+0x9d0]` also holds `float pitch, yaw, roll` at `+0x14/+0x18/+0x1c` (matching its fwd
    exactly), but a write there is undone within 30 ms: it's rebuilt from the player.
  - **The player (`[[exe+0x1696ce0]+0x30]`) holds `float pitch, yaw, roll` at `+0x9e0`** (radians; pitch grows looking
    down, yaw is the heading atan2(fx, fz) and grows the same way as 2033's, roll 0), the same layout as Metro 2033
    Redux's `+0x650`. Copies of the yaw at `+0x9f0`, `+0xa08`, `+0x3d6c` follow it. **Write test**: yaw `+0x9e4`
    +0.35 rad and pitch `+0x9e0` +0.2 rad both stuck; every copy, the camera object's angles and its forward followed
    within one frame (30 ms) and stayed. Not yet checked on screen by the user.
  - Global signature (Exodus only, no match in either Redux exe): `48 8B 05 ?? ?? ?? ?? 48 8B DA 48 8B 48 30 48 85 C9`
    (`mov rax,[rip+g]; mov rbx,rdx; mov rcx,[rax+30h]; test rcx,rcx`), 3 matches, all `exe+0x1696ce0` (in `.data`).
  - Module (`foura_engine.cpp`): the Redux signature first, then the Exodus one with the angles at `+0x9e0`; the rest
    (apply, limit, recenter, follow) is the 2033 path. Built, 661 checks pass.
  - **First in-game run** (deployed as 1.7.1, no bump; DualSense USB): the log finds the global at
    `MetroExodus.exe+0x1696CE0`, angles at `+0x9e0`, `gyro on (target view angles)`, and the applied yaw/pitch add up
    while the view angles change with them. The player object moves on loads (`view angles gone` then a new heap
    address a frame later), handled by the per-frame path read. Exodus's yaw wraps at +-180 deg (unlike 2033's).
    The gyro stayed suspended by GyroLib (activation) until the user changed settings in the panel. The status line no
    longer shows a clock when there is none (Exodus).
  - The method is now a tool, `tools/camtrace/` (README with the steps), and AGENTS.md makes it the next step when an
    engine's own route doesn't find the camera. Checked live on Exodus: `dump` with the pointer path, `snap` +
    `vectors` (after a 68 deg turn the view's group ranks first: 32 copies, among them `exe+0x17033e0`, `exe+0x30049c0`,
    `exe+0x30021c0`), `write-test` on the yaw (kept, the forward turned 20 deg; a second one, +34 deg, the user saw on screen); `find-vec` only matches stack copies
    while the game runs (the view sways), so it's meant for the paused game; offline `refs` and `sig` reproduce the
    findings above. `bp exe+0x3002200` (the static view matrix) hit the same writer as by hand (`exe+0xe66369`, rbx = the camera
    object `exe+0x3002190`, sources `exe+0x30049b0/c0/d0` in rdx/rdi/r14), detached, and the game kept running.
  - **User verdict: works** (directions right, smooth, no trouble at the up/down limits). Added to `docs/GAMES.md`
    (Steam version) and removed from the to-be-tested list. Still not detected: the main menu and pause screens
    without a mouse cursor.
  - Still open for Exodus: its own pitch limits (the 2033 code clamps at +-90 deg; Exodus measured ~60 deg down in
    2026-10-24's driver-block reading), pause (no clock signature; the game pauses itself when not in front) and the
    main menu (no flag signature).
- 2026-10-25 **Build: Gyro4All 1.7.2** (version chosen by the user). `build/release/Gyro4All-v1.7.2.zip`. What's new since
  1.7.1: Metro Exodus Enhanced Edition support (Steam; 4A module, view angles found with `tools/camtrace/`); the
  camtrace camera-finding tool; README "Main Features" section; `docs/GAMES.md` fixes.
- 2026-10-26 **Mass Effect: Andromeda (EA app, Frostbite, 1.7.2, DualSense, Steam Input not running)**: first run.
  - Detected as Frostbite; the exe has an `.ooa` section (inline hooks). xinput1_4 import slot found in the protector's
    table (`exe+0x431e510`), IsXInputDevice check found from its `IG_` string and hooked, the game polls
    XInputGetState.
  - **Bug: the XInput translation flipped every second** ("shown as XInput controller 0" / "not shown: a real XInput
    controller is connected"), so the game never kept the controller. Cause: this system's `xinput1_3.dll` is
    GameInput's `xinputforwarder` (its PDB path says so; it imports no driver calls and loads `xinput1_4.dll`), not the
    DirectX redistributable one. The watcher's "real" XInputGetState is xinput1_3's trampoline, which then calls the
    hooked xinput1_4 export from inside xinput1_3, not passed through by `passes_through`, so it got the translated
    controller and counted it as a real one while it was shown. Fix: `passes_through` also lets xinput1_3's calls
    through (like xinput9_1_0's; its own exports are hooked already). Built, 674 checks pass; not yet run in game.
  - Types: `DataContainer TypeInfo at exe+0x48492B0`, "info data layout unknown", only 312 types (Dead Space has
    ~23,885): the type reader doesn't understand this Frostbite's TypeInfo yet. No camera model fits; the stick fallback
    runs (blind, default stick model).
  - `TypeDump` was `*` by default, so every Frostbite game wrote `Gyro4All-types.txt` on every start, supported ones too.
    Now unset by default: the dump is written only when no camera model fits; `TypeDump=<filter>` or `*` writes it
    always, `0` never. INIs created by 1.7.x keep `TypeDump=*` (remove it to get the new default).
- 2026-10-26 **Build: Gyro4All 1.7.3** (version chosen by the user). `build/release/Gyro4All-v1.7.3.zip`. What's new since
  1.7.2: the XInput translation no longer flips every second on systems whose `xinput1_3.dll` is GameInput's forwarder
  to xinput1_4 (protected exes: Mass Effect: Andromeda); Frostbite's `Gyro4All-types.txt` is only written when no
  camera model fits (or with `TypeDump=`). Deployed to Mass Effect: Andromeda. A run with the pre-bump build (same code)
  kept the DualSense shown as XInput for the whole session (no more flipping).
  - Mass Effect: Andromeda's exe on disk is encrypted (EA's OOA: `.ooa`/`.ooadk` sections; `typeinfo` and `fieldinf`
    sections hold random bytes in the file), so its TypeInfos can only be read in the running game.
  - **2017 info data layout** (from the 312 raw info data of `Gyro4All-types.txt`): the 2013 (BF4) one with the array
    type's TypeInfo inserted at +0x18: name +0x00, flags/size +0x08 (kind = flags >> 4 & 0x1F: class 0x35, array
    0x41, enum 0xC089, struct 0x29), module +0x10 (`Core`-like record), array type +0x18, alignment/field count +0x20,
    then classes: parent +0x28 (DataContainer: itself), fields +0x30 (ATActionGraph: 1 field, pointer into `fieldinf`),
    then functions; arrays: element +0x28, info data 0x38 bytes; enums: values +0x28 (ATComparator: 6); structs
    (ATName, 2 fields): +0x28/+0x30 code, +0x38 `.data`, +0x40 a pointer into `fieldinf`, so the fields are taken from
    the first of +0x40, +0x38, +0x30, +0x28 whose records read. Arrays have their own names (`ATActionGraph-Array`).
    TypeInfo: info +0x0, next +0x8 (as 2013). Field records assumed to be 2013's (0x18 bytes: name, flags, offset,
    type), not yet seen. `fb_types.cpp`: layout 2017, checked on DataContainer after 2013 (`shaped_2017`), and the
    2013 backward search for newer types. Built, 674 checks pass; deployed (1.7.3, no bump), not yet run.
  - **In game: works.** `info data layout 2017`, **23,345 types** (312 from DataContainer on, 23,033 newer) in ~0.1 s,
    with fields (27,687 field lines); the 2013 field record (0x18 bytes) is right. Camera models: Dead Space's
    `HybridRotationInputChimeraBehavior` is missing; Battlefield's `ClientSoldierEntity` exists but its aim update
    signature isn't in the exe ("found 0 times"). The stick fallback runs (gyro as right stick, blind).
  - Camera system: `ContactCamera::` (Cinebot controllers: `InputController` with `InputControllerAxesStrategy`
    (Yaw/PitchAxisInputData, SpeedRatio), `SplineAxesController` (size 0x860, Yaw/Pitch/Roll spline axes),
    `RotationController`, `FreeController`, `RecenterController`, `CollisionController`; `GameClient::ClientPlayerCameraEntity`
    (0x110). Runtime classes have no reflected fields, only their `*Data`. Input goes through **InputTree**: enum
    `InputTreeShared::InputTreeIntent` with `Intent_LookPitch = 4`, `Intent_LookYaw = 5` (and `InputTreeInput`
    `Input_LookYaw = 35`, `Input_LookPitch = 36`): the counterpart of Dead Space's EIAYaw/EIAPitch action levels.
  - **Dead end so far: a value search for the look levels** (read-only, ~10 GB of writable memory outside thread
    stacks, phases guided by beeps: right stick right / released / left, twice): 812k -> 485 floats that were > 0.3,
    ~0, < -0.3 in step; only 4 were symmetric and repeatable (`|R1-R2|`, `|L1-L2|` < 5 %, `|R+L|` < 10 %), and a second
    pass (stick up / released / down, reading each one's neighbours at -4 and +4 for `Intent_LookPitch`) showed none
    of them, nor their neighbours, following the stick: the best (+1/0/-1 exactly) sits in a block of +-1 values that
    looks like a rotation matrix. Either the levels don't persist between ticks (Dead Space's did, at state+0x500), or
    they aren't floats in [-1, 1] next to each other. Next: the view itself (camtrace `snap`/`vectors`).
  - camtrace `snap --heap` (10 GB each) + `vectors` after a ~80 deg turn: 957k turned unit vectors, dominated by the
    character's bones (3x4 matrices with ~1 m translations follow the aim, head/spine), none in the exe. Reading the
    candidates live minutes later for a tilt test failed: the heap had been reused (stale addresses). What worked: a
    **live search for world transforms** (`camxform.py`, scratch): every 16-byte aligned LinearTransform (3 orthonormal
    Vec4 rows + a translation over 5 units) in writable memory (~30 s), then turn ~100 deg (keep the ones whose axes
    turned in yaw), then tilt up ~30 deg (keep the ones that also pitched with their heading kept): the character
    turns but doesn't pitch, the camera does both. Result: dozens of copies of one transform at world position
    (-9.50, -11.01, -7.53), row 2 (forward, Frostbite's +z) heading 100 -> -0.2 -> -0.9, pitch -1.9 -> -32.3 (looking up
    makes it negative: the transform's z points back), **two of them in the exe's static data, `exe+0x4315820` and
    `exe+0x42ea630`** (rows right / up / forward / translation; `exe+0x4315810` holds a heap pointer just before,
    `exe+0x42ea5e0` and `+0x42ea680` floats ~532-537 near it).
  - `write-test exe+0x4315840 --yaw 20` (the forward row; user had saved): undone within 30 ms (the read back already
    showed the old heading): **a copy the game rewrites every frame**. Next: `bp` on it to see the writer, if the exe
    has no anti-debugging (sections: only EA's `.ooa`/`.ooadk`, no Denuvo-style sections seen; not confirmed).
  - **Breakpoints (cdb `-pd`, one hit each; the game kept running through all of them):**
    1. `exe+0x4315840` is written by `exe+0x16e78d0..` (a function at `exe+0x16e7890`): copies 4 rows from
       `rcx = [[[arg2+0x30]+8]+0]` (heap, here `0x3882b720`) to `exe+0x4315820`. That source holds the same view, with FOV
       1.0472 rad and aspect 1.7778 at -0x40/-0x30; write test on it: undone too.
    2. The source's forward row is written by `exe+0x35f7d0` (render view `setTransform`: copies `[rdx]` to `[rcx]`,
       sets `+0x158`, then builds the frustum planes), called by `exe+0x15173aa` with `rcx = view+0x130`, `rdx =
       view+0x40` (view object `0x3882b5f0`; projection floats at `+0xf0..+0x100`, flags `+0x11a..+0x120`).
    3. `view+0x40`'s first writer each frame is a reset: `exe+0x1282350` builds a default view (`exe+0x1515ba0`) and
       copies it (`exe+0x1515e00` -> `exe+0x1515cf0`, write at `exe+0x1515d62`) into three views (`+0`, `+0x520`,
       `+0xa40`). Skipping writes whose `[rsp+0x40]` is `exe+0x1282379` (`bpcond.py`, scratch): the real writer is
       `exe+0x1516944` (in `exe+0x15168c0`), called from `exe+0x14fb552` (in `exe+0x14fb000`, which also copies a
       0x90-byte camera parameter block to `view+0xf90..+0x1010`), whose 6th argument is the transform: from
       `exe+0x14bb891`, `rbx+0xd0` (params at `rbx+0x40`), `rbx` walking a list (`[rbx+0x18]` next, head at
       `rbp+0x1d8`). Execution breakpoints at `exe+0x14bb86d` twice: `rbx` = `0x374bcd80` / `0x375bcd80`, i.e. on the
       render job threads' stacks: the view requests are copied per frame, so the renderer side ends here.
  - Next: the camera's angles (Cinebot computes the transform from yaw/pitch every frame): a live value search for
    floats equal to the yaw/pitch read from `exe+0x4315820` (radians or degrees, both signs, 0/90/180 deg offsets),
    narrowed over several turns (`anglescan.py`, scratch).
  - **Angle search: no yaw.** 4.0M first matches (16 s), 3.4M after three turns (yaw 100 -> -32 -> 115 -> -8): those
    are the constants matching the pitch variants (pitch stayed 0, so 0/90/180 deg and pi/2 values everywhere);
    every yaw-variant candidate was gone. So the yaw isn't a plain float in radians or degrees wrapped to +-180 (maybe
    unwrapped, relative to the character, a spline parameter or a quaternion). The tilt (to pitch -60.0, apparently the
    camera's limit: forward y = -0.866) left one float, `60.0` next to a direction (0, -0.866, -0.5): likely the limit
    or a copy, not checked.
  - Next idea: the camera objects themselves. The type reader gives the TypeInfo of `ContactCamera::SplineAxesController`
    (0x860 bytes), `InputControllerAxesStrategy` (0x108), `RotationController` (0x510); their getType (`find_gettype`)
    leads to their vtables, and instances are the heap objects whose first qword is that vtable. Diffing one live
    instance while turning shows its yaw/pitch fields (whatever their form).
  - **Vtables and live instances** (`vtables.py`, scratch: getType = `lea rax,[rip+type]; ret` in `.text`, vtable = the
    `.rdata` qword pointing to it, instances = heap qwords equal to the vtable): `SplineAxesController` getType
    `exe+0xbacaf0`, vtable `exe+0x2e4b3b8`, **no instance**; `InputControllerAxesStrategy` `exe+0xbc3cf0`, vtables
    `exe+0x2e19b50` / `exe+0x2e514d0`, 4 hits; `InputController` `exe+0xbaa340` / `exe+0x2e4b7d8`, 2; `RotationController`
    and `FreeController`: none; `ClientPlayerCameraEntity` `exe+0x11aecb0` / `exe+0x2edea48`: none. Over all 147
    ContactCamera/Cinebot runtime classes, the live controllers come in pairs: `InputController`, **`TrackableController`**
    (orbits a trackable: the player), `SpringController`, `CollisionController`, `ShakeController`, `DummyCoreController`,
    `ActionSpawnerController`, `TargetingTargetAdhesionController`, `DebugInfo/DebugModeHandlerController`, 7
    `RelativePosDumperController`; plus 2 `CinebotCamera`, 2 `CinebotCameraMode`, 2 `ClientCinebotCameraEntity`, 1
    `CinebotTransitionBlend`, 26 `ClientCinebotCameraDirectorTrack`, 36 `DofTrack`, 5 `TrackableEntity`.
  - **The camera's angles: `InputControllerAxesStrategy` `+0xC0` (yaw) / `+0xC4` (pitch), float degrees** (`objdiff.py`,
    scratch: the live controllers read at rest, after two turns and a tilt, against the static view copy). Instance
    `0x123fce0d0` (one of the four with its vtable): true yaw 100.0 -> 44.2 -> 121.8 -> 122.1, `+0xC0` 0 -> 304.17 ->
    21.78 -> 22.02 (the stick's accumulated yaw, wrapped to 0..360, relative to a reference heading: -55.8 and +77.6
    exactly); true pitch 0 -> -30.8 on the tilt, `+0xC4` 0 -> 30.81 (inverted). The other three instances didn't
    change. `CinebotCamera` (`0x137ce0f70`) holds the resulting axes and position (+0x10.., +0x40.., copies at +0x50..),
    `SpringController` +0x200.. the same.
  - `write-test 0x123fce190 --add 20` (later, at rest): it read 0.0 before the write (it had held 22.02 eight seconds
    after the last turn, so it goes back to 0 at some point while idle) and 0.0 again 30 ms after: rewritten every
    frame, the view didn't move (static copy unchanged). So `+0xC0`/`+0xC4` are the strategy's output (accumulated
    input of the current move?), not the camera's stored angles. Next: a write breakpoint on it: the code that
    writes it reads the stick and feeds the camera, the place to add the gyro (like Battlefield 4's aim update).
  - **Breakpoint on `+0xC0`: written by `exe+0xbc458b`** (`mov [r12],rax`, 8 bytes: `+0xC0` and `+0xC4` together) in
    `exe+0xbc44e0`, the strategy's per-frame input read (rcx = r14 = the strategy `0x123fce0d0`). It takes `rbx =
    [strategy+0xa0]` (an input wrapper), calls `exe+0xbb86d0(rbx)` and then the wrapper's vtable slot `+0x30`
    (`call [rax+30h]` with out pointers `rsp+0xc0`, `rsp+0xb0`, `rsp+0x30`), and stores the two floats it returns as
    a pair at `[arg6+0x10]` (arg6 = `strategy+0xb0` or `+0xc8`, chosen by the flag `strategy+0x100`, in the caller
    `exe+0xbc4440`, which calls it once more with `[strategy+0xe0]`/`+0xe8` when set). Then `xmm6 = [arg5+0x38]`
    (a speed?), and `[strategy+0xa8]`'s vtable `+0x18` divided by it. Called from `exe+0xbc4229` <- `exe+0xbaa315`
    (near `InputController`'s getType `exe+0xbaa340`) <- `exe+0xbb871c`. So `+0xC0`/`+0xC4` are what the input
    wrapper reports this frame (in degrees: the odometer of the move, back to 0 once idle), the stick's way into the
    Cinebot camera. Hook candidates: the wrapper's slot `+0x30` or `exe+0xbc44e0`, adding the gyro's degrees to the
    pair it stores. Not yet worked out: what consumes the pair, and whether an added amount turns the camera once.
  - **What the pair is** (sampled at 50 Hz, read-only, stick right then released): `+0xC0` is the camera's **absolute
    yaw relative to the character**, degrees, wrapping 360 -> 0 (356.2 -> 302.0 -> ... -> 128.7 while the true yaw
    went 99.5 -> 48.3 -> ... -> -131.3; a constant offset plus a spring lag of ~6 deg), `+0xC4` the pitch the same way
    (up positive). Idle a few seconds, the camera recenters behind Ryder and the pair goes back to 0 (why every scan
    started at true yaw 100.0, the character's heading). Nothing else near the strategy holds the value.
  - **The pair is written twice per frame**: breakpoint skipping `exe+0xbc458b` (camtrace `bp --skip`): the second
    writer is `exe+0x5e8076`/`+0x5e809e`, the **odometer update `exe+0x5e7f60`** (vtable `exe+0x2d73a18` slot 3 at
    `exe+0x2d73a30`, the only `.rdata` qword pointing to it; object `[strategy+0xb8]`), called at `exe+0xbc46ec` with
    (odometer, Vec2 axes by value in rdx, Vec2 speeds by value in r8, float dt in xmm3, float* pair as the 5th
    argument): it gets the rates from `exe+0x5ef380` and does `pair.yaw += rate.x * [odo+0x10] * scale * dt`,
    `pair.pitch += rate.y * [odo+0x14] * scale * dt`. So each frame: `exe+0xbc44e0` loads the camera's current angles
    into the pair (from the input wrapper), the odometer adds the stick's turn, the camera follows the pair.
  - **Camera model `mea_camera.cpp`**: the odometer update found by its prologue (`4C 89 44 24 18 48 89 54 24 10 57 48
    83 EC 50 48 8B 05`, once in `.text`) checked against its accumulation at +0xF9 / +0x120; the vtable slots holding it
    (`find_in_image`) swapped for a hook that calls the game's and then adds the gyro's turn since the pair's last call
    (yaw subtracted: stick right lowers it; pitch added), after `sync_gyro_frame`; gate: an odometer update in the
    last 100 ms. Tried after Dead Space's and Battlefield's models. Built, 674 checks pass; not yet run.
  - **In game (user): works** on foot (log: `camera model Andromeda resolved (32 ms)`, slot `exe+0x2D73A30` hooked, one
    odometer/pair per camera mode, ~95 updates/s, the gyro off in menus). **Except the scanner** (d-pad down): the
    gyro's pitch is inverted there, yaw right.
  - Scanner, read-only: the live strategies (by vtable) are the same with the scanner open (data `0x11951a5d0`,
    `SpeedRatio` yaw 0.75 / pitch 0.85, both positive). Execution breakpoint on the odometer update (one hit): normal
    camera and scanner both hit odometer `0xf32c800` with speeds (0.675, 0.765), axes (0, 0) at rest, the same pair
    (pitch 339.9 normal, 5.5 in the scanner). So the scanner uses the same odometer and pair; the inversion is in what
    the scanner camera does with the pitch (or in the stick it feeds the odometer: not seen, the stick was at rest).
  - **Crash caused by the tooling**: a conditional execution breakpoint (skip odometer `0xf32c800`) never hit in 120 s;
    the script's `subprocess` timeout killed cdb while it was still attached with the hardware breakpoint set, and the
    game was gone a few seconds later (no crash dump). Never kill an attached cdb: let a breakpoint script detach on
    its own (cdb `.detach` after a timeout inside the script, or a breakpoint that is sure to hit).
  - User: in the scanner **the stick isn't inverted, only the gyro's pitch**. So the stick reaches the scanner camera
    with the opposite pitch sign to the pair's.
  - Scanner, read-only (`scannerdiff.py`, `scannerstrat.py`, `stratcompare.py`, scratch; instances of the 147
    ContactCamera/Cinebot classes by vtable, phases normal / scanner / normal / scanner): opening the scanner creates a
    camera mode of its own (`CinebotCameraMode`, `InputControllerAxesStrategy`, `InputController`, odometer,
    `RotationController`, `TrackableController`, `CollisionController`, `ShakeController`, ...); with the scanner open
    the `CinebotCamera`'s FOV goes 60 -> 45 (`+0x110`, `+0x948`). Both strategies share the same data
    (`InputControllerAxesStrategyData`: yaw/pitch `SpeedRatio` 0.75/0.85, `OdometerDefaultFOV` 50) and the same
    input wrappers (`InputAnalogWrapper`, vtable `exe+0x2e4c1c8`: concept 5 = `Intent_LookYaw`, 4 = `Intent_LookPitch`,
    dead zone 0.05). With the scanner open only one strategy's pair moves when turning (pair 90.0/356.1 -> 90.45); its
    fields against the idle one's are runtime values (pointers, -0.4, -0.72, 7.9), no clear mode flag. Not found yet:
    where the scanner's pitch inversion is applied.
  - **Fix: the pitch sign learned from the stick** (`mea_camera.cpp`): the hook reads the pair before and after the
    game's odometer; while the right stick shown to the game (`xinput::right_stick`, new) is pushed past 0.4 up or
    down and the odometer changed the pitch by more than 0.01 deg, the sign of that change per stick up is the pair's
    relation (3 frames in a row to (re)learn it, logged). The first relation learned is the reference (on foot); a
    pair with the opposite one gets the gyro's pitch inverted. Limits: a pair is only corrected once the stick moved
    vertically in it; with a real XInput controller (Xbox pad, Steam Input) nothing is learned (no translated stick).
    Built, 674 checks pass; not yet deployed (game running) nor tested.
  - In game: **the scanner still inverted**. The log learned one relation (on foot: `turns its pitch up with the stick
    up`) and none in any other pair (0 updates with the pitch inverted), so in the scanner the stick's vertical moves
    didn't change the hooked pair's pitch by 0.01 deg: the scanner probably takes the stick's pitch elsewhere (its own
    `RotationController`?) while following the pair inverted. Diagnostic build: about once a second per pair while
    the right stick is pushed, the odometer's yaw and pitch change on that pair is logged.
  - Diagnostic run: **the learning works**. On foot pair `0x1352D9AB0`: stick up 0.40 -> pitch +0.215 deg ("as is").
    Scanner pair `0x120EC45B0` (created with the others when the area loaded, kept all session): stick up 0.44 ->
    pitch -0.210, stick down 0.68 -> +0.935 ("the gyro's pitch inverted"); its yaw has the normal sign (stick right
    0.44 -> -0.226). The first run missed it because the vertical pushes stayed under the 0.4 threshold (0.40-0.46
    here): lowered to 0.25. **User: with the pitch learned, the scanner's gyro pitch is right.** (DualSense, the
    controller shown as XInput, Steam Input not running.)
  - But learning from the stick fails a player who aims with the gyro only (no vertical stick, nothing learned), and
    its "first relation learned is the reference" depended on the order (the scanner first would invert the gyro on
    foot later). **Replaced by learning from the view**: `find_view()` finds the static render view copy by the code
    that fills it (`0F 10 01 0F 29 05 rel32` / +10 `0F 10 49 10 0F 29 0D rel32` / +21 `0F 10 41 20 0F 29 05 rel32`,
    the three stores 0x10 apart; `exe+0x4315820` here), whose third row is the view's back axis (view pitch up =
    -asin(y)). Each pair's pitch input per update (the odometer's own change, i.e. the stick, plus the gyro's pitch
    the hook added) is smoothed (x0.8 per update, for the spring) and correlated with the view's tilt since the pair's
    last update (score x0.98 per update, tilts over 10 deg ignored as cuts); at +-2 deg^2 the pair's relation is set
    (logged), and a pair that tilts the view down gets the gyro's pitch inverted. Absolute, so the order of cameras
    and the game's invert-Y option don't matter; the stick or the gyro alone teach it. The `xinput::right_stick`
    accessor added for the stick version was removed again. Built, 674 checks pass; not yet run (the signature's
    uniqueness can only be checked in the running game).
  - First run with the gyro only: still inverted, because the view copy's code **matched 4 times** (learning off).
    Live: `exe+0xd5ce50`, `+0xd5d060`, `+0xd5d150` (copies to `exe+0x402e940/8d0/9b0`, after `48 6B C0 58 49 8D 88
    68 02 00 00 48 03 C8 74 43`) and ours `exe+0x16e78cd` -> `exe+0x4315820`, the only one right after `48 8B 0A 48
    8D 97` (`mov rcx,[rdx]; lea rdx,[rdi+disp32]`). That prefix (10 bytes before) is now checked too. Built, 674 checks
    pass; not yet deployed (game running).
  - **In game, gyro only (user): works**, the scanner's gyro pitch is right. Log: `render view copy at
    MassEffectAndromeda.exe+0x4315820`, then per pair `tilts the view up ... as is` (on foot and other modes) and
    `tilts the view down ... inverted` (the scanner's), 1293 of 4564 odometer updates with the pitch inverted. Some
    pair addresses changed relation later (0x126E6C050 up -> down after 72 s, 0x124A9F4B0 up -> down; 0x124A7B560
    down -> up -> down within 66 ms): strategies are freed and recreated per camera mode, so an address can come back
    as another camera, and a camera blend (opening/closing the scanner) can briefly read the wrong way; it settled
    right each time. (DualSense, shown as XInput, Steam Input not running.)

- 2026-10-27 **Warhammer 40,000: Space Marine 2 (Steam, Swarm Engine, 1.7.3 release; DualSense, the game's own HID
  reads; started by the user without EAC)**: first run, recon and a new `swarm` engine module.
  - Install: `client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe` (client, x64) plus
    `... - Retail.dll` (the same code built as a DLL: the in-process server of solo and hosted games; both show up in
    `ue: section` lines). Window title `Warhammer 40,000: Space Marine 2 CLIENT 15.2.PATCH.1454884/1455141`, **window
    class `Magnus`**. Data in zip `.pak`s under `client_pc\root\paks\client\default\` (`default_ssl.pak`: compiled SSL
    scripts, `*.sslbin`; `default_other.pak`: `.cls` actor classes and `.sso` objects). No RTTI names in the exe; the
    "Swarm" strings are the zombie-swarm ECS (`EcsComponentSwarm*`), not the engine's name.
  - Runtime: the game imports Sony's pad library (`HID.DLL`) and reads the DualSense itself (64-byte reports, buttons
    match SDL); DirectInput mouse and keyboard; D3D12 through NVIDIA Streamline (`sl.interposer.dll`); the Epic Online
    Services overlay (`EOSOVH-Win64-Shipping.dll`) rewrites the exe's `LoadLibrary*`, `DirectInput8Create` and
    `GetCursorPos` imports after Gyro4All (left to it). The game ships `msvcp140.dll` 14.34 (the user renamed it; the
    System32 14.51 one loads) and `vcruntime140*.dll` 14.34 (harmless).
  - **Why the F10 panel did nothing**: the DllMain thread pumps only the splash screen's window (a D3D11 swapchain),
    runs gyro frames for ~10 s, then stops pumping for good (`thread:0/0` in the pump report), while the `Magnus`
    window is pumped by another thread (~40 calls/s). Gyro frames (GyroLib, the panel's input, the status line) stopped
    with it. Fix in the core: `Engine::pump_window_class()`: the game thread is 0 until the first message for a
    top-level window of that class, then that thread (`dllmain.cpp`, `adopt_game_thread`).
  - Camera, from the editor's type descriptions (`client_pc\root\tools\meta_descs\meta_desc_ssl.ps`, shipped with the
    game): `GameCameraSystem.gameCamera` (`GameCameraPackage`: `tpsController` = `TpsCameraController`,
    `cameraInputController` = `GameCameraRotateController`, `inputSenseFovGamepad`/`inputSenseFovMouse` splines,
    `rotationSpeed`, `followTargetRotationCoef`); `GameCameraActionRotateController.rotateAction` is an `ActionVector`
    of the input system. Scripts: `ssl/camera/tps_camera_controller`, `game_camera_rotate_controller`,
    `game_camera_action_rotate_controller`. Exe strings: `TpsCameraController`, `gsTPS_CAMERA_CONTROLLER`,
    `sslOBJ<gsCAMERA>`, `GetGameCamera`, `GetCameraDirection`/`SetCameraDirection`, `MatrixWorldToCamera`. The
    meta_descs give designer data, not runtime offsets: the view's angles are still to be found in memory
    (`tools/camtrace/`).
  - `src/engines/swarm/`: detection by `..\..\paks\client\default\default_ssl.pak` from the exe's folder, window class
    `Magnus`, `pump_window_class`, no camera hook yet (`no_camera`: the gyro as right stick through the game's own
    controller reports), the mouse cursor as the only menu signal. Built (no warnings); not yet deployed (game running)
    nor tested in game.
  - **In game (user, DualSense, Steam Input not running): the panel works** (F10 and Create+Options; log: `thread ...
    pumps the game window (class Magnus) and becomes the game thread`, status lines all session). **The blind stick
    fallback turns nothing**: the deflection goes into the game's 64-byte HID reports (`stick fallback: stick (0.35,
    0.19)`, ~80 updates/s) and the camera doesn't move. Not investigated yet: the game may read the pad through
    another path too (a second handle, `HidD_GetInputReport`, raw input).
  - **Camera, camtrace (2026-10-27, exe of the 2026-10-26 update; Y is up)**. Every step below was a copy (write-test
    undone within a frame):
    - `vectors` (static data): the render view block at `exe+0x4e8ec40` (camera-to-world rows right/up/forward/position
      at `exe+0x4e8ec7c`, its inverse at `+0x40`; ~22 copies up to `exe+0x4e93...`). Written by `VCRUNTIME140!memcpy`
      (0xA48 bytes from a stack struct) called from `exe+0x1d76e90` (`rcx` = `exe+0x4e8ec40`), from the render-view setup
      `exe+0x1803710`, itself called through a vtable from `exe+0x1801045` (`rdx` = `lea [rip+..]` = `exe+0x4e8ec40`).
    - `exe+0x1803710` gets the render camera with `call exe+0x1350ee0` (ecx 0, edx = byte `[rcx+0x72b089]`): world =
      `exe+0x1dd4fe0(byte)` (a virtual call on `exe+0x4f7df50`, then `[table + (byte&3)*16]`), component list =
      `[[world+0x710] + dword[exe+0x4d55820]*16 + 8]`, camera = `[[list]]` (index 0). Render camera object (heap):
      +0x00 camera-to-world 4x4 (rows right, up, forward, position), +0x40 view matrix, +0x80 near 0.1 / far 250000,
      +0x144 render size. Break after the getter: `bp exe+0x18037a6 --exec` (rax).
    - Written by `exe+0x27a0e92` (matrix store) from `exe+0x22220a0` (sets the render camera: `rcx` = camera, `rdx`
      = position/matrix), called by the game's camera component update `exe+0x1373d50` (`rbx` = component,
      `[rbx+0x70]` = the render camera; `rdx` = an event whose first qword points to the source world matrix, times an
      attachment offset from `[rbx+0x48]`).
    - That event comes from the scene node's transform-changed dispatch `exe+0x1726170` (`rcx` = node; world matrix 4x4
      at node+0x98, copied to node+0xd8; listeners at `[node+0x10]` entries of 16 bytes, count `[node+0x18]`, each
      called through `vtable+8`). The node's world is computed in `exe+0x1726480`: parent = `[node+0x20]` (its world
      read by `exe+0x171d7d0`), local transform at node+0x58 (identity for the camera node: it follows its parent).
    - A write breakpoint on the node's world matrix ended with the game gone (no crash report; cdb exit 0x80000004),
      as the camtrace README warns. Heap addresses are per run.
  - **The view's angles found and write-tested (2026-10-27, second run of the same build).** Steps:
    - Camera node again with `bp exe+0x18037a6 --exec` (render camera = rax), then `bp exe+0x1373d50 --exec --cond
      "poi(@rcx+0x70) == <render camera>"` (the component of that camera; the node is `rsi` there). camtrace gained
      `bp --exec` (`ba e1`) and `--cond` for these. Execution breakpoints never took the game down.
    - `[node+0x20]` is not a node but an attachment link (vtable exe+0x3643390): its `vtable+0x48` (exe+0x18E5340)
      copies the world matrix of the node at `[[link+0xd0]]`+0x98. Rig: camera node (local identity) <- link <- **arm
      node** (local at +0x58: yaw relative to the root, pitch, and the shoulder offset (-0.92, 2.73, -3.10)) <- link <-
      **root node** (world: the character's yaw only, pitch 0; no parent). Write-tests on the root's world: undone.
    - Value search (scratch script, float32/float64 equal to the heading in degrees or radians, +-360, negated), the
      user turning the camera in place between scans: on the arm's relative yaw it left 7 values in 0x30-byte boxes
      of a list (animation/script parameters in degrees, slightly lagging: consumers); on the camera's world heading
      it left 3 heap floats (plus one on a stack): one next to HUD-like numbers (1220, 400, 200: the compass?) and
      **the third-person camera controller's**.
    - **Controller** (vtable exe+0x362B450, one instance; +0x08 = the arm node; a `main_camera` string at +0x1C8):
      `float pitch` at **+0x130** (degrees, grows looking down) and `float yaw` at **+0x134** (degrees, the view's
      heading `atan2(x, z)`, Y up, in (-180, 180]; it shrinks turning right: the first turn "right" took the heading
      from -33 to -94). Write-tests kept and the view followed: yaw +20 twice (-158.7 -> -138.7, -55.5 -> -35.5, user:
      "se ha movido") and pitch +5 (forward y -0.198 -> -0.283, looking further down).
    - Findable on every run: the constructor (exe+0x13A1C54) stores the vtable: `C7 41 18 01 00 00 00 48 8D 05 ?? ??
      ?? ?? 33 DB 48 89 01 48 89 59 08 48 8B F9 C7 41 10 03 00 00 00` (disp at +10, end +14), unique, -> exe+0x362B450.
      Other references to the vtable: exe+0x13A1FB6, exe+0x1669305 (destructors). A heap scan for the vtable qword
      finds the single controller (3 s from Python, out of process).
  - `swarm` module, second version: finds the vtable by that signature, the controller by a background heap scan (again
    5 s after it goes away), writes `pitch -= gyro pitch`, `yaw = wrap180(yaw - gyro yaw)`; `view_angles` for the stick
    fallback; menus by the mouse cursor only. Built (no warnings); not yet deployed (game running) nor tested in game.
  - **In game (user, DualSense): the gyro turns the camera, both axes the right way.** Log: `swarm: camera controller
    vtable ...exe+0x362B450`, `camera controller at ... (1 found; 6.75 GB scanned in 469 ms)` 4 min after start (in a
    level), `gone` and found again (same address) 45 s later. **Bug (user): the pitch limits weren't respected.** The
    log shows `stick fallback: the game undid the gyro's camera writes 8 frames in a row` several times for a few
    seconds each: the game clamps the controller's pitch to the camera mode's limits, the core took the clamped writes
    for a camera it can't write and switched to the stick fallback, which turns nothing here.
    - The limits are per camera mode, in the presets (`CamBase.angleSettings`: `maxConstrainRotationY` = down,
      `minConstrainRotationY` = up, degrees; `ssl/camera/cam_modes/mgs/*.sso` in `default_other.pak`): idle 75 down /
      70 up, aim_minizoom 89/70, sprint 50/50, melee 75/70, last_stand 30/30, target_lock up 89. The controller itself
      holds no limits (+0x00..+0x138 dumped).
    - Fix: no `view_angles` (no stick fallback for this engine); `limit_turn` and `apply` clamp the pitch to -70..75
      (cam_idle), widened to wherever the game's own stick takes it (aiming down to 89). Built; not yet tested.
  - **Bug (user): with GyroLib's panel open, the d-pad navigating it also reaches the game** (the mouse doesn't). The
    log says the panel idles the game's reports (`made idle ... 11996 made idle so far` in 24 s), so the game gets
    the d-pad another way, the same way the stick fallback's deflection never reached it. The exe imports
    `CreateFileA` as well as `CreateFileW` (and ReadFile, GetOverlappedResult(Ex), WaitFor*, CancelIo(Ex); no raw
    input, no XInput, DirectInput for mouse and keyboard only); nativepad only hooked `CreateFileW`, so a DualSense
    handle opened through `CreateFileA` was never followed. Added `hk_create_file_a` (HID paths go through
    `hk_create_file` as Unicode; logs `the game opens HID devices through CreateFileA too; followed` once). A
    hypothesis until that line shows up; built, 674 checks pass, not yet tested.
  - **In game (user): both fixed.** Log: `nativepad: the game opens HID devices through CreateFileA too; followed`;
    the panel idles the reports again and the d-pad no longer reaches the game; `pitch=... (limits -70..75)`, no
    `stick fallback` lines; the gyro stops at the pitch limits.
- 2026-10-27 **Build: Gyro4All 1.8.0** (version chosen by the user). `build/release/Gyro4All-v1.8.0.zip`. What's new since
  1.7.3: the Swarm Engine module (Warhammer 40,000: Space Marine 2: camera controller pitch/yaw, heap scan, pitch limits);
  `Engine::pump_window_class` (the game thread is the one that pumps the game window's messages); nativepad follows
  HID opens through `CreateFileA` too; camtrace `bp --exec` / `--cond`. Docs: GAMES.md and ADVANCED.md (Space Marine 2).

## Status summary (2026-10-08)
- Pacific Drive: **done** (on foot + car profile, in-game panel, calibration). Install:
  `games/PacificDrive/deploy-gyroue.sh "<Win64 dir>"`; uninstall with `remove`. UE4SS is parked as `dwmapi.dll.off`.
- Returnal (UE 4.25, modular): **done**, generic view with no profile (v0.3.2), user-verified in combat.
- Cronos: The New Dawn (UE 5.4.4): **works** with a profile (raw double rotators in the pawn's `View` component),
  user-verified with 0.4.0. Covers UE5 double rotators.
- Cronos 0.4.5: calibration in menus (no cursor) and the F10 panel with HDR/RenoDX are both user-verified.
- Next: redeploy 0.4.5 to Pacific Drive/Returnal and smoke-test (render worker, menu detection, view keys).
- Life is Strange Remastered (UE 4, UProperty objects, DX11): **works** with the probe-found profile (+OnGyro for the
  walking camera), user-verified 2026-10-09. First DX11 game and first pre-4.25 property system.
- Gears of War: E-Day (UE 5.6, Xbox/GDK build, Themida): **works** with the generic view (no profile), user-verified with
  0.5.5 as `version.dll` (2026-10-09). Needs inline hooks, the System32 DXGI factory vtable and the heap scan for
  GUObjectArray. Install with `PROXY=version.dll bash tools/deploy-gyroue.sh "<...>\Binaries\WinGDK"`.

## Ideas for later
- XInput translation extras (0.7.0 is alpha with the default mapping): split touchpad (left half View, right half
  Menu, via `SDL_GetGamepadTouchpadFinger`), extra buttons (Edge paddles, mic, Capture) copying a standard button
  from the ini. Not keyboard keys: UE would switch to keyboard/mouse prompts.

- **Auto-probe with closed-loop check** (discussed 2026-10-09, parked): find the camera's real rotation state without a
  manual probe. Candidates = float/double rotators in objects reachable from Pawn/PC/Camera that equal the POV; drop
  per-frame output copies; write a small delta (~0.5 deg) to each and keep the one that turns the camera next frame.
  Only in stable gameplay. Risks: writing to look-alike non-rotation data, cameras that aren't stored rotators
  (quaternions, input accumulators, animation-driven). Would cover Cronos-like cameras, not ADS/vehicle/menu signals.
  Worth it only if more custom-camera games show up.
- Heap scan as the universal GUObjectArray finder: works for any build, but the data-section scan stays first (instant
  in normal games); the heap scan remains the fallback.

- 2026-10-11 **GyroLib 1.3.1: upstream burst delivery replaces GyroUE's pacer.** GyroLib
  commit 9addfd9 answers our report (GyroLib issue #1). SHA256SUMS verified for the SDK, the demo and `gyrolib.dll`
  6841434…; ABI 1, INI schema 0.2.0 and headers unchanged apart from a comment. Also fixes GyroLib issue #2
  (INI `gyro.acceleration=0` with stale fast sensitivities stays Off).
  - **How GyroLib does it** (`src/detail/delivery.hpp`, `MotionDelivery`, per endpoint, inside `gl_update` after the
    camera/cursor angles and before trackball/flick): a sample batch counts as a burst when consecutive samples are
    >= 2 ms apart in sensor time but arrive < 1 ms apart; three consecutive bursts separated by updates with no
    samples latch pacing until the source restarts. Each update's integrated angle is then spread linearly over a
    window (peak-hold of the gap between batches, x0.98 per batch, 15-60 ms), starting at the next update; time-based,
    so a second update at the same timestamp outputs nothing. Pending angle is dropped on gates, calibration,
    view/target/profile/modifier changes and stalls > 100 ms. SDL source only (not Steam), no INI setting, flick stick
    and trackball not buffered. Synthetic tests only; no real Switch Pro test upstream.
  - **Compared with GyroUE 0.6.17** (queue per sample weighted by gyro speed, played at the measured sample rate,
    holding `gap_mean + 0.5σ` capped by PacingMaxMs, statistical on/off every frame, switchable). Synthetic replay of a
    Switch Pro BT stream modelled on the Luto trace (5.24 ms sensor, bunched arrivals, 60/85x2/144 fps): both cut empty
    frames from 42-75% to 0.3-2.6% and speed CV from 1.0-1.9 to 0.31-0.36 (GyroUE) / 0.38-0.44 (GyroLib; ideal
    0.24-0.44), and both add about 25 ms over the raw output. GyroLib's window sat at a median 39-42 ms.
    Risks to check on hardware: the latch never releases (a DualSense over BT whose reports bunch could get a
    permanent >= 15 ms window), and Steam-provider motion isn't paced.
  - **GyroUE change (no version bump yet):** the Pacer, `Pacing` and `PacingMaxMs` are removed (both pacers stacked
    would delay twice and re-bunch); old keys in existing `GyroUE.ini` files are ignored. `MotionTrace` stays (raw
    and applied columns are now equal). New status line from the sample observer and `gl_gyro_state.new_samples`:
    `motion N samples/s in M arrivals/s | camera updates U/s, T% turned, P% of those without a new sample (GyroLib
    burst delivery)`. P > 0 while turning means GyroLib is pacing; it must stay ~0 for a DualSense.
  - **Luto test (user), Switch Pro over Bluetooth, Steam Input off:** works well with GyroLib 1.3.1 and no GyroUE
    pacer (tested with a test build of this change; an existing `Pacing=1` line in GyroUE.ini is ignored).
  - **Still to test:** Switch Pro over USB, DualSense over USB and BT (the status-line P% must stay ~0, see the latch
    risk above), Steam Input on, and a `MotionTrace=1` capture for `tools/motion_trace_report.py`.

- 2026-10-13 **Jedi: Fallen Order (Steam, UE 4.21, exe `SwGame-Win64-Shipping.exe`, UE4SS also installed), GyroUE 0.8.1:
  engine not readable.** GUObjectArray found (`ObjObjects` at exe+0x3875CE0, stride 0x18, ~26.8k objects) but no
  `FNamePool` line ever, `view=none` all session and the 60 s "engine data can't be read" notice. Root cause: the pool
  only exists from UE 4.23; 4.21/4.22 keep names in **`TNameEntryArray` (GNames)**: `Chunks[128]` of
  `FNameEntry*[16384]`, then `NumElements`, `NumChunks`; `FNameEntry` = `Index` (name index << 1 | wide), `HashNext`,
  characters (NUL-terminated) at +0x10. Not the Hogwarts issue (there a false pool was taken; here there is none).
  - Fix (`ue.cpp`, `find_legacy_names`/`legacy_entry_string`): when the pool isn't found and the object array is,
    look in the exe's writable sections for GNames (a pointer to the table, or the table itself): entries 0/1 must read
    `None` and `ByteProperty` at the same text offset (tried 0x10, 0x0C, 0x08, 0x14, 0x18), and NumElements/NumChunks
    must be consistent. Logs `ue: no FNamePool; GNames (TNameEntryArray, ...)`. Properties are `UProperty` objects
    there, already handled (`g_field_class_name`, as for Life is Strange Remastered).
  - Built (no version bump), **not tested in game yet**: the reflection layout detection on 4.21 is the next unknown.
  - **Tested (user, DualSense BT): engine readable.** `GNames ... characters at entry +0x10`, layout `UProperty
    objects, UE <= 4.24` SuperStruct=0x30 ChildProperties=0x38 Field.Class=0x10 Field.Next=0x28 Field.Name=0x18
    ElementSize=0x34 Offset_Internal=0x44 sizeof(Property)=0x70; PC `BP_SwPlayerController_C` (ControlRotation +0x3C8),
    pawn `BP_Hero_C`, camera manager **`RsCameraManager`**, view target the pawn. The gyro wrote ControlRotation (296
    degrees) and the camera didn't move. Probe (8 captures): ControlRotation overwritten (output),
    `PC.TransformComponent0` RelativeRotation and raw 0x260/0x280 kept without effect,
    `Camera.ModeOutputs.Rotation` (offset from the POV) and `CameraCachePrivate.POV.Rotation` "not testable: nested in
    a struct". Orbit radius ~380.
  - **Read-only memory hunt** (scratch script, PC/pawn/camera manager + two pointer levels, 2308 blocks, 12 rests
    between stick turns; also unit quaternions and 2D direction vectors): only exact copies of the POV
    (ControlRotation, camera manager +0x19FC/+0x1F6C = POV caches, TransformComponent0, view matrices, a native
    struct pointed to by PC+0x6D0 at +0x174). The hunt read only 0x2000 bytes of the camera manager, which missed
    ModeOutputs.
  - Reflection of `RsCameraManager`: `ActiveCameraMode` +0x2600 (`RsCameraMode`: ModeName, MotionParms, Aliases,
    Behaviors; no state of its own), `ModeInputs` +0x2610 (AnchorLocation/AnchorRotation, ControllerInput +0x2704,
    MouseInput, ...), **`ModeOutputs` +0x2740** (Location +0x2740, **Rotation +0x274C**, LocationOffsetCS (arm, about
    -450 X), RotationOffsetWS +0x278C (pitch -4: POV = ModeOutputs.Rotation + this)). The unreflected region after
    `ActiveSplineCameraModeName` (+0x2D18..+0x3498) holds no copy of the camera angles.
  - **Write proof (`WriteProcessMemory`, user agreed):** +30 yaw at ModeOutputs.Rotation: camera +30.00 and stayed
    3 s, camera position moved 227 units (expected chord for a 450 arm: 233, so it orbits); +10 pitch: +10.00, moved
    78.8 (expected 78.4); both undone cleanly. The PC+0x6D0 native struct: no effect, reset by the game.
  - Profile `profiles/SwGame-Win64-Shipping.ini`: `Object=Camera`, `Offsets=0x274C`, `RotatorSize=12` (keeps
    `DualSenseAsDualShock4=1`). Deployed with Probe=0; needs a game restart and a gyro test. Probe gap: nested struct
    rotators are never live-tested, although here the nested one was the answer (`ModeOutputs.Rotation`); the live test
    could write them as raw offsets (owner offset + nested offset).
  - **Dotted `Property=` and nested probe candidates (user's request; built, no version bump, not tested in game
    yet).** `find_prop` resolves `Struct.Member` (any depth) through StructProperty types, offset = sum of the
    offsets, so every caller (views, live test, `read_view_rotation`, `apply`) takes nested rotators and a profile can
    name one by reflection instead of a build-specific raw offset (user's choice over raw `Offsets=`, safer for other
    games). The live test no longer rejects nested properties ("nested in a struct" removed; it was untestable only
    because `prop_of` didn't resolve dots), and the passive pick no longer skips them. Reflected candidates on the camera
    manager are a passive "camera output" only inside the engine's `PlayerCameraManager` part (same rule as raw
    offsets): `CameraCachePrivate.POV.Rotation` stays an output, `ModeOutputs.Rotation` (+0x274C) doesn't.
    Profile now `Object=Camera`, `Property=ModeOutputs.Rotation`. To verify: the gyro with this profile, then Jedi with
    the profile set aside and Probe=1 (the probe should verify `Camera.ModeOutputs.Rotation` by itself).
  - **Probe test (user, Jedi, DualSense BT, profile set aside, Probe=1, 3 captures): the probe verified
    `Camera.ModeOutputs.Rotation` live and wrote it as the game's profile** (`Property=ModeOutputs.Rotation`, same as
    the hand-made one; the existing `[GyroUE] DualSenseAsDualShock4=1` kept). POV.Rotation and ControlRotation
    rejected as overwritten, TransformComponent0 as no effect. The user then restarted with Probe=1 still set and saw no
    gyro: the probe pauses the gyro (`applied yaw=0.0` all session), so the profile wasn't exercised. Probe=0 set; gyro
    test pending. UX gap: after a VERIFIED LIVE profile, a restart with Probe=1 silently keeps the gyro paused.
  - **Gyro test (user, DualSense BT, probe-written profile, Probe=0): works.** `camera: confirmed, the camera follows
    the gyro (ModeOutputs.Rotation, view 'Camera')`; applied yaw/pitch move in gameplay, gyro off in menus/loads
    (`view: none`). Repo profile marked verified. Not checked yet: combat, cutscenes, other camera modes (splines).
  - **Probe UX (user's request; built, no version bump, not tested in game yet):** after a verified live result
    (`finish_tests`, game profile or `.suggested.ini`), `disable_probe()` writes `Probe=0` to GyroUE.ini (and to the
    game's profile if it sets Probe itself) and logs `probe: Probe=0 written ...`. The notice now says the camera was
    found and verified, to RESTART THE GAME so the gyro turns the camera, and that the probe is off (or to set Probe=0
    by hand if the write failed). Texts that asked to set Probe=0 by hand updated (probe log header, GyroUE.ini
    default comment, profile header, ADVANCED.md).

- 2026-10-14 **Valor Mortis (Xbox/Game Pass GDK build, UE 5.7, exe `ValorMortisMSG-Win64-Shipping.exe`), GyroUE 0.8.1:
  engine not readable.** FNamePool found (exe+0xADF4ED0) but no GUObjectArray: data sections, XOR-encoded globals
  (none) and three heap scans (7.4 GB) found nothing; one candidate at exe+0xAEC2F90 whose header matched but whose
  items read null. The exe can't be read from disk (WindowsApps permissions).
  - **Read-only look at the running game:** the candidate is the real `ObjObjects` (Max 0x210000, Num 70868, MaxChunks
    0x21, NumChunks 2). Chunk pointers end in `...0008` and **each 0x18-byte `FUObjectItem` holds the Object pointer at
    +0x8** (item i at chunk + i*0x18 + 8: objects with InternalIndex 0, 1, 2... and vtables in the exe). UE 5.7 puts a
    64-bit field (flags and refcount merged) before the Object pointer.
  - Fix (`ue.cpp`): `object_array_stride` also tries the Object pointer at +8 (`g_item_object`, used by
    `object_at`); the log line says `object at +0x8`. Built (no version bump), not tested in game yet.
  - **Tested (user, DualSense, Probe=1): works.** `GUObjectArray ... object at +0x8, 7135 objects`; reflection layout
    unchanged (FField, SuperStruct=0x40 ChildProperties=0x50 Field.Next=0x18 Field.Name=0x20 Offset_Internal=0x44).
    PC `BP_PlayerController_C`, pawn `BP_PlayerCharacter_C` (first person; menu pawn `BP_MenuPawn_C`). The probe
    verified **`PC.ControlRotation`** (also turns the character), i.e. the generic view: the game needs no profile, only
    the 5.7 fix. Probe=0 written by itself; the gyro worked without a restart because the verified view was already the
    active one (the restart notice is misleading in that case). The written profile equals the default view.
  - PlayStation prompts: PCGamingWiki's "Controller platform" setting is the game's own option, absent from the
    Xbox/Game Pass build. Not an engine setting; dropped (a CommonUI `SetGamepadInputType` route was discussed, not
    pursued).

- 2026-10-14 **Valor Mortis crashes in the main menu (2 sessions, not resolved).** The game reports crashes through Sentry
  (crashpad), not UE's CrashReportClient: envelopes in `%LOCALAPPDATA%\ValorMortisMSG\.sentry-native\external\` hold
  the minidump (attachment `event.minidump`) and are deleted once sent, so copy them before restarting the game.
  - Dump (UE 5.7.3, GyroUE 0.8.2): access violation reading 0xFFFFFFFFFFFFFFFF at `exe+0x2EED0C0` on `RenderThread 0`;
    that thread's stack has only the game, nvwgf2umx and D3D12Core, no GyroUE/GyroLib. DLSS frame generation on
    (`ActiveFrameGen=1`, `DLSSFGMode=17`), RTSS hooks loaded, GyroUE's DX12 panel hooks active (render worker idle).
  - Both crashes came less than 0.5 s after `gyro off: mouse cursor shown` in the main menu, both with
    NativeFlickStick=1 (in the second, native suppression had just worked: 888 reports changed). Not isolated: the
    proposed A/B (GyroUE without panel hooks via empty `ui.menu_key` and `ui.gamepad_menu_shortcut=0`; without GyroUE;
    without frame generation) was declined for now.

- 2026-10-14 **Build: GyroUE 0.8.2** (version chosen by the user), `build/release/GyroUE-v0.8.2.zip`. Since 0.8.1:
  UE 4.22-and-older name table (GNames/TNameEntryArray); UE 5.7 object array items (Object pointer at +8);
  `Property=Struct.Member` resolved by reflection and nested rotators tested live by the probe (camera manager
  candidates are outputs only inside the engine's part); the probe sets `Probe=0` by itself after a verified camera
  and its notice asks to restart; Jedi: Fallen Order profile (`Object=Camera`, `Property=ModeOutputs.Rotation`).
  Tested in game: Jedi: Fallen Order (probe + gyro, DualSense BT), Valor Mortis (UE 5.7, probe + gyro, DualSense).

- 2026-10-14 **Bug (user): GyroLib's panel (F10 or the pad shortcut) navigated with the pad's d-pad also drove the
  game behind it.** Only the controller shown as XInput was neutral while the panel was open (`xinput.cpp`,
  `gl_panel_open`). Valor Mortis and Jedi: Fallen Order read the DualSense themselves (Sony's library, HID reports
  through `nativepad.cpp`), which passed through untouched; a real XInput pad (Xbox, Steam Input, DS4Windows) did too.
  - Fix: `host.cpp` `panel_has_pad`: true while the panel is open and, after it closes, until every SDL button is
    released and both triggers are up (the close press doesn't reach the game). Passed per frame to
    `xinput::set_neutral` (our slot reads neutral; real slots read connected with a zeroed `Gamepad`) and
    `nativepad::set_neutral`: every Sony report the game reads with a known layout and a verified CRC is made idle
    (`idle_report`: sticks 0x80, triggers 0, buttons cleared with the d-pad hat at 8, touch points "no finger"; motion,
    sequence/counter and battery kept; Bluetooth CRC rewritten). DualSense layout for USB 0x01 and BT 0x31, DualShock 4
    layout for DS4 0x01/0x11, a DualSense's basic BT 0x01 and a DualSense shown as a DualShock 4. Doesn't need
    `NativeFlickStick` (DualSense handles are always followed; a real DualShock 4 only with `NativeFlickStick=1`).
    Log: `nativepad: the game's controller reports made idle (GyroLib's panel has the controller)` / `passed through
    again`. Not covered: games reading pads through Raw Input/DirectInput, and the press that opens the panel with the
    pad shortcut (it reaches the game before GyroLib opens the panel).
  - Built (no version bump), not tested in game yet.

- 2026-10-15 **Ported from the GyroUE repository (commit 5bc258b, "Support for gyro button long press and camera
  recenter button"): GyroLib long-press blocking and the camera recenter button** (user's request; built, no version
  bump, not tested in game yet). GyroUE's own 0.9.0 zip (same day) carries these on the pre-split code; GyroUE's
  journal entries are summarised here, adapted to the core/engine split.
  - Long-press blocking (GyroLib 1.3.1, `ADVANCED_MOTION.md`, "Blocking long-press actions"): with
    `context.N.gyro.activation` Hold (3) or Hold to disable (5), a single `context.N.activation.button` (1..32 = SDL
    ordinal + 1) and `context.N.activation.block_long_press=1`, `gl_filter_event` defers that button's press:
    release < 200 ms -> `GL_EMIT_TAP` (the host gives the game a short press), hold >= 200 ms -> blocked. Needs
    `GL_HOST_LONG_PRESS_BLOCKING`, declared only where the events are wired.
    - `core/longpress.cpp` (new, game thread, after `gl_update`): every SDL button of the selected pad through
      `gl_filter_event` (eligible = the active view's activation button), as the SDK's
      `examples/tps/controller_actions.hpp`. Result per button: `hide` (deferred or blocked; the activation button is
      held back from its very first report while blocking is on, because the game reads the pad on its own threads)
      and `tap` (80 ms press after `GL_EMIT_TAP`). Focus loss, the panel taking the pad (`panel_has_pad`) or a
      controller change finish held buttons with `native_hold=1` and drop pending taps.
      `host.suspend_long_press_blocking` stays 0 (the game's own hold interactions can't be seen).
    - Applied in `core/xinput.cpp` `read_pad` (our slot; SDL ordinals before the `XInputNintendoLabels` swap), in
      real XInput slots (`XInputGetState(Ex)`) when the selected pad is itself an XInput pad the game polls (Xbox
      type or Steam Input handle; packet number offset by a change counter so a tap reads as new input), and in
      `core/nativepad.cpp` reports of the selected Sony controller (button bytes from b+7 DualSense / b+4
      DualShock 4: hat + face, L1 R1 L2 R2 Create Options L3 R3, PS touchpad Mute (DualSense only); hat re-encoded;
      BT CRC rewritten), only after those bytes agree with SDL (>= 10 compared reads with a button down and >= 4x the
      misses; 30+ misses dominating -> never). Sony controllers the game reads are now always followed (was:
      DualSense always, DualShock 4 only with `NativeFlickStick=1`).
    - Capability: `xinput_shown || real XInput filter || native buttons verified`; log `long-press blocking available
      (...)` / `unavailable: ...`, `longpress: button N tapped/held`. Not covered: Raw Input/DirectInput games, Steam
      Input keyboard bindings.
  - Camera recenter (GyroLib 1.3.1, "Recenter camera"): `gl_set_recenter_step_callback` exposes
    `context.N.camera.recenter_button` (default 0 = Off) and `context.N.camera.recenter_duration_ms` (0..1000,
    0 = instant). The callback gets the fraction of the remaining pitch to remove; GyroLib already drops requests
    unfocused, paused, on a cursor view or in menus, and doesn't hide the button from the game.
    - Core (`core/host.cpp`): the callback is registered in `start_gyro` (so the main menu's panel shows the row),
      fractions combined (`1 - (1 - a)(1 - b)`) and applied after the gyro delta, not counted by the follow check.
      Support is asked from the engine when the view changes (new `Engine::recenter_support` -> Yes / NotYet / No):
      NotYet is retried each frame and never withdraws it (that would cancel a transition); No (or a NotYet after a
      Yes for a new view) withdraws it (NULL callback, hides the row). No gameplay view keeps the current state.
      Applied through the new `Engine::recenter(fraction)`. Log: `recenter: available in view N 'Label' (level pitch
      X)` / `unavailable in view N 'Label' (Recenter=0 in the profile | its rotation can't be read yet)`.
    - Unreal (`ue_engine.cpp`): supported while the view's rotation can be read (`read_view_rotation`: first raw
      offset, else `Property=`/`ControlRotation`); applied as `apply(view, delta, 0)` with
      `delta = wrap180(RecenterPitch - pitch) * fraction`, so raw offsets, `WorldRotation=1` and `OnGyro=` get it
      like a gyro delta. New per-view profile keys `RecenterPitch=` (default 0) and `Recenter=0`.
    - Unverified per profile: Pacific Drive car (`CameraControllerRotation_Target`, relative to the car), Cronos (raw
      View rotators), Jedi (`ModeOutputs.Rotation`), Hogwarts (`BehaviorStack` float rotators, may be an
      accumulator), Life is Strange (pitch read from the first offset, +0x33CC). To test: bind a button in the
      Recenter row, look up/down, press it (instant and with a duration); a game whose PlayerCameraManager clamps
      pitch (writes may land at 360 instead of 0); `Recenter=0` hides the row.
  - Docs: README current scope (long press, recenter rows; Visage and Valor Mortis in the games table), ADVANCED
    (panel paragraph, Valor Mortis row, log lines, `RecenterPitch=`/`Recenter=` in the profile format).
  - Verified: clean build (`tools/build-gyro4all.sh`). **Not tested in game.**

- 2026-10-15 **Ported from the GyroUE repository (commits e75bd96 and 84ab27b): sensitivity profiles ADS / ALT_ADS /
  OTHER, default view labelled "Exploration"** (user's request; built, no version bump, not tested in game yet).
  GyroUE released these, with the long-press/recenter work above, as its own 0.9.0 (`GyroUE-v0.9.0.zip`; tested in
  game there: not yet, Cronos has that build with ADS on LT Hold). 84ab27b only moved that build entry in GyroUE's
  journal.
  - GyroLib's panel can't bind a button to a view (a tab edits a view, never activates it; no host-defined
    settings), so the buttons live in `Gyro4All.ini` (`[Gyro4All]`, the game's profile overrides; legacy `[GyroUE]`
    read too): `<Key>ProfileButton=` and `<Key>ProfileMode=Hold|Toggle` for Key = Ads, AltAds, Other. All off by
    default (a new Gyro4All.ini writes them empty, `core/settings.cpp`): a profile needs both keys valid. Button
    names: Xbox or PlayStation (face buttons by position), d-pad, Back/Start, Touchpad, Misc, paddles, LT/RT
    (trigger > 8000).
  - Engine-neutral, in `core/host.cpp`: GyroLib views 1001 (ADS), 1002 (ALT_ADS, a second aim) and 1003 (OTHER),
    always registered after the engine's views (`register_sensitivity_profiles`). Disabled ones get the label
    "<label> - disabled" and a tab description saying which keys to set. Parents: ADS and OTHER inherit view 1;
    ALT_ADS inherits ADS. An engine view with one of these Ids turns that profile off (logged, no tab).
  - Only in the default view (id 1): `frame()` passes the profile's id to `g_input->update` instead of 1; the engine
    still turns the default view. Other views (vehicles) keep their own tab (LT = brake there). Buttons read from
    GyroLib's selected SDL pad after `gl_update`, applied on the next frame. Latest activated profile wins; Toggle
    turns off when gameplay is interrupted (not the default view, cursor, pause, no focus, panel has the pad), because
    the game's own aim state isn't read. Log: `settings: profile 'ADS' (view 1001) ...`, `profile: <label>`.
  - No in-game notice for disabled profiles: GyroLib 1.3.1 has no API for the panel's selected tab (tried and
    dropped in GyroUE, user's call).
  - Unreal: the default view's label is now "Exploration" (was "Camera"): `ue_engine.hpp`/`.cpp` default, the probe's
    profile suggestion (`ue.cpp`), the shipped profiles (Cronos, Hogwarts Legacy, Life is Strange, Jedi `SwGame`,
    Severed Steel `ThankYouVeryCool`) and ADVANCED's examples. A user profile with `Label=Camera` keeps that name.
  - Docs: README "Sensitivity profiles" section with the settings table.
  - Verified: clean build. **Not tested in game.**

- 2026-10-16 **Ported from the GyroUE repository (commit b3dac74, `ue.cpp` only): case-preserving FName pools
  (`WITH_CASE_PRESERVING_NAME` builds)** (user's request; built, no version bump, not tested in game with Gyro4All).
  Found in GyroUE on PAYDAY 3 (Steam, UE 5.x, `PAYDAY3-Win64-Shipping.exe`): GUObjectArray found but no FNamePool,
  `view=none` all session. In those builds each pool entry is `ComparisonId` (uint32, the entry's own index), then a
  header with bIsWide in bit 0 and Len in bits 1..15, then the characters, 4-byte aligned (offset in block =
  (index & 0xFFFF) * 4; block 0: `None` header at +4, chars at +6; `ByteProperty` chars at +0x12). FName is 12 bytes
  (ComparisonIndex, Number, DisplayIndex), so UObject::Outer is at +0x28 (Name 0x18; Class 0x10, InternalIndex 0x0C
  unchanged). GyroUE checked the reflection layout live there: SuperStruct 0x48, ChildProperties 0x58, FField Name
  0x20, ElementSize 0x34, Offset_Internal 0x44, all found by `detect_layout` unchanged.
  - `engines/unrealEngine/ue.cpp`: `find_name_pool` also accepts that block 0 (header 0x0008) and sets
    `g_name_case_preserving` and `g_obj_outer = 0x28`; `entry_string` decodes those entries; every Outer read
    (`outer_of`, `find_subobject*`, the probe's object paths, formerly `kObjOuter`) uses `g_obj_outer`. Log:
    `ue: FNamePool.Blocks at ... (header len case-preserving entries, Outer +0x28)`. The stock pool path is
    unchanged (a stock block 0 has the `ByteProperty` header at +6, never `None`).
  - Tested in GyroUE 0.9.1 (user, PAYDAY 3, DualSense, generic `ControlRotation` view; connection and Steam Input
    not reported). PAYDAY 3 is an always-online co-op game, outside the offline-only ground rule (user's request in
    GyroUE). Its README and ADVANCED game-table rows were added too (user's request), with the online-game warning.
  - Verified: clean build. **Not tested in game with Gyro4All.**

- 2026-10-16 **Ported from the GyroUE repository (commit 589c1b1, "Match=Camera switching fixes and in-game camera
  learning")** (user's request; built, unit tests pass (378 checks), no version bump, **not tested in game with
  Gyro4All**). GyroUE found and tested all of it in PAYDAY 3 (its journal, 2026-10-16):
  - The case: in PAYDAY 3 the gyro stopped after some in-game actions. The follow check caught it: `camera: stopped
    following the gyro (ControlRotation, view 'Exploration'; ...) - target CH_Star_C, camera manager
    BP_PlayerCameraManager_C`. The probe found **`Camera.@SBZClampCameraRotationModifier` raw +0x68 (double rotator)**:
    during those actions this camera modifier turns the camera and `ControlRotation` becomes an output (rewritten
    every frame). The view target doesn't change, so `ViewTargetClass` can't tell the two cameras apart. The modifier
    exists only during the action.
  - Tested in GyroUE (user, PAYDAY 3, DualSense; connection and Steam Input not reported): `Match=Camera` switching
    both ways ("worked perfectly"), AutoProbe end to end (stopped following -> probe -> 3 captures -> live test ->
    `[View.Auto2]` appended -> profile read again -> switching, no restart), fast switching with carry-over, and no
    freeze at the pitch limits after the yaw-only fix. Untested there too: the probe key and hot reload after a hand
    edit. Dead end recorded there: "stopped following" after 2 windows gave a false AutoProbe start; back to 3.
  - How it maps onto Gyro4All (GyroUE keeps all of this in its one `host.cpp`):
    - `core/camera_match.hpp` (new, header-only, engine-neutral): `CameraMatch`, the choice between the base view and
      its `Match=Camera` views: fast path (3 writes in a row put back by the game, judged on yaw, ~30-50 ms), slow path
      (camera ignores >= 2 deg of yaw over >= 0.2 s), missing object (at once), carry-over of the lost gyro (max 45 yaw /
      10 pitch), all ignored -> the view followed last, 2 s rest.
    - `engines/unrealEngine/ue_profile.cpp` (new): `View` moved there from `ue_engine.hpp`, plus `match_camera`;
      `load_views(path)` (duplicate ids rejected; Views= from `[Gyro4All]`, else `[GyroUE]`), `has_views`, `add_view`
      (appends `[View.Auto<id>]` with `Match=Camera`, `Parent=1` and a comment; writes `[Gyro4All] Views=` with the old
      list plus the new key, so a GyroUE profile keeps its views; ids skip `kSensitivityViewIds`), `same_rotation`.
    - `ue_engine.cpp`: `base_view` (ViewTargetClass view, else default) + `active_view` (CameraMatch over the base and
      the Match views for that view target). The rotation is read before the gyro write (`read_frame`) and after it
      (`apply`) for the fast path; `after_frame` runs `camera_match_frame` (switch logs, max 200; carry-over write)
      and the follow check. The `view:` line logs the base view only. Profile hot reload (`check_profile_file`, every
      1 s on the file's write time; views only, the `[Gyro4All]` settings still need a restart; ids 1001-1003 dropped
      on reload). The POV is read every frame (`g_need_pov` removed).
    - Follow check: keeps running after its verdict, judges yaw only, logs `camera: stopped following the gyro (...)`
      after 3 ignored 2 s windows in a row and `follows the gyro again`, with `camera_description` (view target class,
      camera manager class, `CameraStyle`: `value_string` now reads NameProperty). The first "ignores it" verdict or a
      "stopped following" starts AutoProbe.
    - `ue_probe.cpp`: `ProbeKey=` (default `Ctrl+F7`; F1..F24 with Ctrl/Shift/Alt) starts the probe during play or
      cancels any probe; `AutoProbe=1` (default) starts it from the follow check, max 3 per session, 30 s apart; an
      automatic probe keeps the gyro on until its live test, gives up after 45 s without a capture or when the camera
      follows again. A verified camera of a probe started during play, or of any probe in a game with a profile, is
      added with `add_view` and read again at once (`profile_changed`); only `Probe=1` in a game without a profile
      still writes `[View.Default]`. `ue::probe_reset` forgets earlier captures. Plain F7/F8 only (no modifiers).
      Pitch wraps (UE5 0..360) in captures, live test and the diff's matching (`ue.cpp` `follows`, `probe_diff`).
      Difference from GyroUE: a `ProbeKey=` that is present but empty means no key, as documented (GyroUE falls back
      to the default then).
    - Core: mouse look counts like the right stick (`host::mouse_moving`, from `WM_INPUT` in the `PeekMessageW` hook,
      `GetRawInputData`, never consumed; the engine's `look_input`). `Engine::views_changed` (new): the core
      registers new views with GyroLib and withdraws removed ones (`gl_unregister_gameplay_context`), and decides
      recenter support again. `EngineFrame::base_view_id` (new): the sensitivity profiles also apply to the
      `Match=Camera` views of the default view. Recenter support is logged once per view and state. Transition lines
      (held 0.5-3 s): `focus: ...` (with the foreground program and window class), `gyro: GyroLib enables/suspends the
      gyro`, `motion data available/lost`, `motion source N`, `the controller reads as lying still/moves`, `GyroLib's
      sample count restarted`.
    - `util.hpp` `split` trims tabs too (a hand-edited `Views=Clamp <tab>` lost its view in GyroUE).
    - Not ported 1:1: the carried-over turn isn't added to the core's `applied yaw/pitch` totals (it's written by the
      engine). The RE Engine module gets none of this (no views to switch, no probe).
  - Tests: `tests/profile_tests.cpp` (from GyroUE, adapted: `[Gyro4All]` sections, plus a GyroUE-profile case),
    target `gyro4all_tests` in `src/CMakeLists.txt` (links only `ue_profile.cpp`, `settings.cpp`, `log.cpp`), run by
    `tools/test-gyro4all.sh`. The package and deploy scripts take only `winmm.dll` from the build folder.
  - `profiles/PAYDAY3-Win64-Shipping.ini` (new, `[Gyro4All]`): default view + `[View.Clamp]` Id=2, `Match=Camera`,
    `Camera.@SBZClampCameraRotationModifier` +0x68, RotatorSize=24. Docs: ADVANCED (PAYDAY 3 row, log lines,
    "Several cameras for the same view target", "Learning a camera during play", profile example, follow-check text,
    build/test commands, layout), GAMES (PAYDAY 3 row), AGENTS (layout, test script).
  - To test with Gyro4All: PAYDAY 3 switching both ways and at the pitch limits; the probe key; AutoProbe adding a
    view; a hand edit of the profile applied without restart; mouse turns in probe captures; that games without
    `Match=Camera` views behave as before (the follow check now keeps running and may start AutoProbe).

## Dead ends

(none yet)

## 2026-10-25 Dead Island 2: camera splits yaw and pitch

- Dead Island 2 (UE4, `DeadIsland-Win64-Shipping`), first person: with the default view (`PC.ControlRotation`) the
  gyro turns pitch only; the game rewrites ControlRotation's yaw (probe live test: "overwritten (output)"). Yaw comes
  from the pawn's capsule `Pawn.CollisionCylinder` (DSCapsuleComponent), raw float rotators `0x200,0x220`: the probe
  (probe key, during play) learned it as a Match=Camera view, live test "yaw followed, pitch didn't move". With that
  view alone the gyro turns yaw only: the capsule's pitch doesn't reach the camera.
- New profile key `PitchView=<id>` (ue_profile, `apply` in ue_engine.cpp): the view turns yaw only, its pitch goes into
  that view's rotation. The probe now prefers a rotation that turned both axes; a yaw-only one is learned with
  `PitchView=1` (ControlRotation). Unit test `test_add_view_yaw_only`. Builds, 665 checks pass; not yet verified in game.
- In game (user, 2026-10-25): the learned capsule view turns yaw, but it is NOT the stick's first-person camera: it
  turns the character's body past the game's limits and shows the character model. Dead end as the gyro's yaw. The
  stick's yaw source is still unknown: the game rewrites `PC.ControlRotation` yaw (and `Pawn.@FirstPersonCamera`
  +0x3C/+0x884, `Pawn.@StackFeed` +0x3C, Pawn +0x15EC) every frame. Exe: plain sections (.text/.rdata/.data/...), no
  Denuvo string, so a camtrace hardware breakpoint on ControlRotation's yaw is the next step.
- camtrace (cdb hardware breakpoint on `PC.ControlRotation` yaw, PC+0x2C4; new `bp --skip base:size` ignores Gyro4All's
  own writes from winmm.dll; the hit check now needs the marker on its own line, since cdb echoes the command):
  the game's write is `AController::SetControlRotation` (writes at exe+0x369F370), called from the game's
  `APlayerController::UpdateRotation` override at exe+0x17C78F0. It reads `RotationInput` (PC+0x458, float rotator)
  and `GetControlRotation()` (vtable +0x660). For the DI2 character (pawn) it stores RotationInput at pawn+0x12EC and
  +0x12F8, then **sets the view's yaw to the pawn's RootComponent (capsule) world yaw** (component +0x200, the rotator
  cached from the quaternion at +0x1F0/+0x230). After that, the camera manager (PC+0x2F0, vtable +0x760 =
  ProcessViewRotation) and PC+0x7F0 run, then SetControlRotation (vtable +0x668) and the pawn's vtable +0xB88 with the
  delta. So a yaw written to ControlRotation is lost, and the stick's yaw turns the body through RotationInput.
- Test profile: `[View.Default] Property=RotationInput` (the gyro added to the stick's input every frame, at the
  PeekMessageW pump, before the tick). Waiting for an in-game check.
- In game: `Property=RotationInput` doesn't move the camera (the gyro wrote ~136 times/s). camtrace bp on RotationInput yaw
  (PC+0x45C): the player tick at exe+0x3A42620 (the caller of UpdateRotation) sets `RotationInput = ZeroRotator`
  at its start (exe+0x3A426E4, after a call to exe+0x3A42E20), then the input adds the stick, then UpdateRotation reads
  it, and it is zeroed again at the end (exe+0x3A42DD5, after vtable +0x438). A write from the PeekMessageW pump (before
  the tick) is always wiped: the gyro must be added inside the tick, between the input and UpdateRotation (a hook on
  the controller's UpdateRotation, exe+0x17C78F0 in this build), or go in as right-stick input (stick fallback).
- Reassessment of the yaw (2026-10-25). The pawn's vtable +0xB88 (exe+0x1775040, a FaceRotation(new, old)) gets the
  view's delta: it keeps pitch in pawn+0xDB4 (limits pawn+0x15D0/+0x15D4 = -90/90) and, when the yaw limit pawn+0x15CC is
  > 0, a clamped yaw in pawn+0xDB8 (pawn+0x15CC = -1 in normal play: no yaw limit). Then it calls
  SetActorRotation(RootComponent quat (+0x230) * delta) (exe+0x3467A80): the whole actor turns, children included. The
  capsule's +0x200/+0x220 are USceneComponent's rotation caches (cached quat at +0x1F0/+0x210, rotator at +0x200/+0x220),
  not its transform (ComponentToWorld, quat at +0x230). UpdateRotation recomputes the +0x200 rotator only when +0x1F0 !=
  +0x230, so a write to the cache alone turns the view's yaw but not the actor: the arms and mesh stay behind (the user saw
  the character model). The probe skipped `Pawn.CollisionCylinder.RelativeRotation` with WorldRotation=1 (it only offers
  WorldRotation for absolute-rotation components; the root's world rotation equals its relative one).
- Test profile: default view `Object=Pawn.CollisionCylinder Property=RelativeRotation WorldRotation=1 PitchView=2`, with
  view 2 = `PC.ControlRotation` (ViewTargetClass that never matches) for the pitch.
- In game: dead end. With `CollisionCylinder.RelativeRotation WorldRotation=1` the view's yaw turns, but the camera sits
  at the edge of the character model (the mesh is seen): writing the root's transform doesn't move its children
  (camera, arms, mesh), which only the engine's UpdateComponentToWorld does. While walking, the yaw stops: log
  `WorldRotation not applied to CollisionCylinder: its world rotation doesn't match RelativeRotation this frame`
  (CharacterMovement rewrites the transform) until the right stick turns the actor again. Every memory-only route for
  this yaw is used up (ControlRotation: rewritten from the capsule; capsule caches: no actor turn; capsule transform: no
  children, rewritten by movement; RotationInput: zeroed at the start of the tick). The yaw has to go through engine code:
  RotationInput added inside the tick (UpdateRotation), or AActor::K2_AddActorWorldRotation called through ProcessEvent.
- New: `Property=RotationInput` views (`src/engines/unrealEngine/ue_update_rotation.cpp`). The gyro is queued and added
  to `PC.RotationInput` inside the controller's UpdateRotation, hooked with MinHook. UpdateRotation is found by shape:
  the one PC vtable entry whose function reads [rcx + RotationInput offset (reflection)] before any call, jump, return
  or change of rcx (MinHook's hde64 length decoder; `ue_update_rotation_match.hpp`, unit-tested with hand-assembled
  bytes). No fixed addresses. Queued turns not taken within 250 ms are dropped. The view's rotation for the follow
  check, stick fallback and recenter is ControlRotation plus the queued turn. Dead Island 2 profile:
  `[View.Default] Property=RotationInput`. Builds, 672 checks pass; in-game test pending.
- First in-game run of the hook: no gyro at all (`applied yaw=0.0` while the gyro turned ~140 times/s, no
  `update rotation:` line). `RotationInput` isn't a reflected property in UE4 (a plain member of APlayerController), so
  the lookup failed, and the earlier `Property=RotationInput` test without the hook wasn't writing either. Dump=1 of the
  PC: `InputYawScale` FloatProperty @0x464 (2.5), `InputPitchScale` @0x468 (1.75); RotationInput = 0x464 - 12 = 0x458, as
  the trace found. Fix: `update_rotation::rotation_input`, a reflected RotationInput if there is one, else InputYawScale
  (or UE5's InputYawScale_DEPRECATED) minus ControlRotation's rotator size, logged once per class.
- Second run: RotationInput found at +0x458 (before InputYawScale), but 4 PC vtable entries matched the shape:
  [344] exe+0x17C78F0 (UpdateRotation) and [369]-[371] exe+0x17A4D90/0x17A52C0/0x17A4DE0, Dead Island 2's
  AddPitchInput/AddYawInput/AddRollInput overrides (each reads one component, +0x458/+0x45C/+0x460, calls the engine's,
  then adds the change to +0x934/+0x938/+0x93C). Not unique, so not hooked. The shape now needs reads of two different
  components before the first call (UpdateRotation copies the whole rotator). 674 checks pass.
- Third run: WORKS in game (user, DualSense; connection and Steam Input state not recorded): UpdateRotation hooked at
  vtable[344] exe+0x17C78F0, yaw and pitch turn like the stick, the body follows, walking keeps yaw. One problem: in a
  cinematic the gyro still turned the camera sideways, while the stick doesn't: the hook added to RotationInput past the
  engine's look input gate (AddYawInput/AddPitchInput check IsLookInputIgnored). bCinematicMode isn't reflected in this
  build. Fix: the detour asks the controller's IsLookInputIgnored() before adding the gyro, and drops it while true. The
  virtual's slot comes from the reflected UFunction `IsLookInputIgnored`: its native thunk (the one code pointer in the
  UFunction) does `mov reg, [rcx]; call [reg + slot]` (`first_virtual_call`, unit-tested). `ue::find_function` added.
  678 checks pass; the cinematic test is pending.
- The cinematic: the IsLookInputIgnored gate was found (vtable[231], exe+0x3687CD0, from its thunk at exe+0x3CA1090)
  but never fired during the scene: DI2 doesn't block look input that way there. Reverted (code, tests, docs).
  Dumps (Dump=1 of PC, pawn, camera manager, its CutsceneComponent, pawn CharacterMovement) in the scene vs gameplay:
  the only state difference is `Pawn.CharacterMovement.MovementMode` (ByteProperty @0x1A4): 5 (MOVE_Flying) in the
  scene, 1 (MOVE_Walking) in play. `BP_Base_Player_C.IsInActiveCutscene` (@0x1A08) is false in both, and the
  CutsceneComponent (DIPlayerCameraManagerCutsceneComponent) is inactive in both: not a "cutscene" for the game.
- Built in (Unreal module, `read_scene`, not a profile option): the scene counts as paused while the pawn's reflected
  `IsInActiveCutscene` is true or its `CharacterMovement.MovementMode` is 5 (MOVE_Flying); logged on change (`scene:`).
  Risk: a game whose normal play flies the character (jetpack, flight) would lose the gyro there. 674 checks pass;
  in-game test pending.
- Dead Island 2 VERIFIED in game (user, 2026-10-25, Epic version; pad DualSense, connection and Steam Input state not
  recorded): yaw and pitch through the UpdateRotation hook, the body follows, walking keeps yaw, the gyro stops in the
  scripted scene (MOVE_Flying) and works again after it. Profile shipped: `profiles/DeadIsland-Win64-Shipping.ini`
  (`[View.Default] Property=RotationInput`). GAMES.md row added.

- 2026-10-25 **Bug (user): Dead Island 2, with GyroLib's panel open the game's mouse pointer moved behind it and the
  d-pad still drove the game.** Log: no `nativepad:` lines at all, and `the import of KERNEL32.dll!LoadLibrary* was
  written back by something else (now EOSOVH-Win64-Shipping.dll+...); left to it`: Epic's overlay owns the exe's
  LoadLibrary imports, so `hk_load_*` never saw the delay-load of `libScePad.dll` and its CreateFileW/ReadFile imports
  were never patched (no idle reports while the panel is open). The mouse: `WM_INPUT`/`WM_MOUSE*` were consumed, but
  Unreal's Slate polls `GetCursorPos` every frame in menus.
  - Fix: `hooks.cpp` watches `libScePad.dll` loads (`watch_dll`, ntdll loader notification) and runs `patch_all` from
    a new thread (starts after the loader lock is released). `overlay.cpp` hooks `GetCursorPos` in the game modules
    (import-table mode only): while the panel captures the mouse it returns the position from when it took it.
  - Built (674 checks pass, no version bump), not tested in game yet.

- 2026-10-26 **Dishonored: Definitive Edition (Epic, UE3, 1.7.3 dev build; DualSense, connection not recorded, Steam
  running, Steam Input state not recorded): XInput translation and the gyro camera work (user, in game).**
  - **XInput: the game never polled the translated controller.** `xinput: ... shown as XInput controller 0` and the
    notice appeared, but a new 10 s diagnostic (`game polls per slot a/b/c/d (n answered with controller k)`, plus the
    buttons pressed) read 0/0/0/0. The exe (static read) imports `XINPUT1_3.dll` by ordinal 2/3 (`jmp [rip+slot]` thunk
    at exe+0xC7817C) and calls XInputGetState in two places: the joystick enumeration (exe+0x7891E0, a static function:
    XInputGetState on slots 0..3, then each connected slot not yet listed gets an entry of type 3 in a 0x1F4-byte
    joystick array) and the per-frame poll, which only polls listed entries. The enumeration runs when the viewport is
    created (ctor exe+0x78BF40), from a DirectInput setup function (exe+0x78FE40, vtable slot) and from the deferred
    message handler's WM_DEVICECHANGE case (switch at exe+0x78F0D8, no wParam check). The game's first check came
    1.5 s before Gyro4All showed the pad (2 s grace), and neither Gyro4All's posted WM_DEVICECHANGE nor one posted by
    hand to `LaunchUnrealUWindowsClient` ever re-ran it (still 0 polls). Dead end: WM_DEVICECHANGE for this build.
  - Fix: new `Engine::controllers_changed()` (core, game thread, right after the WM_DEVICECHANGE when the controller is
    shown or withdrawn; `xinput::Settings::controllers_changed`). `Unreal3` implements it in `ue3_xinput.cpp`: the
    exe's XInputGetState import slot (by name, or ordinal 2 of xinput1_3/1_4) -> `FF 25` thunks / `FF 15` calls -> `E8`
    calls to the thunks -> the call followed within 0x30 bytes by `cmp r32,4` and a backward `jl`, in a function
    starting at most 0x100 bytes before it; called with a null `this` under SEH. Log: `ue3: controller list:
    enumeration at Dishonored.exe+0x7891E0 (found from 2 calls of XInputGetState, 16 ms)`, then `the game looked for
    controllers again`; slot 0 polled ~1300 times per 10 s after it. Buttons and sticks work in menus and play.
  - **Reflection** (`tools/unrealEngine3/ue3tool.py find`, after letting it try 4-aligned UObject fields): GObjects
    exe+0x17F4630 (Index +0x8), UObject packed to 4 bytes: **Outer +0x3C, Name +0x44, Class +0x4C**; names are UE3's
    **GNames** (exe+0x17F4270, TArray<FNameEntry*>, text at entry +0x14, **entries at any byte**, e.g. 0x4194FF9 for
    "Class"); UProperty Offset +0x88, ElementSize +0x68, ArrayDim +0x64; UBoolProperty mask +0xA4; UStruct.SuperField
    +0x74. Engine.GamePlayers +0x67C, Player.Actor +0x64, Actor.Rotation +0x100, Controller.Pawn +0x2BC,
    Actor.WorldInfo +0x1A0. Scene: DishonoredEngine -> DisLocalPlayer -> DishonoredPlayerController,
    DishonoredPlayerPawn, DishonoredPlayerCamera.
  - **No WorldInfo.Pauser**; the pause menu sets `WorldInfo.Paused` (+0x36C mask 0x100; checked open and closed).
    `bIsMenuLevel` (+0x36C mask 0x20000) stays on in gameplay (persistent level `DishonoredGameFull_P`), so it can't
    tell menus from play here.
  - **The camera follows `DishonoredPlayerController.Rotation`** (write test in free play, with screenshots: pitch +20
    up, -20 down, yaw +30 right; kept). In the scripted intro scene (letterbox, Emily) the game rewrites it every frame.
    Pitch reads 0..65535 in play, negative in that scene.
  - `Unreal3` generalised (no per-game offsets): the Class field found in 4-byte steps (the class Class points to
    itself, the first 64 objects' classes are classes), Name/Outer the two 8-byte fields before; names through BioWare's
    chunks or GNames (text at +0x10/0x14/0x18/0x0C, entry pointers at any byte), whichever reads "Class" and
    identifiers; SuperField (PlayerController -> Controller) and the bool mask field (WorldInfo's bools sharing an
    offset with distinct single bits) found; Pauser optional; paused also while `WorldInfo.Paused`. New profile key
    `[Unreal3] MenuLevel=0` ignores bIsMenuLevel; `profiles/Dishonored.ini` sets it. Two failed runs on the way: name
    entries required 8-, then 4-byte alignment. Log in play: `ue3: GObjects at Dishonored.exe+0x17F4630 (65604
    objects), UObject Outer +0x3c Name +0x44 Class +0x4c, GNames at Dishonored.exe+0x17F4270 (text +0x14), property
    offset field +0x88, SuperField +0x74, bool mask +0xa4, 8 ms`.
  - **User verdict: works.** Gyro off in the pause menu (`gyro off: game paused`). Still open: the main menu has a
    player controller and pawn, so the gyro is "on" there (its writes are undone: `stick fallback: the game undid the
    gyro's camera writes`, then `the stick turns nothing`); a few moments in play also had their writes undone and
    went to the stick fallback (learnt about axial, curve 2.25-2.5, yaw ~100-150 deg/s), cause not looked at.
    Mass Effect LE not re-tested with the generalised module.
  - `docs/GAMES.md`: Dishonored moved to the supported games.
- 2026-10-26 **Bug (user): Mass Effect LE 1 lost its camera hook after the Unreal3 generalisation.** Log: `ue3: name
  table not found (the class Class at heap ..., its name value 0x336b1, 16 sample names)`, then the blind stick
  fallback. The table at exe+0x16A2090 was right (chunk 0 in `.rdata`, chunk 1 on the heap, "Class" read fine), but 3
  of the 16 sample objects were named `<uninitialized>`, which isn't an identifier, and every sample had to be one
  (the old code and `ue3tool.py find` had the same rule and failed the same way on this run: which objects exist
  depends on when the search runs). Fix: the name table counts when at least 3/4 of the samples are identifiers
  (module and tool). Also the bool-mask search now only needs distinct bits among bools sharing an offset (at least
  two must share one), so a WorldInfo bool in another word can't hide it. Deployed to ME1/ME2/ME3; **user: ME1 and
  Dishonored work** (ME1 log: `name chunks at MassEffect1.exe+0x16A2090 (text +0xc), property offset field +0x94,
  SuperField +0x60, bool mask +0xd0`; `WorldInfo.Pauser +0x644`, no `Paused`). ME2/ME3 not re-tested.
- 2026-10-27 **Crysis (2007, EA app, 1.2.1 build 6156, CryEngine 2, 64-bit): new `CryEngine2` engine module
  (`src/engines/cryEngine2/`). Built, 674 checks pass, no version bump; not installed or tested in game yet.**
  - Install layout: `Bin64\Crysis64.exe` (x64, 53 KB, not wrapped) is the launcher that loads `CrySystem.dll`; the EA
    app starts `Bin64\Crysis.exe`, a 32-bit wrapper (scrambled section names), which runs `crysis64.exe` (`Crysis.ini`).
    `Bin32\Crysis.exe` has an `.ooa` section. PunkBuster (`Pb\`) is multiplayer only. No ASLR: fixed image bases
    (CryGame 0x39000000, CryAction 0x30500000, CrySystem 0x36500000, CryInput 0x33500000).
  - Loading: `Crysis64.exe` imports neither winmm nor version, but `CrySystem.dll` imports both (`timeGetTime`,
    `GetFileVersionInfoA`), so `Bin64\winmm.dll` loads with it on the main thread. Message pump: `PeekMessageA`
    (CrySystem, CryGame, the renderers): module prefix `cry`. `CryInput.dll` imports `xinput1_3` by ordinal 2/3/4 and
    `dinput8`. `Editor.exe` and `CrysisDedicatedServer.exe` are `is_launcher` (plain proxy).
  - **MSVC 2005 RTTI** is in the DLLs (Complete Object Locator signature 0, no self RVA). Vtables in CryGame.dll:
    `CPlayer` +0x360A98 (186 slots), `CPlayerInput` +0x3614A8 (13: 0 dtor, 1 PreUpdate +0x66530, 2 Update,
    3 PostUpdate, 4 OnAction, 7 Reset, 8 DisableXI), `CPlayerMovementController` +0x3620E8 (slot 0 RequestMovement
    +0x793B0), `CGame` +0x3668C8. CryInput.dll: `CXInputDevice` +0xEF78, `CMouse` +0xE838.
  - `CPlayerInput`: +0x28 CPlayer*, +0x30 stats, +0x38/+0x3C actions, +0x40 deltaMovement, +0x4C XInput
    deltaMovement, **+0x64 mouse deltaRotation** (Ang3; PreUpdate multiplies it by the sensitivity, `CPlayer+0x978` and
    zoom factors), **+0x70 XInput deltaRotation** (times frame time and the stick scale), +0x84 DisableXI. PreUpdate sums
    both into a `CMovementRequest` on its stack (rsp+0x50: flags, bit 0x40 = delta rotation at +0x60, Ang3 radians) and
    calls `[CPlayer+0x490]->RequestMovement(request)` (return address CryGame+0x66F29); RequestMovement adds +0x60
    when bit 0x40 is set. PreUpdate leaves the bit clear when `CPlayer+0x82D` is set (player can't turn).
  - Module: RequestMovement hooked inline (MinHook); calls from PreUpdate's return address get the gyro's pending turn
    added to the request's delta rotation (radians; pitch +x up, yaw -z right: **senses assumed from the SDK's mouse
    code, to verify in game**). After the game's sensitivity and zoom, so the gyro's sensitivity is GyroLib's only.
    Gameplay view while PreUpdate requested within 100 ms; else no view (menus, pause, loading). The vtable slot,
    RequestMovement's first bytes and the call before the return address are checked; another build fails over to the
    blind stick fallback. No `view_angles` / recenter yet.
  - Next: install (ask first) into `Bin64` and test: does the EA app's 32-bit `Crysis.exe` start with a 64-bit
    `winmm.dll` in Bin64 (else `version.dll`, or start `Crysis64.exe`); senses; vehicles, ladders, zoom/scopes;
    whether the XInput translation shows the DualSense to CryInput; pause/menu gating.
- 2026-10-27 **Crysis: the gyro camera works (user, in game: "funciona perfecto"; DualSense shown as XInput
  controller 0, connection and Steam Input state not recorded).** Log: `cryengine2: RequestMovement at
  CryGame.dll+0x793B0 (hooked)`, ~1400-1600 requests per 10 s with >98% carrying the gyro, `gyro off: no gameplay view`
  when PreUpdate stops. Senses as assumed (pitch +x up, yaw -z right). The XInput translation works (CryInput polls
  slot 0). The pause/menu gate works through the 100 ms request timeout.
  - **Bug: the F10 panel never opened** (`perf: present: -`, no `panel: captured`). Crysis's renderers get their device
    entry points at run time: `CryRenderD3D10.dll` imports d3d10.dll only for shader helpers (`D3D10ReflectShader`...)
    and looks up `D3D10CreateDevice` / `CreateDXGIFactory` with GetProcAddress (CrySystem.dll does too, for its
    capability check); `CryRenderD3D9.dll` the same for `Direct3DCreate9`. The import-table hooks never saw them, and
    `loads_graphics_api_itself` doesn't apply (modular build, and an API DLL is imported).
  - Fix (core, `overlay.cpp`): on the import-hook path, the game modules' `GetProcAddress` is hooked; a lookup of one
    of the device/factory creation exports (dxgi, d3d11, d3d10, d3d10_1, d3d9 via `panel_host::runtime_export`) in
    the loaded DLL of that name returns the same hook the import tables get, keeping the export as its original. Logged
    once per function (`overlay: d3d10.dll!D3D10CreateDevice looked up at run time by ...: hooked`). Built, 674 checks
    pass, deployed to Crysis; not tested in game yet.
- 2026-10-27 **Release 1.8.1**: new CryEngine 2 module (Crysis 2007, 64-bit, EA app 1.2.1 build 6156: gyro camera
  through the local player's movement request, XInput translation), and the panel's GetProcAddress hook for games
  that look up their D3D9/D3D10/D3D11/DXGI creation functions at run time (Crysis; panel not verified in game yet).
  `docs/GAMES.md`: Crysis (2007) moved to the supported games. Zip: `build/release/Gyro4All-v1.8.1.zip`.
- 2026-10-27 **Crysis Remastered (`Bin64\CrysisRemastered.exe`, PE timestamp 0x61FBF4B8, 2022-02-03): support added to
  the CryEngine module, which is renamed `CryEngine2` -> `CryEngine` (`src/engines/cryEngine/`, `Engine=CryEngine`;
  unreleased, so no old `Engine=CryEngine2` to keep). Built, 674 checks pass; not installed or tested in game yet.**
  - 1.8.1 before this: `engine: none detected (...), using Unreal`. The exe is monolithic (no CrySystem/CryGame DLLs),
    base 0x140000000, no ASLR, not encrypted (.text entropy 6.5), **no RTTI** (2 `.?AV` names, none of the game's).
    Imports: PeekMessageA, xinput9_1_0 (GetState/SetState/GetCapabilities), dinput8, winmm, version, d3d10
    (`D3D10CreateBlob` only), steam_api64. `um scan` doesn't recognise it ("Unknown native engine").
  - **CPlayerInput** from the string "CPlayerInput::SetState called: should never happen" (exe+0x1C284F0), used by
    SetState (exe+0x196B69C, `lea rcx,[msg]; jmp <log>`, like Crysis's slot 5), at exe+0x1A56228 = slot 5 of the
    vtable **exe+0x1A56200**: 0 dtor, 1 **PreUpdate exe+0x39E4E0**, 2 Update, 3 PostUpdate (same code as Crysis's:
    `cmp [rcx+38h],[rcx+3Ch]`), 4 OnAction, 5 SetState, 7 Reset (`mov [rcx+38h],40h` as in Crysis), 13 GetMemoryUsage
    (size 0xD8), 14 actions (+0x9C). The IActionListener vtable at +8 sits right before it (thunks `sub rcx,8`).
  - PreUpdate is split into helpers. `exe+0x39F150` is AddDeltaRotation(request, Ang3): bit **0x40** of the request's
    (now 64-bit) flags, delta rotation at **request+0x88**. The request is at rsp+0x60 and goes to
    `[CPlayer+0x2A0]->vtable[1]` (RequestMovement), return address **exe+0x39EBA7**; after it PreUpdate zeroes the mouse
    delta at +0x64 (same offset as Crysis). Sensitivity: `CPlayer+0x7BC`, `+0x9DC`.
  - Module: one `Build` per game. Crysis unchanged (static RequestMovement). Remastered: PreUpdate hooked (vtable slot
    and first bytes checked); its first calls read `CPlayerInput+0x28 -> CPlayer+0x2A0 -> vtable[1]`, check that it is
    in the exe, and hook it. Senses assumed the same as Crysis's (pitch +x up, yaw -z right). Detection: exe name
    `crysisremastered.exe` and `..\Game\gamedata.pak`; module prefix "" (the exe has the pump and the imports).
  - Next: install (ask first) and test: senses, the gate, the XInput translation (xinput9_1_0), and the F10 panel
    (renderer API not checked yet).
- 2026-10-27 **Crysis Remastered: the gyro camera works (user, in game; DualSense shown as XInput controller 0,
  connection and Steam Input state not recorded).** Log: `engine: CryEngine (detected)`, PreUpdate hooked at load,
  `RequestMovement at CrysisRemastered.exe+0x162980 (hooked)` on the first gameplay frame (the player had a controller
  right away), ~1200 requests per 10 s with >99% carrying the gyro. Senses as in Crysis. **In vehicles the gyro is off**
  (`gyro off: no gameplay view`): PreUpdate stops requesting movement while the player is in a vehicle (the vehicle
  takes the input), so the 100 ms gate closes; the user is fine with that.
  - **The F10 panel works here through the new GetProcAddress hook**: `dxgi.dll!CreateDXGIFactory1` and
    `d3d11.dll!D3D11CreateDevice` looked up at run time by the exe, then `panel: captured D3D11 swapchain` and
    `D3D11 renderer ready`. (Crysis 2007's DX10 path still not confirmed in game.)
- 2026-10-27 **Kingdom Come: Deliverance 1.9.7 (Epic) added to the CryEngine module (builds, not tested in game).**
  - Install: `bin\Win64MasterMasterEpicPGO\KingdomCome.exe` (small launcher, ASLR) loads `bin\Win64Shared\*` and then
    **WHGame.dll** (engine and game in one, ASLR, MSVC RTTI with ~5170 type descriptors). WHGame imports winmm.dll,
    version.dll, PeekMessageA/W, DINPUT8 and xinput9_1_0 (XInputGetState/SetState/GetCapabilities). DX11. No
    anti-cheat (EOS SDK for the Epic login). A 1.8.1 install there logged `engine: none detected ... using Unreal`.
  - RTTI: `IMovementController` vtable WHGame+0x21C42E0 (slots 1-3 purecall: RequestMovement, GetMovementState,
    GetStanceState); **`wh::entitymodule::C_ActorMovementController` vtable WHGame+0x21C4330, slot 1 RequestMovement
    WHGame+0x36CED4**. Its request copy tests bit **0x40** and passes request+0x88 to **WHGame+0x21D158 =
    CMovementRequest::AddDeltaRotation** (`or flags,40h`, Ang3 at +0x88..+0x90): Remastered's layout.
  - AddDeltaRotation's callers: WHGame+0x21D7A8 (the player's input update; reached from C_Player's vtable slot 193,
    WHGame+0x4B069C -> +0xA7C580, which calls it on `[player+0xA90]`), CAIProxy (slot 1 -> +0x6286C8), the xgenai
    module's update (+0x238B68) and C_Human/C_NPCActor (+0x1055F2C, +0x1068BC4). The player's update builds the
    request at rsp+0x70; its delta rotation is skipped when `[game+0xC8]->[+0xC0]->[+0x788]->vtable[2]` returns true
    (another view owns the turn). It sends it with `[actor]->vtable[0x220/8]()` (the movement controller) `->vtable[1]`,
    return address **WHGame+0x21DD6A**; a cold path of the same function (+0xB8D0A2 AddDeltaRotation, request at
    rbp+0x110) sends a second one, return **WHGame+0xB8D0D9** (when it runs is not known).
  - Action names seen: `rotateyaw`/`rotatepitch`, `xi_rotateyaw`/`xi_rotatepitch` and the horse's own
    `horse_xi_rotateyaw`/`horse_xi_rotatepitch`; C_PlayerInput (`wh::entitymodule`) OnAction WHGame+0x1FF950. Combat
    lock-on cvars `wh_cs_PlayerInput*` (lock area, unlock distance) may fight the gyro.
  - Module: `kKcd` build, static RequestMovement (vtable slot and first bytes checked) and both return addresses
    checked by their bytes. Detection: exe `kingdomcome.exe` next to `WHGame.dll`; module prefix "whgame". Senses and
    units assumed Crysis's (radians, pitch +x up, yaw -z right).
  - Next: install (ask first) and test on foot, on horseback, in combat (lock-on) and in dialogues; check the XInput
    translation and the F10 panel.
- 2026-10-27 **KCD, first run (user): the CryEngine module was detected but no camera hook, and F10 didn't open.**
  - `RequestMovement isn't at WHGame.DLL+0x36CED4`: the vtable check read the slot with `mem::rdp`, which rejects
    pointers not aligned to 8 (`plausible`); RequestMovement is only 4-aligned. Function pointers from vtables are now
    read with align 1 (all three reads in the module). The gyro went to the game as right-stick input meanwhile (XInput
    translation fine: `DualSense shown as XInput controller 0`, the game polls XInputGetState).
  - Panel: the first D3D11 swapchain is the launcher's splash window ("One time render" in kcd_launcher.log); the panel
    was created on it, and the game's swapchain on the real window came 1 s later (`the game presents to another window
    now; the panel's input stays on ...`). The panel's F10 polling requires its window to be the foreground root, so it
    never opened. Fix: panel ABI 4 adds `set_window` (ImGui's Win32 backend restarted on the new window);
    `panel_host::follow_window` calls it on the game thread when the target window changes, and the overlay moves its
    window subclass there (`unsubclass` restores the old window's procedure if it still has ours).
  - Also seen: the EOS overlay (`EOSOVH-Win64-Shipping.dll`) rewrites WHGame's imports of LoadLibrary*, GetProcAddress,
    XInputGetState (patched again), DirectInput8Create and GetCursorPos after start.
- 2026-10-27 **KCD, second run (user): the gyro camera works; F10 still doesn't open.**
  - `RequestMovement at WHGame.DLL+0x36CED4 (hooked), PreUpdate's call returns to WHGame.DLL+0x21DD6A`; ~1000-1300
    player requests per 10 s, >98% with the gyro; the gate closes and opens around menus. Senses not yet confirmed.
  - Panel: F10 reached GyroLib (`panel state open`, then `closed` 3 ms later) because `ready()` stayed false: the log
    never shows `swapchain ... is D3D11` nor `D3D11 renderer ready`, so gyro4all.dll never got a Present on its target,
    although our Present hook ran ~130 times/s (perf `present:`). The game presents to a swapchain other than the two
    captured at creation (splash and game window); not yet known which (a wrapper? one made outside the hooked
    factories?). Also, the earlier window fix logged nothing (`the panel's input moves ...` missing): follow_window
    needs the target to change first.
  - Fix (to test): when the static panel's target hasn't presented for 500 ms and the game presents to another
    D3D10/D3D11 swapchain with a window, the panel moves to it (`overlay: the game presents to ... swapchain ..., not
    the panel's (...): the panel moves to it`, which also logs both pointers), and its input follows that window.
- 2026-10-27 **KCD, third run (user): the gyro camera and the F10 panel both work.** Kingdom Come: Deliverance 1.9.7
  (Epic), installed as `version.dll`; DualSense shown as XInput controller 0.
  - The adoption fired: `the game presents to D3D11 swapchain <first> (dxgi.dll), not the panel's (<second>, idle 4172 ms):
    the panel moves to it`, then `swapchain ... is D3D11` and `D3D11 renderer ready (feature level B000)`. So the game
    presents to the **first** swapchain it creates (its window); the second one, made a second later on another window,
    never presents. Capture-on-create kept the last one; the panel now follows the swapchain that presents.
- 2026-10-27 **Release 1.8.2.** `build/release/Gyro4All-v1.8.2.zip`. Changes since 1.8.1: Crysis Remastered and
  Kingdom Come: Deliverance in the CryEngine module (renamed from cryEngine2); vtable function pointers read without
  8-byte alignment; the D3D10/D3D11 static panel follows the swapchain the game presents to (500 ms idle target) and
  moves its input and window subclass to that window (panel ABI 4: `set_window`). GAMES.md: Kingdom Come: Deliverance
  supported (Epic 1.9.7, `version.dll`).
- 2026-10-27 **Crysis 2 Remastered (`Bin64\Crysis2Remastered.exe`, PE timestamp 0x62EC0C0E, 2022-08-04; data in
  `GameCrysis2\`): detection and the gyro camera added to the CryEngine module. Built, 674 checks pass; not installed or
  tested in game yet.**
  - Detection: the Remastered games are a table (exe, a data pak relative to Bin64, Build); Crysis 2 Remastered's pak
    is `..\GameCrysis2\gamedata_chk0.pak`. Module prefix "" (monolithic exe). A detected game without a Build fails
    over to the stick fallback ("no camera hook known for this game yet").
  - The exe: base 0x140000000, no ASLR, not encrypted, no RTTI (2 `.?AV` names). Imports PeekMessageA, xinput9_1_0,
    dinput8, winmm, version, d3d10 (`D3D10CreateBlob` only). Ships `libScePad.dll` (not imported statically: the game
    may read the DualSense itself; not looked at yet).
  - CPlayerInput from the same SetState message (exe+0x1C30AB0, used at exe+0x12F2FD0 = slot 5 of the vtable
    **exe+0x1C30DD0**, 16 slots): 0 dtor, 1 **PreUpdate exe+0x12F0D70**, 2 Update, 3 PostUpdate, 4 OnAction,
    7 Reset, 15 GetMemoryUsage (size 0x420). Crysis 2's layout differs from Crysis's: **CPlayer* at +0x310**,
    actions +0x318/+0x31C.
  - PreUpdate (AVX code) builds the request at rbp-0x70 (64-bit flags), adds the delta rotation inline at
    **request+0x78** after testing bit 6 (0x40) of the flags, sets the bit, and calls
    `[CPlayer+0xBB0]->vtable[1](request)` (RequestMovement), return address **exe+0x12F1737**.
  - Build: the same PreUpdate-then-controller search as Crysis Remastered, with a new per-build `input_player`
    (0x28 elsewhere, 0x310 here). Senses assumed as in the other games.
  - Next: install (ask first) and test: senses, vehicles, the gate, the controller (libScePad.dll: native DualSense
    or XInput translation?), the F10 panel.
- 2026-10-28 **Release 1.8.3.** `build/release/Gyro4All-v1.8.3.zip` (30 files, `winmm.dll` reads `Gyro4All 1.8.3`;
  674 checks pass). Changes since 1.8.2: Crysis 2 Remastered in the CryEngine module (detection by the Remastered
  games table, `..\GameCrysis2\gamedata_chk0.pak`; the camera through PreUpdate's movement request, per-build
  `input_player` 0x310). `docs/GAMES.md` lists it as working (user: rename `MSVCP140.dll` in Bin64).
- 2026-10-28 **Release 1.9.0.** `build/release/Gyro4All-v1.9.0.zip` (30 files, `winmm.dll` reads `Gyro4All 1.9.0`;
  674 checks pass). Changes since 1.8.3: new Glacier 2 engine module (`src/engines/glacier/`) for HITMAN 3 (World of
  Assassination): the third-person camera controller's angles turned directly, the game's live pitch limits, the
  look-at blend at aim in/out stopped while the gyro turns, pause and cursor gating (entry below). Core:
  `MotionTrace=<seconds>` (values over 1 set the trace length; 1 = 30 s as before). No profile for the game.
- 2026-10-28 **Hitman 3 (Glacier 2): new engine module `src/engines/glacier/`, camera turned directly, working.**
  Epic build, `HITMAN3.exe` FileVersion 3.280.0.0 (`gameinfo.txt`: Build ID 10545744). Tested by the user with a DualSense
  over USB, Steam Input off; the F10 panel (DX12, through Streamline's `sl.interposer.dll`) works.
  - Loading: `Retail\winmm.dll` loads (the exe imports `timeBeginPeriod`/`timeGetTime`/`timeEndPeriod`). Frame from
    `PeekMessageA` on the main thread (called from exe+0x12951B3). No anti-cheat in the folder (EOS SDK only); the
    game has online modes: offline/single-player use only. ZHMModSDK uses `dinput8.dll`, no clash.
  - Controller: the game reads the DualSense itself over HID through Sony's `libScePad.dll` (imports `scePadReadState`
    etc., not `scePadSetMotionSensorState`: no gyro of its own on PC). Report 0x01 (USB); buttons match SDL.
  - Detection: `thumbs.dat` next to the exe and `..\Runtime\packagedefinition.txt`.
  - Globals (signatures from ZHMModSDK, https://github.com/OrfeasZ/ZHMModSDK, checked unique in this exe): player
    registry exe+0x3130720 (`lea rdx,[rip+..]; mov [rsp+50h],r15`), ZGameTimeManager exe+0x315D5C0 (static object,
    `m_bPaused` +0x70, works: pause menu), also CameraManager exe+0x3946CE0, InputManager exe+0x312B6E0,
    Hitman5Module exe+0x245B350, GetCurrentCamera's global exe+0x3194BA0.
  - The registry's `TArray<SNetPlayerData>` at +0x390 has begin = registry+0x50 and **end = 0** in this build: read the
    inline `m_aPlayerData[4]` at +0x50 (0xD0 each; net player +0x50, Hitman ZEntityRef +0xA8). Interfaces found by name
    through the entity type (+0x20 list of {STypeID*, offset}; IType name at +0x10): `ZHitman5` at ref+0x3A8;
    `m_rMainCamera` at ZHitman5+0x1348 (ref), +0x1350 (`ZHM5MainCamera`, also `ZCameraEntity`).
  - Dead end: snapshots of ZHM5MainCamera (0x3000 bytes), its composite entity and ZHitman5 while turning a full
    circle: only matrices and quaternions, no float angles.
  - camtrace (3 breakpoints): the camera's world matrix (ZHM5MainCamera+0x20) is written by a generic set-transform
    (exe+0x3F182) from local quat (+0x50) and position (+0x60) composed up the parent chain (+0x80; the parent is
    identity here) at exe+0xF26D90; the local quaternion is written at exe+0xF26748 from the **third-person camera
    controller's update exe+0x724240** (`this` from an execute breakpoint at exe+0x7242ED). The controller: vtable
    **exe+0x1E1BA10** (constructor exe+0x722945: `xor ebp,ebp; lea rax,[rip+vt]; mov [rsi],rax; lea rcx,[rsi+2F0h]`,
    unique), +0x10 the ZHM5MainCamera interface, +0x18/+0x20 the Hitman ref/ZHitman5, **yaw +0x2C, pitch +0x30**
    (float radians; copies at +0x3E4/+0x3E0). Write tests on both kept. Yaw grows turning right (kept in [0, 2π) by the game), pitch
    grows looking down; the game clamps the pitch (-0.785 up seen, at least 1.222 down).
  - A pointer search for the camera at +0x10 missed the controller (it lies in a region the search skipped); the
    module finds it with a heap scan for the vtable (+0x20 = the local ZHitman5) on its own thread.
  - Pitch: the gyro stops at the game's limits (45 up, 70 down: -0.78540..1.22173 rad, checked with the stick held fully up and down; widened by the game's own stick, at most 1.5); pushing
    past them made the stick fallback start. The stick fallback still starts now and then in play (9 times in a few
    minutes, 1 s each; the game takes the camera over at times) and saved `StickFallbackLearned=radial,0.000,1.75,76.2,
    35.7,3` to the game's `profiles\hitman3.ini`.
  - Next: which moments make the game undo writes (cover, aiming, interactions, cutscenes?), aiming/scope views,
    Bluetooth and Steam Input on, a shipped `profiles/hitman3.ini`, GAMES.md row.
  - Aiming (L2 with a weapon, recorded 60 s, gyro still): the same controller and the same two angles turn the view;
    only the FOV changes (+0x200 and copies: 40 on foot, 25 aiming; +0x2D0 -31.6 -> -6.8, +0x414 0 -> ~15.5 and
    moving with the yaw while aiming). The stick fallback starts while aiming: suspected aim assist/aim slowdown (the
    update's branch at exe+0x7243E2 recomputes the pending yaw +0x3E4 toward a target point, under game flags) taking
    the angles back from the gyro's writes. To check with the game's aim assist off.
  - The game's aim assist was already off: not the cause. Stick fallback switched off for Glacier (the module reports
    no view_angles while the controller is found; recenter reads the pitch itself). The blind fallback (no
    controller found) stays. To test: aiming with the gyro.
  - **The aim freeze was the message pump, not the camera.** `MotionTrace=300` (new: a number over 1 = that many
    seconds): at every weapon raise and lower the game thread pumps no window messages for 0.14-0.2 s while it keeps
    rendering (no F lines and no samples consumed at 15.8, 21.8, 27.9, 36.4, 45.4 s; nothing in the log there), so the
    PeekMessageA frame didn't run. The game's own stick keeps working (read inside its update). Static reading of the
    update first (exe+0x724550): it adds the stick delta from the input object `[ZHitman5+0xBD0]+0x50` to the pending
    angles, clamps the pitch to +0x5C4/+0x5C8 and stores them unless a vfunc ([+0x320]+0xD0) says not to; nothing
    resets the yaw. Fix: the Glacier module runs Gyro4All's frame on its own thread woken by each Present
    (frame_source, as Frostbite); apply adds the turn to the angles and to their pending copies (+0x3E4/+0x3E0), each on
    its own, so a write landing mid-update still counts once.
  - Dead end, don't repeat: a conditional hardware breakpoint (`--cond`) on a value the game writes every frame stops
    the game at each write; it hung the GPU (`GPU hang. D3D12: Removing Device.`) and the game died.
  - **Crash with that build** (twice, ~1 min into play, gyro turning): the game's own `int 3` at exe+0x11DFCEF, the
    same place it stopped after its "GPU hang" under the debugger; Gyro4All's frame thread idle (waiting for Present)
    in both minidumps (no heap in them); the trace up to the crash: no NaN, largest turns 9 deg yaw / 21 deg pitch in
    a frame. Cause not known yet: bisecting with two test switches in Gyro4All.ini, `GlacierPresentFrame` (0 = frame on
    the PeekMessageA hook) and `GlacierPendingWrites` (0 = only the angles are written). First test: Present frame on,
    pending writes off.
  - Test 1 (Present frame, pending writes off): no crash, freeze as before. Its trace: during each aim animation the
    frame thread ran on its 50 ms timeout, i.e. **no Present at all** for ~0.2 s: the game's whole frame loop blocks,
    apparently after the update's `pending = angle` copy, so angle writes in the block are lost to `angle = pending`.
  - The update's input delta: `[ZHitman5+0xBD0]+0x50` = float yaw, pitch **in degrees** (scaled by pi/180 at
    exe+0x1F92698 in the update, added to the pending angles, cleared after). New default `GlacierInputDelta=1`: the
    gyro's turn is added there instead, for the game's update to consume. Not yet known whether the game's input
    adds to it or overwrites it each frame.
  - Input delta result: on foot the update consumes it (yaw follows, delta cleared); **aiming at full zoom it doesn't**
    (yaw frozen exactly, the gyro's turn left in the delta; the update skips its stick step, exe+0x72494E's vfunc), while
    the game's own stick still turns the aiming camera another way. The `+0x8BC` yaw arc stays at pi: no arc. Fix: a
    delta still exactly as the module left it last frame = not consumed: the leftover goes back out, the turn goes to
    the angles (aiming), and every 8th frame the delta is tried again.
  - That fix failed in game (DualSense USB, Steam Input off): aimed in, the gyro barely moved. Live recording (25 s,
    aim in and out twice): aimed, the update **clears the delta every frame without applying it** (yaw +0x2C constant
    for 12 s with the delta written each frame), so "cleared" read as "consumed" and the turn was thrown away; the
    log's "doesn't consume" switches (every 50-100 ms, also on foot) were just reads before the game's next update.
    Both behaviours seen aimed (left, or cleared and dropped). Also: `+0x8B4`/`+0x8B8` (= the update's +0x2F0 +
    0x5C4/+0x5C8) are the **live pitch limits**: 1.222/-0.785 (70 down/45 up) on foot, +-1.396 (80) aimed,
    interpolated during the raise and lower.
  - Fix: each delta write judged on the next frame by **whether the angles moved by at least half of it** (yaw, or
    pitch away from its limits; turns under 0.005 deg tell nothing). Dropped (re-checked 1 ms later, in case the read
    fell between the update's clear and its store) or left untouched for 3 Presents: the turn is added again and goes
    to the angles; in the angles mode a 0.01 deg yaw marker (alternating sign) sits in an empty delta, and once the
    game applies it the gyro goes back to the delta. The delta is updated with an 8-byte compare-exchange, so the
    game's update can't consume it between Gyro4All's read and write. The pitch clamp reads the live limits (the
    learned widening is gone). The status line shows the mode and the switch count.
  - Still frozen/flickering at aim in and out (the environment moving and snapping back). Recordings with the stick
    only: the right stick never goes through `[ZHitman5+0xBD0]+0x50` (only Gyro4All's own writes are there; probably
    the mouse's path); on foot the stick's turn rate is the controller's `+0x2F4` (= the update's +0x2F0 struct +4);
    aimed, the game turns `+0x2C` itself (stick, ~0.12 deg/frame) in another path. The second input object
    `[ZHitman5+0xBD8]` (+0xB0 after the first, same class) stayed zero. The game's own aimed turning made the
    delta/marker judging flap 30-40 times a second (dead end).
  - The input step's delta gate `r12b` (exe+0x72476D) comes from globals exe+0x315D5A8..5B4 / exe+0x30A1196, constant
    in play; the clear at exe+0x724928 depends on `[[ZHitman5+0xEA0]+0x183]`, 0 throughout. Neither is the aim switch.
  - Cause of the freeze: the **look-at blend** (exe+0x727E05..727EEC): while the byte at the update's +0x2F0 struct
    +0x6C4 (controller `+0x9B4`) is set, each update pulls the pending angles toward a fixed world point (+0x6A0, i.e.
    controller `+0x990`) by the progress +0x6C8 over +0x570 (0.54 s), then clears it. Recorded: started at a gyro
    aim-in (yaw held for 0.54 s), never at the four stick aim-ins/outs. What sets it was not found (the two
    `mov byte [rbx+0x6C4],1` in the exe, exe+0x676DEF and exe+0x677365, are on another object).
  - Fix: angles written directly again (`GlacierInputDelta` now defaults to 0) and, while the gyro turns, the blend
    byte is cleared (as the game does when it ends); logged as "look-at blend stopped for the gyro". Untested.
  - In game (DualSense USB, Steam Input off): aim in/out with the gyro now works as intended (the look-at blend stopped
    once in the log). But the **F10 panel was gone**: the exe first makes a D3D11 swapchain for a startup window
    (0.3 s after start), and the Present-driven frame thread ran its first frame (GyroLib's context, `overlay::attach`)
    right away, so the panel attached as the static D3D11 one there and stayed; the game's D3D12 swapchain (captured
    4 s later) never got it. Fix: `overlay::has_dx12_swapchain()` (core), and the Glacier frame thread holds its first
    frame until the D3D12 swapchain is captured (20 s fallback). Frostbite's frame thread starts the same way (not
    seen to lose its panel). Checked in game: the F10 panel is back.
  - No per-game profile for Hitman 3: everything it needs is in the Glacier module (decided with the user).
  - Tested by the user: the game behaves the same with `GlacierPresentFrame=0` and `=1` now that the look-at blend is
    handled, so the Present-driven frame thread is **removed** (with the setting and the D3D12-swapchain wait for the
    panel; `overlay::has_dx12_swapchain()` removed from the core again). The gyro frame runs on the PeekMessageA hook
    like the other engines, on the game thread, so its angle writes can no longer land in the middle of the update.
- 2026-10-28 **Bug (user): Mass Effect LE 3 fell to the blind stick fallback (1.8.3, then 1.9.0).** Log: `ue3: engine not
  found (GObjects found, names found, property offset field missing)`; `ue3tool.py find` failed the same way (no
  `Matrix` planes). Cause: the name table the search accepted is a **decoy array** whose chunk 0 and chunk 1 slots hold
  the same pointer (an address inside the exe; the real chunk 1 is on the heap). Names in chunk 1 (24093 of ~53000 values) are read
  from inside other entries, so they come out as tails of other names ("nlineEventSet", "nterpTrackSet..."), which are
  still identifiers and passed the 3/4-identifiers rule added on 2026-10-26. `Matrix`/`Vector`/`Rotator` have the class
  `ScriptStruct` (a chunk 1 name), so they read as `nlineEventSet` and the planes were never found. The real table
  (`exe+0x17B33D0`, text at entry +0xC) gives: GObjects `exe+0x1887E40` (153259 objects), UProperty Offset +0x94, bool mask
  +0xD0, SuperField +0x60, Engine.GamePlayers +0x498, Player.Actor +0x68, Actor.Rotation +0x114, Controller.Pawn +0x3BC,
  Actor.WorldInfo +0x198, WorldInfo.Pauser +0x670. Fix: a chunk-array candidate is rejected when two chunks that the
  sampled names use are the same pointer or null (module `find_names` and `ue3tool.py`). `ue3tool.py find` now works on
  ME3; module built, 674 checks pass. **Not yet verified in game** (needs redeploying and a run).
- 2026-10-29 **Hitman 3: small hitches turning with the gyro (stick smooth) at a 100 FPS cap: uneven sensor time per frame.**
  `MotionTrace=120` (1.9.0, DualSense USB, gyro-only sweeps at a wall). Gyro4All's frames are perfectly regular (3795 of
  4394 at 10.0 ms; the rest 20 ms where the game ran at 50): the double `PeekMessageA` per frame is not the cause (the
  0.5 ms throttle keeps one gyro frame per game frame). ~485 samples/s, sensor intervals 1.3-3.8 ms; SDL reads the reports
  at `gl_update`, so every sample's arrival is the frame time (~13 us before it) and GyroLib's burst delivery (empty
  updates only) never engages. The sensor time inside each 10 ms frame varies 8-12 ms (p5-p95), and the turn per frame
  follows it: per-frame turn against its neighbours sd 0.19 (p5 0.71, p95 1.27), correlation 0.93 with the frame's sensor
  span, 0.21 with its sample count. The stick turns by frame time, so it stays smooth.
  - Offline replay of the trace (scratch script): playing each frame's output back in sensor time mapped to the host clock
    (offset = lowest arrival-minus-sensor latency, no extra delay) halves that jitter (sd 0.24 -> 0.11 in the replay's
    measure, which also counts the hand's own acceleration). Same fix as asked of GyroLib on 2026-10-09 (play back in
    sensor time with a small delay); not engine-specific. Not implemented yet.
  - **Implemented: sensor-time pacing in the core** (`src/core/sensor_pacer.hpp/.cpp`, wired in `host.cpp`; untested in
    game). `SensorPacing=1` (default) / `SensorPacingDelayUs=-1` (automatic), both re-read when `Gyro4All.ini` or the
    profile is saved. GyroLib's samples (observer) give sensor and arrival times; host time = sensor + the lowest
    arrival-minus-sensor offset of the last 2 s. Each update's GyroLib turn becomes a segment of a timeline (from the
    previous update's last report to this one's); each frame gets the turn up to (frame - delay), the last rate carried
    forward past the last report (at most 10 ms). Automatic delay = median of the last report's lag behind the frame.
    Per axis the frame's turn is what is owed, clamped to 0.5-1.5x of (rate at the target x frame time) with the sign of
    the sensor motion: never a step back; an extrapolation the reports undercut is paid back while turning. A GyroLib
    turn in an update without new reports (its burst delivery, BT pads like the Switch Pro) flushes what is owed and
    leaves GyroLib's turn unpaced for 1 s after the last such update (no pacing on top of GyroLib's). The gate closing,
    a 100 ms stall or a sensor clock restart drop what wasn't played back. Status line every 10 s: `sensor pacing:
    delay, frames paced, % extrapolated, frames left to GyroLib's burst delivery`.
  - Replay of the Hitman trace through the C++ class (scratch harness, same jitter measure): off 0.238, automatic
    (settled at 1.95 ms) 0.090, delay 0 0.094, delay 5 ms 0.077; totals kept (-3.78 in, -3.79 out); no frame turned
    against GyroLib's sign with automatic or 0. Better than the Python estimate (0.14) because the 0.5-1.5x clamp also
    evens out the per-segment rate. Unit tests (6, synthetic uneven USB-like reports): steady 100 deg/s at 100 FPS worst
    frame 43% off unpaced, 4.6% paced; VRR 100-165 FPS worst 3.5%; stop/restart and reversal keep the total; burst
    passthrough; off untouched.
  - **In game (user, DualSense USB, ~20 s of gyro sweeps at a wall, `SensorPacingDelayUs=0`):** the game ran at
    ~160 FPS this time (6.3 ms frames, stretches of 13 ms), 3 reports per frame typically. Log: delay 0.00 ms, 100% of
    frames extrapolated, nothing left to GyroLib's burst delivery. From the trace (F lines: GyroLib's turn vs the paced
    one; same measure as above but on the turn rate, since frame times vary): raw 0.321 (worse than 0.238 at 100 FPS,
    as expected: the same ~2 ms report jitter weighs more in shorter frames), paced 0.117; totals kept (yaw -13.30 vs
    -13.24, pitch 14.70 vs 14.65), no frame against GyroLib's sign. The scratch replay of this trace through the class
    reproduces the live output exactly; replayed with the automatic delay: 0.108, paced minus GyroLib cumulative p50
    0.016 / p95 0.26 deg (0.078 / 0.55 with delay 0: mostly the intended lead of ~2 ms, not an error). Variants that
    allow a zero turn while ahead (no 0.5x floor) left 10-16 frames nearly stalled mid-sweep: the floor stays. The
    sweeps had no holds (direction changes only), so overshoot at a hard stop isn't measured yet.
  - **Same test with the automatic delay (`SensorPacingDelayUs=-1`):** ~160 FPS again. Log: delay settled at 1.95-2.5 ms,
    50-51% of frames extrapolated. Trace: raw 0.308, paced **0.082** (-73%); totals identical (yaw -16.65, pitch
    -11.24); no frame against GyroLib's sign; paced minus GyroLib cumulative p50 0.013 / p95 0.22 deg. Replaying this
    trace with delay 0: 0.088, p95 0.49 deg. On both traces the automatic delay is ~7% smoother than 0 and follows the
    motion more closely; 0 is ~2 ms earlier. Default stays automatic.
  - **Hard-stop test (automatic delay, ~30 fast wrist turns of 300-1800 deg/s at the camera, each stopped and held; user
    noticed nothing odd).** At normal stops the paced camera matched GyroLib's within ~0.05 deg (no overshoot). Two
    faults found in the trace, both fixed:
    1. A **143 ms game stall** (a frame with 62 reports, GyroLib +13.5 deg in it): the stall reset kept the reports but
       the frame's turn was capped by the 1.5x clamp over at most 50 ms of frame time, so 7.2 deg went out and the
       rest trickled in over the next 100+ ms, plus a ~0.25 deg offset that lingered. Now a stall (frame or report
       gap over 100 ms) sends what is owed and the update's turn at once and restarts the timeline from its last
       report (the jump is already on screen). Counted in the status line (`S stalls`).
    2. **Reports stopping for 12.6 ms in the middle of a ~950 deg/s turn** (two frames with no reports, then 25 deg in
       one): the 10 ms extrapolation cap gave 6.8, 3.4, 11.7 deg; with the cap at 20 ms: 6.8, 6.8, 10.0, 7.8 (GyroLib
       alone: 0, 0, 25.0). The traces have 70-77 frames without a report per 45-60 s (not all mid-turn).
    Replay of this trace, before/after: paced minus GyroLib cumulative p50 0.25 -> 0.008 deg, 300 ms after a stop
    mean 0.14 -> 0.03 deg (max 3.3 -> 0.5); jitter 0.153 -> 0.148 (raw 0.343: fast flicks). Unit test for the stall.
  - **In game with the stall fix (automatic delay, 20 s of sweeps then fast turns; the user felt one frametime hitch).**
    ~160 FPS. Sweeps/turns: raw 0.312 -> paced 0.109; totals kept (yaw -42.13/-42.07); no frame against GyroLib's
    sign; at stops paced matches GyroLib (mean +0.04 deg, max 0.59; 300 ms after: max 0.22). The hitch: a **144.5 ms
    game frame at t=44.3 s** in a 633 deg/s turn; the pacer counted it as a stall and turned GyroLib's 4.42 deg plus
    the 0.57 owed in that frame, the next frames as GyroLib (-0.55/-0.59, -0.28/-0.20): no tail. Gyro4All's own frame
    work in that 10 s window: max 0.61 ms (perf line), so the stall is the game's. Two more stalls (148 and 189 ms)
    happened while the camera was still.
- 2026-10-29 **007 First Light (Steam, Glacier, `Retail\007FirstLight.exe`): GyroLib never starts; the game loop's frame
  hook found (offline, plus read-only stack reads of the running game; no debugger).**
  - Symptom (1.9.0): `engine: Glacier (detected)`, then no gyro frame at all, so GyroLib is never created and no
    `gyrolib.ini` is written. The exe imports `GetMessageW`/`DispatchMessageW` only (no `PeekMessageA/W`,
    `MsgWaitForMultipleObjects`): the window is pumped by a thread of its own, blocked in `GetMessageW`, and the game
    loop runs on the main thread (the DllMain thread). Same shape as RE7 (above), unlike HITMAN 3 (`PeekMessageA` on the
    main thread).
  - The exe is not packed and keeps MSVC RTTI (`.?AVZGameLoopManager@@`, `.?AVZGameTimeManager@@`, ...).
    `ZGameLoopManager`: vtable exe+0x2D0B628 (5 entries), constructor exe+0x7AC3A0, a **static object at
    exe+0x5CF8280** (built at exe+0x7BAC87 by the startup code that builds the engine's static components).
  - In this build `ZGameLoopManager` is not ZHMModSDK's HITMAN 3 frame-update registry: exe+0x7AD0C0
    (`this, const ZString& event, const delegate& (32 bytes), int priority`, 82 call sites) registers listeners of
    named **game-loop events** (`Start`, `Stop`, `Paused`, `Resumed`, `GameplayStart`, `GameplayStop`, `IntroCutStart`,
    `IntroCutEnd`, `LoadScreenGone`, `PlayingAfterLoad`, `SavegameRestored`, `StartSingletons`, ...; kept in a tree at
    +0x60 keyed by name, strcmp), exe+0x7AD320 unregisters them, exe+0x7ACFA0 raises one by name. `Paused`/`Resumed`
    could gate the gyro. Dead ends: the `ZEntityManager::FrameUpdate` ZString constants (exe+0x34ED288, 30 users) are
    keys of a per-name time budget table (exe+0x3E95E00, via exe+0x7E660), not frame callbacks;
    `ZBoxReflectionManager::PerFrameUpdate` is only an assert string.
  - **The main loop** (main thread, from the CRT entry): exe+0x2A8E3B0 -> 0x46430 (WinMain) -> 0x3F4F0 -> 0x3F6D0 ->
    0x676F0 -> (vcall +0x138) 0x3DA20 -> **0x3E150 (the loop)** -> **0x49C10 (one frame; returns false to quit)** ->
    **0x4A300 (the frame's work**: `timeBeginPeriod`/`Sleep` frame limiter, `QueryPerformanceCounter`, reads
    `ZGameLoopManager` +0x18/+0x20 (times) and +0x98 (loop state, 3 = running), calls the subsystems' updates).
  - **Hook point, data only:** 0x49C10 (at exe+0x4A218) loads a 32-byte delegate from **`ZGameLoopManager` +0xB0**
    (q0 = member fn, q1 low dword = this adjustment, q2 = static fn, q3 = object) and calls the frame through it when
    set: q0 && q3: `q0(q3 + int(q1), &closure)`; else q2 && !q3: `q2(&closure)`; else `0x4A300(this)` directly. The
    closure is a delegate of the same layout {0x4A300, 0, 0, the app object}. Live, +0xB0..+0xCF are all zero (no
    wrapper). Plan: a static wrapper in q2 that runs the gyro frame and then `closure.q0(closure.q3 + int(closure.q1))`,
    on the game thread, once per game frame, before the frame's update; found by the code at exe+0x4A218 (a
    RIP-relative `vmovups ymm2, [rip+X]`) rather than the fixed offset. Not implemented or tested yet.
  - **Implemented (not yet tested in game):** `src/engines/glacier/glacier_engine.cpp` `frame_source`: when the exe
    imports neither `PeekMessageA` nor `PeekMessageW` (HITMAN 3 imports `PeekMessageA` and keeps the core's pump
    frame, unchanged), a thread finds the wrapper by its code (`kWrapperCode`, exe+0x4A218 -> exe+0x5CF8330), waits
    for `ZGameLoopManager`'s vtable at wrapper-0xB0 (up to 5 min), then sets only the delegate's static fn (+0x10, one
    locked 8-byte exchange from 0) to `loop_frame`, which runs the gyro frame and then the frame's own delegate
    (`member(object + adjust)`). Checked every second: put back if cleared, left alone if something else fills it.
    Log lines: `glacier: the exe never calls PeekMessage: the gyro frame runs in the game loop`, `glacier: gyro frame
    in the game loop's frame wrapper (...)`. The camera is still HITMAN 3's patterns (expected: not found, so the
    gyro goes out as right stick until 007's camera is found). Deployed to the game folder as `winmm.dll`.
  - **Tested in game (DualSense, report 0x01, Steam Input reports no controller):** works. The wrapper was found and
    set 187 ms after start; GyroLib started on the first loop frame (`gyrolib.ini` created), the F10 panel opens and
    closes (DX12, render worker), motion at ~470 samples/s. Gyro frames ~79/s against ~157 presents/s (frame
    generation on). Gyro4All's frame: avg ~110-130 us, p99 ~250-300 us. The gyro stays off: the HITMAN 3 patterns
    (player registry, game time manager, camera controller vtable) all match 0 addresses in this exe, so there is no
    local player and no gameplay view (not even the stick fallback). Next: 007 First Light's scene and camera.
- 2026-10-29 **007 First Light: looking for the camera (in progress; camtrace, read-only searches, write tests, two
  one-hit write breakpoints with cdb; DualSense, Steam Input reports no controller).**
  - Nothing of HITMAN 3's camera carries over: RTTI shows a different camera system ("Knt" kinetic cameras and game
    camera entities: `ZSphericalOrbitGameCameraEntity` (69 live), `ZKntHumanoidCamera` (1), `ZCylindricalOrbitGameCameraEntity`
    (2), `ZStateDrivenGameCameraEntity`, `ZPlayerCameraOrientationLimiter`...). Sampled while the camera turned, the
    `ZKntHumanoidCamera` and both cylindrical entities don't change at all: configuration, not the live view.
  - Glacier is **Z up**: camtrace's `vectors` assumes Y up (heading atan2(x, z), pitch asin(y)) and ranks junk first; the
    scratch low-memory variant used heading atan2(y, x), pitch asin(z). camtrace `vectors` also ran out of memory on
    two 7.4 GB heap snapshots (both loaded at once).
  - The view (forward vector) has 48 copies, 10 in the exe's static data (e.g. exe+0x5E7B240, used as the view's
    forward in the tests below). Yaw copies = -(view heading) - 90 deg, in radians: exe+0x3ED7ED0 and exe+0x4A23848 are
    `ZAudioManagerWwise` (static object at exe+0x4A234C0) fields, written each frame by exe+0x6915A0 from a
    `ZKntAudioListener`'s transform (`atan2` into +0x388): copies (write tests undone within a frame).
  - `ZCameraManagerMain` is a static object at **exe+0x5D1B200** (vtable exe+0x2D897E0); its vfunc +0x38 (exe+0xBBE2C0,
    via exe+0xBBE030) returns the active camera entity.
  - The camera and the player live in the **Gameplay World ("Gpw")**: a data-oriented simulation fed through input
    queues (`Gameplay::TInputPool<SGpwInput_...>`), in a **2.7 GB private region** (camtrace snapshots skip regions over
    2 GB). Camera inputs include `SGpwInput_PlayerCameraSetSphericalCoordinates`, `SGpwInput_SetPlayerCameraWorldHeading`,
    `SGpwInput_AddPlayerCameraOffsetInput`, `SGpwInput_BlockPlayerCameraInput`. `ZPlayerSphericalCameraRotateToYaw`
    (vfunc 22/23 -> exe+0x17FF5F0) queues a rotate command (degrees * pi/180) into the object at [exe+0x6452820] (+0x1B850).
  - A yaw/pitch pair in radians (yaw = -heading - 90, then pitch, both exact) in that region at
    `<component> + 0x10C/+0x110` (one run: 0x18457358C): written every frame by exe+0x17CCE80 (from exe+0x17CD680,
    main thread, in the game loop frame: ... exe+0x13ACBC0 -> 0x13ADA60 -> 0x13AD870 -> 0x189E320 -> 0x17CD680), computed
    from the camera entity's orientation quaternion (from exe+0x16FD2D0); the components are arrays reached through
    `[exe+0x64528E0]+8` (+0x88 base / +0x90 stride, and +0xE0 / +0xE8 for the source copied into +0x00). A copy (write
    test undone within a frame).
  - Write-tested, no view change: 0x15C531E44 (kept, nothing reads it), 0x17119CE68 (animation value), 0x1C101AE28
    (undone). Not yet tried: the persistent state as a quaternion or a 2D (cos, sin) heading vector; the code that
    consumes `SGpwInput_PlayerCameraSetSphericalCoordinates`; or turning through the official input
    (`AddPlayerCameraOffsetInput` / the stick input) instead of a state.
  - **The right stick, followed from the pad to the camera** (read-only searches, then one-hit read/write breakpoints;
    camtrace's `bp` got `--read` for this, cdb `ba r`):
    1. `scePadReadState` (IAT exe+0x2B1E8E0) in `ZDeviceGamepad_DualSense` vfunc 9 (exe+0x993C60; vtable
       exe+0x2D571A8; DS4: `ZDeviceGamepad_Dualshock4_Windows` exe+0x99BAF0, XInput: `ZDeviceGamepad_XInput_Windows`
       exe+0x99BED0): `ScePadData` to a generic state (dead zone exe+0x992200), copied to device+0x20: **right stick
       at device+0x144 (x) / +0x148 (y)**. The device object lives in the 2.7 GB region (heap scans that skip regions
       over 1-2 GB miss it). exe+0x995D80 copies that state to the stack for the input system.
    2. In the Gameplay World ("Gpw": `[exe+0x64528E0]+8` is a table of component arrays {base, stride} at +off/+off+8,
       indexed by entity; the player is entity 0): exe+0x191352D (main thread, game loop frame) copies a per-frame input
       record (0x250-byte entries) into the **player input component** (+0x558/+0x560): record +0x20/+0x24 -> +0/+4
       (the right stick), +0x18/+0x1C -> +8/+0xC, +0x10/+0x14 -> +0x18/+0x1C, +8/+0xC -> +0x20/+0x24, ...
    3. The same function runs the **look input pipeline** exe+0x17CBE20: a float2 through the stage table at
       exe+0x2BD0F30 (.rdata: [0] exe+0x17CB6F0 reads input component +0x20/+0x24, [1] exe+0x17CB730 dead zone and a
       timer, [2] exe+0x17CB810 identity, [3] exe+0x17CB820 radial curve, [4] exe+0x17CB890 angle snapping (atan2), [5]
       exe+0x17CB970 ..., 9+ stages), result (64 bytes) stored in the **camera input component** (+0xE0/+0xE8) at
       +0x30..+0x6F (previous frame at +0x70, last non-zero at +0xB0).
    4. exe+0x1AD7420 (from exe+0x18A5500): unless blocked (+0x60), copies that rate (+0x30..+0x3B) into the player
       camera control component (+0x1210/+0x1218) at +0; its +0x10.. holds the control config (turn speeds 55/85/75/110
       and 110/90/45/45 deg/s by mode, curve values); then the player camera update exe+0x17B8B10 (components +0x1218,
       +0x4EF0, +0x23F8, +0x2138, +0x2450, +0x3DC0, +0xAE0, +0x4028, +0x3F20).
    - Not found: a persistent yaw/pitch of the camera. No component of entity 0 holds the view's heading as an angle
      (+-h + k*90, rad/deg, f32/f64) except the derived copy (+0x88, +0x10C) and coincidences (+0x1000 +0x58 is the
      player's height); none holds the forward vector or (cos, sin) of the heading; no (x, y, z, w) quaternion in the
      first 64 entities of any component rotates an axis onto the forward.
    - Candidate injection point (not tried): a stage of the look input pipeline (its table is in .rdata) or the
      camera input component's rate at +0x30, written and read in the same frame on the main thread: the gyro as a
      turn rate the game's own camera integrates.
  - **First injection try (built and deployed, not committed): doesn't turn the camera.** `src/engines/glacier/
    glacier_look.cpp`: in builds without PeekMessage, a MinHook inline hook on the player camera update (found from
    its call site, exe+0x1AD74A3 -> exe+0x17B8B10; the Gameplay World global from the update's start, exe+0x17B8B5D
    -> exe+0x64528E0) adds the gyro to the control component's direction/magnitude (+0x1210 component +0/+4/+8)
    as a rate (degrees / (GlacierLookYawSpeed or GlacierLookPitchSpeed, default 110 / 90, * frame time)), unless the
    camera input component's +0x60 (blocked) is set; the view's angles from the derived view component (+0x88 +0x10C
    / +0x110) for a calibration log line every 2 s. In game (DualSense, Steam Input reports no controller): the hook
    is found and called every frame ("player camera running"; it stops in pause/menus), but the view doesn't follow:
    2 s windows asked up to 74 deg and the view turned 0.00 (frames where the stick was still). So the update doesn't
    take its turn from those three floats, or something after the copy overwrites them; next: find where the update
    actually reads the turn (its use of the control component, or exe+0x18A5500 after exe+0x1AD7420). The update's
    edx (the control "entity") is a handle (0x0C020000), used through its low 16 bits (entity 0).
  - User report (stick only): turning the right stick in continuous circles, the camera ends up drifting up or down,
    as if turning inside a sphere. The user saw it in game before this test too, before the look input hook existed
    (earlier builds never wrote the camera): the game's own camera behaviour, not Gyro4All's. Worth keeping in mind
    for the injection (the turn may be applied about the camera's own axes, not yaw about world up then pitch).
  - **The first injection hooked the character's locomotion, not the camera** (offline disassembly, then read-only
    memory sampling while the user moved each stick; DualSense, Steam Input reports no controller):
    - Player input component (`[exe+0x64528E0]+8`, +0x558/+0x560, entity 0): **+0/+4 is the right stick** (already
      scaled: held right ~+0.27, held up ~+0.14), **+0x20/+0x24 the left stick**. The "look input pipeline"
      exe+0x17CBE20 reads +0x20/+0x24, so it is the **move** pipeline: camera input component +0x30 (direction x, y,
      magnitude) and the control component +0x1210 +0/+4/+8 carry the left stick. Sampled with Gyro4All's hook on, the
      control component's direction jittered with the gyro while the stick was still: the gyro was being added to
      movement.
    - exe+0x17B8B10 (the hooked "player camera update") passes control +0/+4 to exe+0x17CC360 (dead zone, then the
      stick turned into a world direction through a body rotation quaternion `[r9]`: entity transforms {quat, pos} of
      stride 0x40 at `[[exe+0x3E9AA20]+8]+0x30`, index the word at `[exe+0x3E9AA68]+4`; live it held the character's
      yaw-only quaternion and position). A second component table at `[exe+0x6452868]+8` holds the same body transform
      (+0x23F0, stride 0x50) and a 2D facing direction at +0x40. So it is the character's locomotion/facing update.
    - The "derived view" yaw at W1 +0x88 +0x10C (exe+0x17CCE80 via exe+0x17CCA10) is atan2 of a target position minus
      the character's position (a look-at/aim heading), not the camera's own state; exe+0x16FD2D0 looks up a 0x60-byte
      record by entity handle in `[exe+0x6452828]` +0xB30 (live: one record, the character's transform).
    - The right stick (+0/+4) is written by the copier at exe+0x1913936 (from the per-frame input record +0x20/+0x24)
      and read by an "any input?" idle check at exe+0x1A38A81 (+0, +4, +0x20, +0x24, +0x18, +0x1C, flags +0x76/+0x71).
      Its camera consumer is not found yet: next, a read breakpoint on +0 with those two skipped, in gameplay.
  - **Dead end that crashed the game:** 007 First Light pauses (pause menu, cursor shown) when it loses focus, so
    breakpoint runs started while the user was in this app's window never hit; three of them were ended by killing cdb
    (`timeout`). Killing an attached cdb leaves the hardware breakpoint in the game threads' debug registers: when
    gameplay resumed, the next access crashed the game. camtrace `bp` now has `--timeout` (default 120 s): it breaks in
    with DebugBreakProcess, clears the breakpoints (`bc *`) and detaches; `bc *` also runs before the detach after a
    hit. Tested on a harmless process (break-in, `bc *`, detach, process kept running). Never wrap `bp` in `timeout`.
  - **In-process traces instead of cdb** (core `WriteWatch`, already in Gyro4All: hardware read/write breakpoints on
    every game thread from its own exception handler, every accessor logged with counts and callers; no debugger, so
    nothing to crash, and the game can stay in front). `glacier_look.cpp` now provides the name `input` (player input
    component, entity 0) and adds nothing to the control component (the gyro turns nothing in this build). In the
    game's `Gyro4All.ini`: `WriteWatch=on` at start, then `WriteWatch=r:input` (re-read every 500 ms), 60 s.
    - `r:input` (+0, the right stick's x; 60 s of gameplay, stick moved): only **exe+0x1914931** (the copier, 2832x,
      `mov [rdi], eax` from the frame's input record `[r14+r12+0x20]`) and **exe+0x1A39681** (the idle check, 2832x).
      The camera doesn't read the stick from this component. (This copier is at exe+0x1914916; an identical one at
      exe+0x1913936.)
    - `r:<record +0x20>` (the queued input record at a stable address across frames): only **exe+0x14FD4D8** (2993x:
      copies the whole record from `r9`, a pending input `{.., 0x263, entity handle 0x0C020000 at +0x10, ...}`, into the
      queue; callers exe+0x14D6A63 <- exe+0x13C9604 <- exe+0x5EA179 <- exe+0x13AEC49) and the copier (2237x). The camera
      doesn't read this record either: the stick reaches the camera by another path.
    - Idea (not yet checked live): exe+0x17CCA10 (the "derived view yaw") computes the heading from a position (W
      `[exe+0x64528F0]` kind +0x710 +0x10, for the entity id at W1 kind +0xEA0 +0x30) minus the character's position
      (W2 kind +0x2340 +0x10). If that is the camera's position, the orbit camera keeps its view as a position on a
      sphere around the player, not as angles: why no persistent angle or quaternion was found, and fits the drift
      when circling the stick.
  - **Steam updated the game at 13:28 on 2026-10-29** (exe timestamp): code moved by about +0xFE0 (the player update to
    exe+0x17B9AF0, the frame wrapper to exe+0x5CF86B0), the Gameplay World global to **exe+0x6452CA0** (+0x3C0).
    Gyro4All's code patterns found them all again; the addresses above (from the earlier build) are stale.
  - **The camera's orientation is a quaternion** (paused-view comparisons, read-only, Python against the running
    game: every unit float4/float3 and double in the big private RW regions, kept when it rotated by the view's angle
    between two paused views; reference = the rendered view's vectors). Float angles: none persistent (only the
    derived +0x10C/+0x110 and transient copies). Yaw test (83 deg) and up-to-down test (139.89 deg): a family of 14
    identical (x, y, z, w) quaternions (e.g. in the entity transform array {quat, pos} pairs) and **one unique
    quaternion followed by a position and lens values (35, 3000)**: in that run at 0x182AAD570, position at +0x10.
    Forward is the local +Y axis, Z up; the derived yaw = heading - 90.
  - **Write tests while paused are not conclusive.** Rotating 0x182AAD570 by 45 deg while paused: the first time
    the derived yaw moved +43.5 within 10 s of unpausing (most likely the user's own stick move: the user saw nothing,
    the camera looking 72 deg down); the second time, at a level view, the game had put the old orientation back after
    unpausing and the view didn't move. Pausing may re-sync the camera from its own state. Next: write while the game
    runs (a few degrees every frame for 3 s), started when Gyro4All logs `player camera running`.
  - **Live spin tests (writing a few degrees every 30 ms for 3 s while the game runs, started on `player camera
    running`) on the copies: no effect**, rewritten every frame: the quaternion family (0x1832C8CA0), 0x182AAD570, the
    orbit offset. A degrees copy (camera-controller-like object with limits -180/180, -60/75; written by exe+0x561B93
    from a copy of the derived angles) kept the writes but nothing reads it.
  - **Following the writers back with in-mod write traces (`WriteWatch=<addr>`, 60 s each, user turning the camera):**
    derived view (W1 +0x88, exe+0x17CDE80: swing/twist angles of a quaternion) <- the per-entity record table
    `[exe+0x6452BE8]`+0xB30 (exe+0x14E378E, the Gameplay World's input queue: the camera reaches the Gameplay World as
    an input) <- a queued record (exe+0xC23507, pushed by exe+0x18A6B90 from the object at `[[exe+0x3E9AE20]+0x10]`)
    <- that object (exe+0x540539, worker thread: the **active camera**'s transform, handle at `[exe+0x3E9ADE8]+4`) <-
    the camera's entity transform {quat, pos, prev quat, prev pos} in `[[exe+0x3E9ADA0]+8]` +0x30/+0x38 (exe+0x4FE5B8:
    only for dirty entities; quaternion from exe+0x538560: a look-at when the flag at bitset +0x720 is set, else
    exe+0x564DD0 from component +0x1E8 = {pivot, offset, shoulder offsets, up}) <- the offset (exe+0x545687 clears the
    component every frame, exe+0x52E0A5 fills it) <- **the orbit update** (loop at exe+0x52DEB0).
  - **The orbit camera's state (persistent, integrated by the game):** camera world `[exe+0x3E9ADA0]` (+8 component
    table), per camera entity: **+0x500 (stride +0x508): radius, yaw, pitch (radians), radius/yaw/pitch rates
    (+0xC/+0x10/+0x14, set by the right stick), yaw/pitch offsets (+0x18/+0x1C)**; each frame `angle += dt * rate`,
    clamped by **+0x558 (stride +0x560): radius, yaw min/max, pitch min/max** (exe+0x51D0F0), angles wrapped to +-pi,
    radius maybe overridden (+0x9D0), then turned into the offset around the pivot (exe+0x5640A0). Live: radius 2.0, yaw
    141.35 deg (derived 141.28), pitch -4.15 (derived -5.81; up negative), limits yaw +-180, pitch -60..75. Spin test on
    the yaw (+0x504): **62 writes of 2 deg turned the view +122 deg** and the game kept it.
  - **Implemented and tested in game (DualSense, Steam Input reports no controller; connection not recorded): works.**
    `glacier_look.cpp` rewritten: code patterns find the camera world (the orbit update loop, which also pins the
    +0x500/+0x508/+0x558/+0x560 offsets), the active camera global and, as before, the Gameplay World and the player
    update (its hook now only tells when gameplay runs). Each gyro frame (game thread, before the frame's update) adds
    the turn to the active orbit camera's yaw (kYawSense +1: grows to the right) and pitch (kPitchSense -1: grows
    downwards), the pitch clamped to the camera's limits, the yaw wrapped to +-pi; no turn when the active camera has no
    orbit state. Log `glacier: orbit camera <handle> (state ..., limits pitch ..)` on each camera switch: in 7 minutes
    it followed four orbit cameras (13020057 limits -60..75, 13020086, 1302006F, 13020067 limits -75..75). User: the gyro
    turns the camera in the right directions and the right stick still works. Calibration lines (view moved / asked,
    2 s windows without the stick): large turns x1.02/1.06, x1.10/1.08, x1.07/1.07, x1.18/0.99 (the derived view is a
    smoothed proxy; the orbit angles take the turn 1:1 by construction); short windows and camera switches are noisy.
    No recenter (Recenter::No). Not yet: a release, GAMES.md, aiming/cover cameras checked one by one, cutscenes.
  - **Indoors, the pause menu (deployed; not yet confirmed one by one in game).** The player's locomotion update stops indoors, so the
    gate is now an inline hook on the orbit update (exe+0x52DDE0, the function around the loop, found from the exe's
    unwind table: `RtlLookupFunctionEntry`, following chained fragments). The orbit update keeps running behind the
    pause menu: the gyro also needs the game loop running, **ZGameLoopManager (the gyro frame's wrapper - 0xB0)
    +0x98: 3 = running, 2 = pause menu** (ZGameTimeManager +0x90 = 1 when paused would also do).
  - **Look-at cameras (L2).** Some orbit cameras look at a point: camera world bitset `[[world]+0x38]+0x720` and the
    point at component +0x1A50 (stride +0x1A58), seen from the transform's position (+0x30, position at +0x10;
    exe+0x538560). Spin tests: the **aim camera (1302006B, limits -80..80) ignores its orbit angles and follows the
    point** (turning the point turns the view 1:1); the normal look-at cameras (13020067 while a weapon is out) keep the
    point but follow their angles. So the gyro turns both on such cameras: the point about the camera's position
    (heading -= yaw, elevation += pitch within the pitch limits) and the orbit angles.
  - **Aim freezes, three causes found with per-frame traces in the mod (logged on each camera switch; removed):**
    1. When the aim ray hits nothing, the point is at about 1e8 m along the view (e.g. 89393240, -44822248, 52902):
       a sanity limit of 1e4 m skipped it and the aim camera stayed fixed for the whole aim. Limit now 1e12 m.
    2. Ruled out: the game doesn't move the written point or angles after the switch, the active camera is the one
       rendered (the camera system exe+0x540460 follows a redirect chain, bitset +0x330 -> component +0xBE0, which
       stayed on the active camera), and its output (`[[exe+0x3E9AE20]+0x10]`, transform first) turned with the gyro
       from the first frame.
    3. **L2's zoom (0.4-0.5 s, with or without a weapon; shorter with one) runs on a transition camera without an orbit
       state**: entity 0x11E with a new generation each time (1302011E, 1303011E, ...), between 13020067 and 1302006B
       both ways (also on other cuts, e.g. to 1302009E). None of its components below +0x4000 holds another camera's
       handle. The orbit update integrates every orbit camera in its list (its argument +8/+0x10: begin/end of 16-bit
       entities), active or not, so the stick keeps turning them during the zoom. Fix: **while the active camera has no
       orbit state, the gyro turns every orbit camera in that list** (points included); the game re-aligns 13020067 to
       the aim camera's view when it switches back. **Tested in game (user): the gyro now turns the view during the
       zoom, with and without a weapon.** Not checked: whether other cameras (cover 13020061, 1302008E, 1302009E) come
       back turned after a cut that turned them, and cutscenes (other cameras without orbit, e.g. 130100CF, also
       trigger it).
  - **With the gyro activated by L2 (GyroLib trigger activation, threshold 0.2).** One session (gyrolib.ini switched
    from always-on to L2 while the game ran) reported the zoom still frozen; it couldn't be told apart in that log. After
    a restart, a summary line per transition (`glacier: transition camera <handle> for N ms: N gyro frames (N turned
    cameras), asked yaw .. pitch ..`, first 12 transitions) showed the gyro arriving on every frame of every zoom
    (27 frames in 500-516 ms, every frame turned the cameras) and the user saw it work, with L2 activation and again
    with always-on. That game's transition camera was entity 0x105 (1302..13140105): the entity changes between
    sessions, so it is told by having no orbit state, not by its index. The cause of the earlier report is unknown.
  - **Log cleanup for publishing.** Removed the 2 s calibration line (`glacier: orbit camera: N frames, the view
    turned ...`, ~1800 lines an hour of play); the camera line (`glacier: orbit camera <handle> (...)`) is now logged
    once per camera (16 kept) instead of on every switch; the transition line stays (first 12 per session). The core's
    status block (status, motion, sensor pacing, perf: 4 lines, every engine) now comes every 30 s instead of 10 s
    (`kStatusMs` in `src/core/host.cpp`; its rates are per second over the real span, so unchanged; docs/ADVANCED.md
    updated).
- 2026-10-29 **007 First Light: a Switch Pro controller (DirectInput, no Steam Input) shown as XInput but never read**
  (built, not tested in game yet). Log: `Nintendo Switch Pro Controller shown as XInput controller 0`, then every 10 s
  `game polls per slot 0/0/0/0`: the game never called XInputGetState (imported from XINPUT9_1_0, hooked).
  - **Why**, from the exe (static disassembly): the input manager's DirectInput device callback exe+0x994820 (rdx =
    DIDEVICEINSTANCEW: guidProduct at +0x14, dwDevType at +0x24) first matches the Sony product GUIDs (Sony pad
    devices), then calls exe+0x994F40 (rcx = ?, rdx = &guidProduct; returns BOOL in eax): Microsoft's WMI
    `IsXInputDevice` sample (wide `Win32_PNPEntity`, `DeviceID`, `IG_`, `VID_`; `cmp ecx,[r14]` = guidProduct.Data1).
    True: an XInput device (0x1B0 bytes, ZDeviceGamepad_XInput_Windows) whose slot is the first from the manager's
    next-slot counter (+0x48) up to 3 where `XInputGetCapabilities(slot, 0)` succeeds with SubType 1 (gamepad), asked
    once; none: slot -1 (never polled). False: nothing (the game has no DirectInput joysticks). Its update exe+0x99BF90
    calls XInputGetState([this+0x188]). Same scheme as Battlefield 4 (2026-10-21), different calling convention.
    A second copy of the `IG_` string's user, exe+0x28B4620 (caller exe+0x28B3D93, result not tested), isn't it.
  - **Fix** (`src/engines/glacier/glacier_xinput.cpp`): from the game's first DirectInput8Create (`dinputpad::on_create`),
    the functions that `lea` the wide "IG_" string whose direct callers test the result (`call; test eax,eax|al,al; je`
    within 16 bytes): exactly one in 007FirstLight.exe (exe+0x994F40, checked offline with the same search in Python).
    Hooked with MinHook; answers 1 for Nintendo (057E) controllers when the XInput translation is on (Sony ones keep
    their native route; other DirectInput pads not handled). Core (`xinput.cpp`): `listed_as_xinput(vendor, product,
    wait)`: Glacier doesn't wait (the device list may be built on the frame's thread, and GyroLib's controller only
    shows seconds later: 3 s in the log), so a listed controller not shown yet reads as a connected gamepad to
    XInputGetCapabilities in the first free slot until the frame decides about it (`g_listed_pending`); the device
    then gets that slot and reads not connected until the controller is shown. Battlefield 4 keeps its 1 s wait.
  - **Engine-neutral fix on top** (`src/core/xinput_wmi.cpp`, built, not tested in game): instead of each engine
    hooking its copy of the check, the check's WMI query is answered differently. `CoCreateInstance` (game modules'
    imports; combase.dll's export inline on protected exes): when the game creates CLSID_WbemLocator, a class object of
    our own (CLSID_WbemClassObject, in process, no WMI connection) gives fastprox.dll's `CWbemObject::Get`
    (IWbemClassObject slot 4), hooked inline with MinHook. A test program here: all 235 Win32_PNPEntity instances share
    that Get with our object (CWbemInstance and CWbemClass vtables alike).
    **First version crashed 007 First Light at start** (stack overflow, dump in CrashDumps): it patched the vtable
    slots of IWbemLocator::ConnectServer (3), IWbemServices::CreateInstanceEnum (18) / ExecQuery (20) and
    IEnumWbemClassObject::Next (4) in place, and the Steam overlay (`gameoverlayrenderer64.dll`) hooks the same
    ConnectServer and CreateInstanceEnum (our ConnectServer original was its hook); our CreateInstanceEnum hook and
    Steam's ended up calling each other. Dead end: patching WMI vtables shared with the Steam overlay.
    The rest of the first version stays: a `DeviceID` starting `HID\` without `IG_`, whose
    VID/PID (USB `VID_xxxx`, Bluetooth `_VID&0002xxxx`) is a present HID game controller (SetupAPI + hid.dll: usage page
    1, usage 4/5/8; paths with `IG_` skipped; Sony skipped with XInput=1 when the game has Sony's pad library), reads as
    `HID\VID_xxxx&PID_xxxx&IG_00\<instance>` (Bluetooth rebuilt in the USB form, which the sample's `swscanf("VID_%4X")`
    parses), and `listed_as_xinput(.., false)` is called. Calls from Gyro4All, gyrolib.dll and SDL3.dll pass untouched.
    The controller list is read again when older than 2 s. Covers any DirectInput pad (8BitDo in DInput mode, generic
    pads), not only Nintendo. The engine hooks (`fb_xinput.cpp`, `glacier_xinput.cpp`) stay as a fallback: with the WMI
    answer the real check already says yes and they pass it on. `VtableHook` moved from dinputpad.cpp to
    `src/core/vtable_hook.hpp` to share it. Not covered: other forms of the check (`IG_` in a Raw Input or SetupAPI device
    name) and controllers DirectInput doesn't list (Steam Controller without Steam).
  - First run (vtable version): `the game creates a WbemLocator` at 5 s, before its first DirectInput8Create, and
    `the game reads controller 057E:2009's DeviceID: shown as an XInput one` 1 s later: the check works as planned.
  - To verify: `xinput: WMI: the game creates a WbemLocator`, `WMI: IWbemClassObject::Get hooked`, `WMI: the game reads controller 057E:2009's DeviceID: shown
    as an XInput one`, `the game lists controller 057E:2009 as an XInput device itself`, then `the game polls
    XInputGetState` and nonzero polls on slot 0. Also Battlefield 4 again (DualSense, its fb hook now passes the real
    answer) and a game that polls XInput slots directly (no change expected).
- 2026-10-29 **007 First Light: Switch Pro controller as an Xbox controller through the WMI hook, user-verified**
  (inline `CWbemObject::Get` version; Steam Input off: `No controllers reported by the game's Steam Input service`;
  Pro controller connection not recorded). The user: the Pro controller (DirectInput, no Steam Input) works in game,
  and so does the DualSense afterwards (the game's own support: `nativepad: the game opened controller 054C:0CE6`,
  64-byte reports, USB). Log: `the game creates a WbemLocator` at 10 s, `IWbemClassObject::Get hooked` 70 ms later,
  `057E:2009's DeviceID: shown as an XInput one` (5 s before the game's first DirectInput8Create: the WMI pass
  runs before the device list), `shown as XInput controller 0`, `the game polls XInputGetState`, then 300-1500 polls
  per 10 s on slot 0, all answered with the controller, sticks and buttons moving. No crash with the Steam overlay.
  The Glacier hook (`glacier_xinput.cpp`) was installed too but had nothing to add (the real check already said yes).
  Not tested yet: Battlefield 4 with this build, an 8BitDo/generic DirectInput pad, a Bluetooth Pro controller.
- 2026-10-29 **Control (Steam 870780, Ultimate Edition, March 2025 build): recon, and a Northlight engine module that only
  detects it** (`src/engines/northlight/`, plan in `games/Control/MODDING_PLAN.md`). Builds and unit tests pass; not run
  in game yet (the game itself has never been started on this machine).
  - Northlight (Remedy), native x64, no packer, no anti-cheat. `Control.exe` (90 KB, .NET) is the launcher that starts
    `Control_DX12.exe` (PE timestamp `0x67e06bf2`) or `Control_DX11.exe` (`0x67e06e49`); `um scan` calls the game ".NET"
    because of it. Modular build: the engine is in `<module>_rmdwin10_f.dll` (DX12) / `<module>_rmdwin7_f.dll` (DX11):
    `rl_`, `app_`, `input_`, `coregame_`, `coreshared_`, `renderer_`, `d3d_`, `physics_`, `ui_`, ... with **decorated
    C++ exports** (coregame 8192, rl 6678) and RTTI.
  - Camera classes: `CameraComponentState@coregame` (exports `getRotation` -> `Matrix3Template<float>`, `getMatrix` ->
    `Matrix4x3Template<float>`, `getInterpolatedTransform`, `getView` -> `PerspectiveView`, `getInterpolatedFOVInRad`,
    vtable `??_7CameraComponentState@coregame@@6B@`), `UpdateCamera@coregame`, `CameraEntity@content`,
    `PlayerCameraComponent(State)`, `TailCameraComponent(State)`, `GamePlayerCameraMovementComponent(State)`,
    `FPSCameraComponent`, `FreeCameraComponent`, `PhotoModeCameraComponent`, `CameraManComponent`.
  - Window messages: `PeekMessageW`/`GetMessageW`/`GetRawInputData` are imported by `app_rmdwin10_f.dll`; the exe
    imports two USER32 functions only. The module returns `module_prefix` `app_rmdwin`, so that DLL is hooked like the
    exe (to verify: the core's per-frame hook fires).
  - Pads: `rl_rmdwin10_f.dll` wraps XInput (`rmdXInputGetState`/`SetState`) and loads it at run time (strings
    `xinput1_4.dll`, `xinput1_3.dll`, `XInput9_1_0.dll`, `XInputGetState`): no import slot, so the core's XInput hooks
    won't see it and the stick fallback this module reports (`no_camera`) can't reach the game yet. Open: inline hooks
    (`inline_hooks()`, as for protected exes) or a `GetProcAddress` hook. No libScePad, no HID: no native DualSense path.
  - Proxy: `winmm.dll` is imported by `bink2w64.dll`, `navigation_rmdwin10_f.dll` and `wtf.dll` (should load; to
    verify); `version.dll` only by `nvngx_dlss.dll`. reg2k's old Control Plugin Loader used `xinput1_4.dll`.
  - Module: `detect` = `coregame_rmdwin10_f.dll` or `coregame_rmdwin7_f.dll` next to the exe; `is_launcher` =
    `Control.exe`; `read_frame` = the default view with `no_camera` (gyro as right stick) and the mouse cursor as the
    only menu signal; `apply` writes nothing.
  - Next: install, check the log (engine `Northlight`, the pump hook, XInput polls), then the XInput route, then the
    camera (exports/RTTI first, `tools/camtrace/` as soon as that stalls).
- 2026-10-29 **CONTROL Resonant (Steam 3669870, Northlight): recon, and detected by the Northlight module** (read-only;
  builds and unit tests pass; not run in game with Gyro4All).
  - One exe, `CONTROLResonant.exe` (99 470 256 bytes, PE timestamp `0x6ac4318a`), DX12 only, not packed (`.text .rdata
    .data .pdata _RDATA .rsrc .reloc`, normal entropy); no Denuvo (press, launch build), no anti-cheat found,
    single-player (`nakama-sdk.dll` and `steam_api64.dll` imported; Nakama's role unknown). 650 decorated exports
    (content/Lua timeline types), RTTI present. Data: `data_pack2\generic|pc\*.rmdtoc/*.rmdblob` (Control:
    `data_packfiles\*.rmdp`). Config: `%LOCALAPPDATA%\Remedy\CONTROLResonant\renderer.ini`.
  - The exe imports `PeekMessageW`/`GetMessageW` (module_prefix "", unlike Control), `WINMM.dll` (`timeGetTime`,
    `timeBeginPeriod`...: the `winmm.dll` proxy loads with the exe), `VERSION.dll`, `XINPUT1_4.dll` ordinal 2 only
    (XInputGetState), `HID.DLL` (11) and `SETUPAPI.dll` (6). Strings: `libScePad_static` ("Failed to initialize the
    libScePad_static library for PlayStation controller support": Sony's pad library linked in, over the exe's HID
    imports, so the core's nativepad HID hooks should see it), `GameInput.dll`/`GameInputCreate` (run time), Wwise 2024.1
    motion sinks (ScePad haptics/rumble, GameInput, XInput rumble). Press: native DualSense on PC with haptics.
  - Camera classes (RTTI): `PlayerCameraComponent`, `TailCameraComponent`, `FPSCameraComponent`, `FreeCameraComponent`,
    `PhotoModeCameraComponent`, `CameraManComponent`, `CameraComponent` (content); `CameraInputComponent`,
    `CameraLockOnComponent`, `CameraSetComponent`, `CameraSetDatabaseComponent`, `CameraSetStateZoneComponent`,
    `CameraConversationComponent`, `CameraClipPlayerComponent` (contentgame). The engine is now ECS (`ecs::system`,
    `heron::*` systems).
  - **The game has a gyro system:** `coregame::gyro_input::gravity_vector::system(GravityVector&, const env::Input&,
    const VariableTime&)` and the global `sg_gyroInput`. Only a gravity vector is named (tilt, not obviously aiming);
    no menu strings found for gyro aiming and no press mention of PC gyro. To check in game: does the options menu
    offer gyro aiming with a DualSense? If it does, Gyro4All must stay off (doubled motion) or replace it.
  - Already in the folder (user's own): ReShade (`dxgi.dll`, RenoDX addons `renodx-controlresonant.addon64`,
    `renodx-mfgunlock.addon64`), SCSKiller (`d3d12.dll` proxy). Watch the overlay's Present hook with both.
  - Module: detects `data_pack2\pc\*.rmdtoc` next to the exe; module_prefix "" there (`app_rmdwin` only when Control's
    modules are next to the exe); `is_launcher` only for `Control.exe` in a Control folder.

- 2026-10-29 **Ghostrunner: a player's DualSense buttons didn't reach the game; not reproduced.** The player (store,
  connection and log unknown) turned Steam Input on to get the buttons, and saw no "DualShock 4 / restart" notice.
  Retest on the Epic build (`Ghostrunner-Win64-Shipping.exe`, UE 4.26, Gyro4All 1.9.0, first run: fresh `Gyro4All.ini`,
  no profile), DualSense over Bluetooth, Steam Input not involved: no Sony pad library and no HID native sign
  (no `native` / `nativepad:` lines; the folder has no `libScePad*.dll`). The game polls `XINPUT1_3!XInputGetState`
  (re-hooked after `EOSOVH-Win64-Shipping.dll` rewrote the import), and 2.2 s after it starts polling Gyro4All shows the
  DualSense as XInput controller 0; buttons and sticks work. The DualShock 4 learning path never runs on this build,
  and USB vs Bluetooth makes no difference to that. Still open: the player's `Gyro4All.log` and store build. If that
  build has a Sony library that never opens, reads, or closes the DualSense, auto mode stays on "native support"
  forever: `sony_sign_holds` is always true with a Sony library, and `dualsense_closed_unread` needs an open and a close.
- 2026-10-29 **Control: first run with the Northlight module: the proxy loads, the engine is detected, but no gyro frame
  ran.** Log: `engine: Northlight (detected)`, `hooks: game modules prefix 'app_rmdwin', 2 loaded`, then `PeekMessageW on
  thread 33976, called from app_rmdwin10_f.dll+0x548F` (about 580 calls/s, almost all empty) while the game thread was
  the DllMain thread 22976, which never pumps. The game window `AppWindowClass` ("Control") belongs to thread 33976
  (also `MSCTFIME UI`/`IME`); `DXGIWatchdogThreadWindow` on another. Fix: `pump_window_class()`/`window_class()` =
  `AppWindowClass` for Control (not Resonant, unseen yet). Built; to verify after redeploying (the log should show
  `thread ... pumps the game window (class AppWindowClass) and becomes the game thread`, then GyroLib starting).
- 2026-10-29 **Control, second run (pump_window_class AppWindowClass): the game thread is adopted and GyroLib runs, but the
  game never polls our XInput.** Log: `thread 34796 pumps the game window (class AppWindowClass) and becomes the game
  thread`, GyroLib ABI 1 / SDK 1.3.1, SDL reader, DualSense (connection not recorded) `shown as XInput controller 0`,
  motion 545-555 samples/s; `game polls per slot 0/0/0/0` every 10 s (rl_rmdwin10_f.dll's GetProcAddress'd
  XInputGetState bypasses the import hooks, as expected). The Steam Input reader found no public Input exports in
  Control's old steam_api64.dll. The mouse cursor read as shown the whole run (`view=menu (cursor)`, gyro off), also
  while the user was in game: unclear whether Control hides it some other way; the cursor line now logs flags, handle
  and position. Change: `inline_hooks()` for Control (system exports hooked inline, the path that handles Titanfall 2's
  run-time xinput1_3 load). Built; not run yet.
- 2026-10-29 **Control, third run (inline hooks): every XInput export hooked (MH_OK), still 0 game polls: the Steam
  overlay hooks xinput1_4 on top of ours.** In the running game `xinput1_4!XInputGetState`/`XInputSetState` jump to
  `gameoverlayrenderer64.dll+0xd9c00`/`+0xd9d90` (a trampoline then Steam's code), while `xinput1_3` and `xinput9_1_0`
  still jump to Gyro4All (`WINMM.dll+0x433c0`/`+0x43a90`). Control's `rl_rmdwin10_f.dll` tries xinput1_4 first, so its
  polls end in Steam's hook, which doesn't call through (Steam Input answering for the pad, presumably). Only Gyro4All
  had loaded the three XInput DLLs; the dinput8 HID opens and the WMI `IG_` check came from GyroLib's SDL, not the game.
  Other facts from this run: the DualSense reports were 78 bytes (Bluetooth); `PeekMessageW` now reaches the hook through
  `gameoverlayrenderer64.dll+0xB1E26` (Steam's PeekMessage hook) and the adoption by window class still picked the right
  thread (33732); the cursor reads `flags 1, cursor 0x10003` (arrow) in menus and `flags 0, cursor 0` in gameplay (gyro
  turned on at 23:12:34), so the cursor gate works. Next: the same run with Steam Input off for Control (does Steam's
  hook then call through?); if Steam still keeps the pad, re-hook xinput1_4 after the overlay or hook rl's own import.
- 2026-10-29 **Control: where the game's XInput calls go, and the fix (not caused by the DirectInput/WMI change of
  880581d).** Steam Input was off for this run. A scan of the running game's writable memory for the XInput exports'
  addresses: `rl_rmdwin10_f.dll+0x3a28a0` = `xinput1_4!XInputGetState`, `+0x3a2898` = `xinput1_4!XInputSetState` (rl's
  data: the GetProcAddress results of `r::RmdXinput`); `snd_rmdwin10_f.dll+0x2b2db8/+0x2b2dc0` = xinput9_1_0
  GetCapabilities/SetState (Wwise rumble); the rest in heaps, SDL3 and our own winmm.dll. So the game calls the export,
  which the Steam overlay hooks in front of ours and answers itself, as in Mass Effect LE (MODLOG: the protector's
  xinput1_4 import slots redirected). That fix searched the exe only (`no xinput1_4 import slot in the exe`). 880581d
  only changed XInputGetCapabilities for a listed, not yet shown controller and added the WMI check; neither decides
  whether XInputGetState calls arrive.
  - Change: `hooks::redirect_slots` searches the non-executable sections of every game module, not only the exe (no
    change for Frostbite/Source, which have no module prefix); Northlight's Control prefix is now `rl_rmdwin` (the
    inline PeekMessageW hook catches app_'s pump without it). Built, 695 checks pass; not run yet. Expected log:
    `hooks: xinput1_4.dll!XInputGetState slot at rl_rmdwin10_f.dll+0x3A28A0 (an import table of the game's own) now
    points to Gyro4All`, then nonzero game polls on slot 0.
- 2026-10-29 **Control: XInput translation and gyro as right stick work, user-verified** (DualSense over Bluetooth,
  78-byte reports; Steam Input off; the Steam overlay running). The game polls slot 0 about 650-740 times/s, all
  answered with the translated controller; `gyro stick up to 1.00` while turning. Sticks and buttons reach the game.
  The cursor gate switches the gyro on in gameplay (`flags 0, cursor 0`) and off in menus (arrow cursor). Next: the
  camera (module exports/RTTI first: `CameraComponentState@coregame`, `GamePlayerCameraMovementComponent`).
- 2026-10-29 **Control: camera objects found from exports and RTTI (read-only), and the first write test.**
  - `coregame_rmdwin10_f.dll` exports: `CameraComponentState::getView` = `this+0xC0` (`PerspectiveView`);
    `getRotation`/`getPosition` call `r::GameObjectState::getComponentByTypeId([this+0x28], <lazily registered type
    id>)` (rl import) and read a quaternion at +0xC0 / the position at +0xD0 of that component (the object's transform).
    `getInterpolatedFOVInRad` blends `this+0x80`/`+0x8c` by the frame fraction.
  - Vtables from RTTI (2025 DX12 build): `CameraComponentState` coregame+0x5d6988; in the exe `PlayerCameraComponentState`
    +0xe45100 (second vtable +0xe450d8 at this+0x40), `TailCameraComponentState` +0xe56ad0,
    `GamePlayerCameraMovementComponentState` +0xe2b068, `FPSCameraComponentState` +0xe1db50, `FreeCameraComponentState`
    +0xe1e158, `PhotoModeCameraComponentState` +0xde54b8, `CameraManComponentState` +0xdfe7c8. In game: 59
    `CameraComponentState`, one of each special camera (each right after its own `CameraComponentState`, 0x3C0 bytes:
    one entity per camera), two `GamePlayerCameraMovementComponentState`.
  - Conventions (snapshots before/after the user turned about 70 deg right and looked up): world +Y up; the camera looks
    along its local +Z, no roll. Every camera state's `+0x40` is the view quaternion (x, y, z, w), `+0x50` the camera
    position. `GamePlayerCameraMovementComponentState+0x40` (copies +0x60, +0xB8; the second instance at +0x4E0/+0x7C0) is
    the unit look direction (= the quaternion's +Z); `+0x2E0`/`+0x540`/`+0x820` is a world point about 120 units along it
    (aim/look target); `+0x500` and `+0x740`/`+0x7A0` positions (pivot, camera). **No yaw/pitch angles** changed in any of
    these objects (searched for -70 deg / +38 deg in degrees and radians).
  - `camtrace write-test GPCMovement+0x40 --yaw 20`: undone within 30 ms (a copy rebuilt every frame). `find-vec` of the
    direction: hundreds of heap copies (render buffers, stride-4 matrix rows): not worth write-testing one by one.
    Next: one-hit write breakpoint (`camtrace bp`) on `GamePlayerCameraMovementComponentState+0x40` to find its writer
    (with the user's OK; anti-debugging unknown: the exe calls `r::isDebuggerPresent`).
- 2026-10-29 **Control: the camera's look direction found, user-verified: `GamePlayerCameraMovementComponentState+0xB8`**
  (vtable exe+0xe2b068 in the 2025 DX12 build; a unit 3-vector, world +Y up, the camera's local +Z).
  - `camtrace bp` (one-hit write breakpoint, cdb `-pd`, the user saved first; detached cleanly, game kept running) on
    `+0x40`: written at `exe+0x3e4165` in the component's per-frame update: `+0x40` = normalize(`+0xB8`), stored only if
    it changed (then `exe+0x6a440` with `r::FlowConnectionManager::sm_pInstance`, a change notification), then `+0x60` from
    `exe+0x3e4210`. Earlier in the same function the stick input (`[rbp+38h]`/`[rbp+3Ch]`), scaled by `[r10+1E0h]`, goes to
    `exe+0x3e4630(this, &input, xmm2, xmm3, dt)`, then `exe+0x3e4aa0`, `exe+0x3e4d20` (the turn of `+0xB8`, presumably);
    another branch writes the scaled input to `+0xF4`/`+0xF8`. Call stack: exe+0x517853, +0x5144f9, +0x383a6f, +0x383d96,
    then rl_rmdwin10_f.dll.
  - `write-test +0xB8 --yaw 20`: **kept** (heading -174.9 -> -154.9 for 1 s), `+0x40` and the player camera state's view
    quaternion followed in the same frame, and the user saw the view jump 20 deg. The source for the gyro: rotate this
    vector every frame (yaw about +Y, pitch about the camera's right axis).
  - Two instances exist in game; the one written (0x...31b0 in this session) is the one whose `+0x40` tracks the view.
    Next: Northlight module: find the component by vtable (background heap scan, as Swarm), pick the live instance,
    apply = rotate `+0xB8`, pitch limits, gate by the active camera (not photo mode / free camera / cinematics).
- 2026-10-29 **Bug (user): DOOM (2016) Vulkan, the F10 / pad-shortcut panel invisible in game but capturing mouse and
  keyboard; in the front end it shows, with its top cut off** (user's log: 1.8.3, `DOOMx64vk.exe`, swapchain and window
  3200x1800 on a 3840x2160 desktop, **AMD GPU** (memory types with `DEVICE_COHERENT_AMD` flags); Steam Input state not
  recorded). Not reproduced here with 1.9.0 (NVIDIA, 3840x2160): panel shown in front end and game.
  - Log: one swapchain and window for the whole session, no present on another swapchain, no Vulkan error, no `no frame
    presented`. In the front end the panel worked (`gyrolib.ini changed, reloaded`). After the level load, F10 opened
    it (ImGui window focused, nav active, DirectInput mouse/keyboard idled) and the user closed it again unseen.
  - Suspected cause (not confirmed: the log didn't record the present's queue): gyro4all.dll's Vulkan renderer took the
    queue and family of the first present (`init`) and submitted every later panel draw there, whatever queue the game
    presented from. On a later present from another queue (DOOM's async compute on AMD) that is (1) another thread's
    queue used without its external synchronisation and (2) drawing into an image owned by another queue family with no
    ownership transfer, undefined on AMD (lost drawing); NVIDIA tolerates it.
  - Fix (`src/panel/panel_vk.cpp` `present`, to test on AMD): the panel submits on the queue being presented; a new
    family rebuilds the frames (command pools are per family) and re-uploads ImGui's textures (exclusive images); a
    family without graphics skips the panel and, with no present seen, the panel closes after 250 ms instead of
    capturing input unseen. Logs `Vulkan: the game presents from queue ... (family N) now, not ...` (first 5 changes)
    and `the panel's last submissions not done after 100 ms` (first 3; that path was silent). Built, 695 checks pass.
  - The front end's cut-off top isn't explained by the log (ImGui display, client and swapchain all 3200x1800).
  - Also in the log: `motion source 0 ... the controller was reopened` every few seconds, often with cursor/menu changes.
- 2026-10-29 **Control: the gyro turns the camera through the look direction (first run of the camera build).** Log:
  `GamePlayerCameraMovementComponentState vtable Control_DX12.exe+0xE2B068 (16 ms)` (RTTI at run time matches the
  offline one); the scan found 4 objects with that vtable: two on a thread stack (fail: vtable/length 0), the live one
  (`0x208d363bb90`) and a second heap instance whose `+0x40` isn't its `+0xB8` (`copy differs (dot 0.000)`, an unused
  one); the live one was picked about 45 s in (once gameplay began). `applied yaw=19.7 pitch=4.7`, then 11.2/67.8 deg:
  the gyro writes went through (user's feel not recorded yet). Bug: the per-frame check used the scan's full test, which
  fails for a frame after a gyro write or while turning fast (`+0x40` and the view quaternion trail `+0xB8`), so the
  component was dropped every 1-10 s (8 times in 30 s) and the gyro stayed off until the next 5 s rescan. Fix: the frame
  only checks the vtable and the look vector's length (the scan keeps the full test). Built; not deployed yet (game
  running).
  - Same day, no regression on NVIDIA (user: DOOM 2016 Vulkan, 3840x2160, panel opened three times, front end and
    game): no `presents from queue` line, so there every present comes from the queue of the first one. Waiting for
    the AMD user's test and log.
- 2026-10-30 **Control: freeze fix verified in the log** (DualSense Bluetooth, Steam Input off, the user's gyro
  activation on L2/L1 press). 6 min of play with the camera build: the component was dropped once (23:56:58), when the
  game replaced it (a new pair from 23:57:03, its `+0x40` still unfilled for a scan; the new live one picked at
  23:57:08: the user died, so a respawn replaces the camera entity); no other drops while turning or aiming (before the fix: 8 in 30 s). Status
  lines every 30 s show the gyro on with pitch -12.9..+6.9 and heading moving. The 5 s rescan interval leaves up to 5 s
  without gyro after such a reload.
- 2026-10-30 **Control: rescan every 1 s (was 5 s) while there is no camera movement component**, so the gyro returns
  about a second after the respawn's new component is filled in (it was up to 10 s). Built (695 checks) and deployed;
  not run yet.
- 2026-10-30 **Control: the gyro judders, the stick doesn't: measured, and the cause** (read-only sampler of the
  component's `+0x40`, the game's once-per-update copy, during a steady pan; DualSense Bluetooth, Steam Input off).
  - Gyro pan, 8 s: 135 updates/s (intervals 5-10 ms as sampled), `+0xB8` written 391 times/s (about 3 per update);
    heading steps median 0.436 deg, 274 of 1079 updates under half the median and 239 over 1.7x.
  - Stick pan, 8 s: 137 updates/s, `+0xB8` written once per update; steps 0.124-0.152 deg throughout (2 of 1097 under
    half, 0 over 1.7x), frame-to-frame jitter median 0.10: the game turns by about the same angle every update.
  - Cause: the update (`exe+0x3e3be0`, in the job system: `r::experimental::JobSystem::executeNextJob` ->
    `r::FlowUpdateGraph::update` -> exe+0x383d96 -> ... -> exe+0x517853) runs on a worker thread, not the pump thread
    that writes the gyro (about twice per frame there: 248 camera updates/s at about 123 presents/s). So 0-3 gyro steps
    reach each game update, and a write between the update's read and write of `+0xB8` is lost. Not Bluetooth (591-614
    samples/s, even) and not the sensor pacing (it keeps the motion right over time; the steps reach the game out of
    step with its frames).
  - Fix: hook the update (found by its unique `+0xB8` -> `+0x40` copy code, `48 8D 57 40 0F 28 C4 0F C2 02 00 0F 50 C0 83
    F8 0F 74 ?? 0F 11 22` at exe+0x3e4152, and its unwind entry; one argument, rcx = the component; frame time from a
    global): `apply` adds the turn, the hook applies it to the live component right before the game's own update (a turn
    untaken for 100 ms is dropped: pause, loading). Without the code, the old direct write. Built (695 checks); not run.
- 2026-10-30 **Control with the update hook: user: "much smoother now, similar to the stick", but still uneven steps,
  "as if accumulating then applying".** Log: `camera movement update hooked at Control_DX12.exe+0x3E3BE0`. Sampler, gyro
  pan 8 s: 124 updates/s, `+0xB8` changed 131/s (one write per update now), 0 updates without a step (was: several), but
  steps / median p10 0.46, p25 0.74, p75 1.40, p90 2.09 and jitter median 0.24 (stick: 0.10): each update took the
  gyro frames added since the last one, one or two of them (the pump thread's frames come about 1.8 per update, out of
  step). The sensor pacing can't fix that: it shapes each gyro frame's motion in sensor time, on the pump thread's clock,
  and the game samples that signal on its own.
  - Fix: the gyro frames keep running totals with QueryPerformanceCounter times; the hook takes the totals at its own
    time (the newest plus the rate over the last ~10 ms of frames for the time since, at most 20 ms) minus what it has
    applied, so a wrong guess evens out at the next update. Built (695 checks); not deployed yet (game running).
- 2026-10-30 **Control with the update hook and the resampled totals: gyro pans as even as the stick's** (sampler, gyro
  pan 8 s, DualSense Bluetooth, Steam Input off): 100 updates/s, `+0xB8` written once per update; frame-to-frame jitter
  median 0.13, p90 0.47 (stick 0.10 / 0.42; hook alone 0.24 / 0.59). A fast stretch: steps -1.086, -1.116, -1.074, -1.134,
  -1.095, -1.061, -1.065, -1.112 deg (within about 5%) while the sampled update intervals swung 5-10 ms. The overall step
  spread (p10 0.23, p90 2.91) is the pan slowing from 1.1 to 0.27 deg per update, not jitter. User's feel and a possible
  bounce when stopping (the guess ahead is at most 20 ms of motion, undone at the next update): not reported yet.
- 2026-10-30 **Control: the "uncontrolled" gyro after the update hook was the core's stick fallback, not the resampling.**
  User: frames smooth, but the gyro "jumps and moves with no control, similar to a gyro to stick with broken
  deadzones/acceleration"; the first (direct-write) build was fine. Log: `stick fallback: the game undid the gyro's camera
  writes 8 frames in a row: gyro as right-stick input` and `stick fallback off: the game keeps the gyro's writes again`
  every 1-3 s, learning stick models from the mixed motion (yaw 52-62 deg/s, curves 1.00-2.25, "10 frames late") and
  saving `StickFallbackLearned=radial,0.000,2.25,56.1,38.5,10` to the game folder's `profiles\Control_DX12.ini`. Cause:
  with the hook, `apply` only queues the turn, so `view_angles` read right after it showed the camera unmoved, which the
  fallback takes for the game undoing the write; the gyro then turned the camera twice (writes + right stick). Likely
  also behind part of the "jumps" with the hook-only build. Fix: `view_angles` adds the queued, not yet applied turn
  while the hook is in use. Built (695 checks); not deployed yet (game running). The saved stick model is wrong (learnt
  from mixed motion) and should be removed from that profile.
  - Deployed the view_angles fix and removed the bad `StickFallbackLearned=` line from the game folder's
    `profiles\Control_DX12.ini` (with the user's OK); no other saved stick model in `Gyro4All.ini` or the profiles.
- 2026-10-30 **Control with the view_angles fix: no stick fallback** (0 `the game undid the gyro's camera writes` since
  the start). Sampler, gyro pan 8 s: 109 updates/s, one write per update (122/s); steps ramp smoothly as the hand speeds
  up (-0.089, -0.131, -0.180, -0.215, -0.268, ... -1.207 deg), jitter median 0.15 (stick 0.10); one 246 ms update gap in
  the sample (a game hitch). User's feel: pending.
- 2026-10-30 **Control: the remaining gyro jitter is the job system's scheduling, which the stick doesn't show.** User:
  stick fallback fixed, "better than before but still notice some jitter". From the samplers' recordings (median
  relative change of each update's step, and of its rate per real time, against the mean of its 2+2 neighbours, while
  turning): stick step 0.024, rate 0.102: the game moves the camera by an even step per update (its frame time is smoothed
  or fixed: `coregame::global::VariableTime`, read from a global at the update's start) although the update jobs start
  5-10 ms apart. Gyro (resampled at the update's real time, fallback fixed) step 0.204, rate 0.150: the gyro's steps
  followed the jobs' start times, plus noise from the up-to-20 ms guess ahead. (gyro3's better 0.036 was the stick
  fallback mixing in stick motion.)
  - Change: the hook reads the totals at a frame clock of its own: advanced by the average update interval (EMA 0.1),
    pulled 10% per update toward real time minus 6 ms, never backwards, reset after 100 ms without updates or 50 ms off;
    the totals are interpolated between the gyro frames there (about every 4 ms, so no guess ahead; the old guess only
    if a frame is later than 6 ms). Costs about 6 ms of latency. Built (695 checks); not deployed (game running).
- 2026-10-30 **Control with the frame clock: per-update gyro steps as even as the stick's** (sampler, gyro pan; the game
  at 77 updates/s this time, 11-15 ms apart): step variation median 0.038 (stick 0.024; the previous build 0.204), rate
  variation 0.140; steps ramp evenly (-0.232, -0.406, -0.566, -0.724, -0.827, -0.977, -1.088, -1.277 deg).
  - Still 2 stick fallback switches (01:01:15, 01:01:20), with the queued turn counted in `view_angles`: the first while
    the user's right stick moved hard (RX -32511..32767), the second with the stick at rest, buttons 0x1313 (combat,
    presumably): the game itself turned the camera against the gyro for 8 frames in a row (aim assist, collision?), which
    the core's check reads as the write undone. It saved another wrong stick model
    (`StickFallbackLearned=axial,0.175,1.75,346.8,158.4,7`) to the game folder's profile.
  - Change: `view_angles` returns false while the update is hooked (the stick fallback is its only user): the hook's
    writes are part of the game's own update, so the fallback has nothing to rescue there; without the hook (another
    build, direct writes), the angles and the fallback stay. Built (695 checks); not deployed (game running).
  - Deployed; the second bad `StickFallbackLearned=` line removed from the game folder's `profiles\Control_DX12.ini`
    (user's OK). Expected in the next log: no `stick fallback` lines while the hook is in use.
- 2026-10-30 **Control: gyro aiming user-verified: "feels right"** (DualSense Bluetooth, Steam Input off, the user's
  activation on L2/L1 press; the update hook, the frame clock and no stick fallback while hooked). Log of the session
  (01:09:35 on): 0 `stick fallback` lines; the camera movement component replaced twice (01:11:10 -> picked 01:11:20,
  01:12:15 -> 01:12:23; respawns or loads), each new one picked once its `+0x40` and view quaternion were filled in.
  Not covered yet: photo mode, the free camera and cutscenes (only the mouse cursor gates the gyro), CONTROL Resonant
  (detection and right stick only), the DX11 exe, USB.

## 2026-10-30 Alan Wake 2 (Epic): no gyro frame, panel never attached

- Detected as Northlight by `data_pack2\pc\*.rmdtoc` (the CONTROL Resonant rule). Log: one PeekMessageW only, on the
  window's thread (AppWindowClass, not the DllMain thread), from `AlanWake2.exe+0x1859487`, then nothing: GyroLib never
  started, so no panel. The pump at `+0x1859481` is one PeekMessageW followed by a blocking GetMessageW /
  TranslateMessage / DispatchMessageW loop (`+0x18594a1`). CONTROL Resonant's exe has the same shape (PeekMessageW
  `+0x3fe0b0`, GetMessageW `+0x3fe12f`, `+0x3fe1df`).
- Fix: the Northlight module runs the gyro frame on its own thread, woken by each Present (`overlay::on_present`, 8 ms
  timer without one), for every Northlight game but Control (`frame_source`, as Frostbite's). `window_class()` is
  `AppWindowClass` for all of them now (WM_DEVICECHANGE goes to the process's window of that class).
- Second issue, core: the game makes a D3D11 swapchain (`dxgi.dll`) ~2.4 s before its D3D12 one and never presents to
  it. A frame running in between would attach GyroLib's static panel to it (one frontend per context), and the DX12
  panel would never come. `overlay::attach` now waits while the D3D10/D3D11 target hasn't presented since its capture
  (`panel_host::target_has_presented`) or has been idle 500 ms.
- The game reads the DualSense itself through HID (Sony's pad library, report 0x01 seen) and imports XInputGetState
  (exe import hooked). Not tested in game yet.
- Verified in game (Epic version, DualSense, Steam Input not involved): `northlight: gyro frame thread`, the panel
  attached on the D3D12 swapchain (`DX12 panel ready`), opens and closes; gyro as right stick (blind stick fallback),
  on in play and off while the mouse cursor shows. The user had to rename the game's own `msvcp140.dll` and
  `msvcp140_2.dll` (14.34.31938, shipped next to the exe) for Gyro4All to work; then `deps:` shows System32's 14.51 for
  both (the game's VCRUNTIME140/_1 14.34 stay, not known to break GyroLib). No camera hook for Alan Wake 2 yet.

## 2026-10-30 Alan Wake 2: camera recon (static, the exe)

- What carries over from Control: little directly. No `GamePlayerCameraMovementComponentState` (no vtable to scan for)
  and no match for Control's update-code pattern (`48 8D 57 40 0F 28 C4 0F C2 02 00 0F 50 C0 83 F8 0F 74 ?? 0F 11 22`).
  `content::GamePlayerCameraMovementComponent` (and its Adhesion, AdhesionLook/MoveCurves, CrouchCurves, FireMode,
  QuickTurn) is still there, but only as content (tuning data), reached through
  `bigfish::component::CameraMovementContentWrapper`. What can be reused is the method and code shape:
  `control_camera.cpp`'s turn of a look vector, the update hook with its totals, interpolation and frame clock.
- The gameplay camera is a real ECS now (`ecs::World`, `ecs::access::DefinitionComponentEntry<T, access, ...>`, access
  2 = read, 3 = write in these system signatures). Components are plain structs (no RTTI). The chain, from the system
  signatures in `.rdata`:
  - `bigfish::player::*` writes `bigfish::component::PlayerCamera` (and PlayerInput / PlayerInputFrame).
  - `bigfish::tailcamera::update` (and `::loadingUpdate`): reads PlayerInput, PlayerMode, CameraMovementContentWrapper,
    Equippable, CharacterController, Skeleton, AnimationOutput, Teleported, WorldTransformReadOnly; writes
    `bigfish::component::TailCameraState` and `coregame::component::CameraView`. The over-the-shoulder camera's own
    state, so the place to turn (as Control's `+0xB8`).
  - `bigfish::cameramixer::initialUpdate` / `loadingInitialUpdate` and two unnamed ones: write
    `bigfish::cameramixer::CameraMixerState`, `coregame::component::CameraLookDirection` and CameraView, reading
    TimelineCameras, GameAimTarget and TargetSelector. These are mixer outputs (blends, timeline cameras): writes there
    would likely be undone each frame, like Control's `+0x40` copy.
  - Others: `coregame::component::FreeCameraState` / `FreeCameraStateOverride` / `FreeCameraTransformOverride`
    (photo mode or free camera), `bigfish::camerapanning::component::CameraPanning`, `TimelineCameraOutput`.
- Exe: plain sections (`.text .rdata .data .pdata _RDATA .rsrc .reloc`), no Themida, Denuvo, VMProtect or anti-cheat
  strings: camtrace (cdb) is allowed with the user's OK.

## 2026-10-30 Alan Wake 2: the gameplay camera's source (camtrace, Epic build)

- `snap`/`vectors` (static data, 90 deg stick turn): 49 turned copies, view matrices around `exe+0x3c15d80` and a
  forward row at `exe+0x3c0d248` (struct at `exe+0x3c0d220`, rotation rows at +0x1c/+0x28/+0x34). Write test: undone.
  `bp`: written by `exe+0x24b16cb` in `exe+0x24b1630`, from a 3x3 matrix in its caller's stack frame, then distance tests
  against an array of 0x70-byte entries (a listener or visibility query): a consumer, not followed further.
- `find-vec` before and after a second turn: 1493 and 1167 copies, 842 at the same address in both (most in large
  repeating arrays, e.g. stride 0x1a0 around a `0x3c8b1c1....` block). Write tests (`--yaw 20`) of the isolated ones:
  one heap copy kept the write **and every copy read after it was 20 deg further: the source**. Others: undone, or kept
  with no effect (stale copies). The source sits in a sparse region with entity-id-like ints (`0x22a25`): an ECS
  component column, not an object with a vtable.
- `bp` on it: written every frame (still camera too) by `exe+0x191a958` (`vmovups [rsi], xmm0`) in **`exe+0x191a4b0`**
  (`.pdata` 0x191a4b0-0x191a99e), called from `exe+0x1919499` (returns to `+0x1919565`). Per frame, with rsi = the 6th
  argument (stack `[rbp+0x6f]`), a float 5th argument (`[rbp+0x67]`, dt) and the 7th/8th pointers (`[rbp+0x77]`,
  `[rbp+0x7f]`):
  - reads the unit direction at `rsi+0x00` (x, y up, z), converts it to two angles (`exe+0x1f32980`);
  - adds the stick's rates times dt: `rsi+0x90` (yaw) and `rsi+0x98` (pitch), plus a timed turn driven by `rsi+0xdc`
    (quick turn, presumably);
  - wraps the yaw into one turn and stores it at `rsi+0x8c`; clamps the pitch between two scaled globals and stores it
    at `rsi+0x94`;
  - rebuilds the direction from them (`exe+0x3a7230` twice) and stores it at `rsi+0x00`.
  So the direction is the state the game turns from each frame: a turn written into it before this function runs is
  kept, and the game applies its own pitch clamp. Same convention as Control's `+0xB8` look direction (y up, heading
  atan2(x, z)).
- Hook plan: as Control's `hk_update`, on `exe+0x191a4b0` (8 arguments: 4 integer, a float, 3 pointers), turning the
  direction at the 6th argument before calling the original. Its bytes at +0x48 (`48 8B D9 48 8B 75 6F C5 F8 10 06 C5
  FB 11 44 24`) occur once in the exe (a signature candidate). Still to check: whether it runs for other cameras too
  (one hit seen), the yaw/pitch signs in game, and what gates it (cutscenes, photo mode).
- Implemented: `aw2_camera.cpp` hooks the update (found by `48 8B D9 48 8B 75 6F C5 F8 10 06 C5 FB 11 44 24 40`, once in
  the exe, and the function around it in the unwind table) for every Northlight game but Control, at configure. The
  first camera state it runs on is turned; another one takes over once that one hasn't run for 100 ms (another one
  running at the same time is logged once). The camera counts as gameplay while the update ran in the last 100 ms (and
  no mouse cursor shows). Control's queue, frame clock, look-vector turn and code search moved to `look_queue.cpp`
  (`LookQueue`), shared by both; Control's behaviour unchanged. Not tested in game yet.
- Measured in game (the hook's `camera steps` line every 5 s, DualSense, gyro on L2/L1 as in Control): Alan Wake 2's
  camera update runs on a **fixed 30 Hz step**: dt (the 5th argument) 33.3 ms at p10/p50/p90, while it renders at ~68
  fps, so the updates come on every 2nd or 3rd rendered frame (intervals p10 ~28, p50 ~30, p90 ~44 ms). The gyro's
  step changed 0.18-0.48 of its size from update to update, with 3-5 single steps over 1.8x both neighbours (and a few
  skipped) per 5 s; while the stick turned too, the whole step varied less (0.11 with the gyro's part at 0.38). The user
  saw occasional steps, not jitter. Cause: LookQueue read the gyro's totals 6 ms behind real time (Control's frames
  were ~4 ms apart), but here Gyro4All's frames come once per Present (~15 ms), so the read was often past the newest
  frame and extrapolated from about one frame's rate.
- Fix: the delay is now at least 1.5x the average spacing of `add()` calls (Control's ~4 ms keeps 6 ms), and the hook
  passes the game's dt to `take()`, which advances the clock by it (each 33.3 ms update gets 33.3 ms of gyro; it also
  covers VRR and frame-time swings); Control, with no dt, keeps the average interval.
- Verified in game by the user after the fix: "Steps gone" (DualSense, Epic version). Measured: in steady turns
  (>200 deg per 5 s) 0-2 doubled steps per window (was up to 5) and skipped ones almost always 0; step change per update
  0.12-0.35 (the hand's own motion at 30 Hz counts in it; the stick alone: 0.027). The game's update intervals also
  changed with the scene (29/44 ms alternating, or an even 33 ms). Not covered yet: aiming, character switches, photo
  mode and cutscenes (the update stops in menus and loading; the mouse cursor gates the rest), and Control after the
  LookQueue change (its ~4 ms frames keep the 6 ms delay, so it should behave as before).
- The 5 s `camera steps` measurement was removed from the hook after the test (its method is above, to add back when needed).
- 2026-10-30 **DOOM (2016) on AMD, second log (user, build with the queue fix): cause confirmed, panel ABI 5.** Panel
  shown and used in the startup screens (presents from queue family 0). From the screen with Campaign / Multiplayer /
  SnapMap on, every present comes from **queue family 1** (`the game presents from queue ... (family 1) now ...: that
  family can't draw`), AMD's compute family (DOOM's async compute), so the earlier fix skipped the panel there: F10 then
  logged `renderer not ready` and nothing opened, in menus, the map and gameplay. The swapchain was made again twice
  (same window and size), unrelated.
  - Fix (to test on AMD): the panel draws on the game's graphics queue when the present's queue can't draw.
    - winmm.dll (`src/core/panel_vk.cpp`): `vkCreateDevice` reads the queue families' flags (logged: `queue families,
      index:flags`); the first queue the game gets from a graphics family is the device's draw queue, passed in
      `g4a_vk_present` (ABI 5: `draw_queue`, `draw_queue_family`, `concurrent` = the swapchain's sharing mode,
      `lock_queue`). As soon as presents come from another queue, the game's `vkQueueSubmit`, `vkQueueSubmit2(KHR)`,
      `vkQueueBindSparse`, `vkQueueWaitIdle` on the draw queue and `vkDeviceWaitIdle` on its device take a recursive
      lock (new hooks, import table and the interposer's exports), which gyro4all.dll also takes around its submission
      there (Vulkan requires external synchronisation of a queue). Logged once: `presents from queue ... the panel can
      draw on queue ...`.
    - gyro4all.dll (`src/panel/panel_vk.cpp`): `pick_queue` (the present's queue if it can draw, else the draw queue);
      exclusive images: three submissions per frame, release (present family -> draw family) on the present queue
      waiting on the game's semaphores, the draw on the graphics queue (acquire, draw, release back), acquire on the
      present queue, chained by semaphores, fence on the last; concurrent images: the draw alone on the graphics queue.
      Layout stays PRESENT_SRC_KHR. Frames rebuilt when the present's family changes (a second command pool and two
      semaphores per image when transferring).
  - Known gaps: a game submitting on the draw queue at the very moment the lock is switched on (the first present from
    the other queue) isn't covered; a game alternating present queues every frame would rebuild the frames each time.
  - Built, 695 checks pass. Not tested in game.
- Switch Pro Controller (shown as XInput, auto mode): works in menus and in play, but the game shows a one-time
  "controller disconnected" alert when a save loads; dismissing it, the controller keeps working. Gyro4All never hid
  it (no "not shown" decision; every poll answered by controller 0, ~2400 per 10 s in play). `XInputTrace=1`: only the
  EOS overlay calls XInput besides the exe's one import (`XInputGetState` ordinal 2, called at `exe+0x20a2a69`,
  `+0x20a2d7a` (the 4-slot scan in `exe+0x20a2cf0`), `+0x20a3a7a`); `dinput8.dll` is loaded at run time, not through an
  import. The game's IsXInputDevice (WMI sample with the Steam `28DE:11FF` exception, `exe+0x151ad29`) reads the Pro
  Controller as XInput. The alert is the game's `ActionPrimaryUserLoggedOutOrDisconnectedInputDevice` (data-driven,
  trigger not found). Left as is (user's call).

## 2026-10-30 Alan Wake Remastered (Epic): Northlight detection

- Install folder (static look, not run): one exe, `Game_f_x64_EOS.exe` (Epic; DX12 only), no `*_rmdwin*_f.dll` modules
  (not Control's modular layout) and no `data_pack2\` (not AW2 / Resonant's). Imports `XINPUT1_4.dll`, `d3d12.dll`,
  `dxgi.dll`, `xaudio2_9.dll`, `EOSSDK-Win64-Shipping.dll`, `fmod.dll`, `bink2w64.dll`; the pump is **PeekMessageA**
  (no PeekMessageW/GetMessageW names in the exe); window class `AppWindowClass`, as the other Northlight games.
- Data: the older pack layout, `data\ep999-000.rmdp` + `data\ep999-000.bin` (index, holds the build path `d:\data`),
  `ep999-001`, and per language `ep999-00x-<lang>.rmdp/.bin`; also `data\videos\*.bik` (`alan_wake_logo.bik`),
  `shaders\build\pc_DXIL\`, `codebuild.txt`, `databuild.txt`. `.keystoneBuild` (Remedy build JSON, project "Deer",
  exes `game_x64.exe`/`game_f_x64.exe`) is probably Epic-only: not used.
- Detection (`northlight_engine.cpp`, `is_aw_remastered`): `data\ep*.rmdp` without Control's coregame modules (Control
  has `data\ep100-*.rmdp` too, but is checked first). `configure` skips the AW2 camera hook search there and logs
  `Alan Wake Remastered (no camera hook yet): gyro as right stick`; frame on `frame_thread` like AW2's.
- Built, 695 checks pass. **User-verified in game: "todo bien"** (DualSense Wireless shown as XInput controller 0;
  log: `engine: Northlight (detected)`, the frame thread running, the game polling slot 0). Steam Input state not
  recorded (Epic build). The Steam build's exe name wasn't checked (detection doesn't use it).

## 2026-10-30 Alan Wake Remastered: the gameplay camera's source (camtrace, Epic build)

- Static: plain sections (`.text .rdata .data .pdata`, Bink sections), one `IsDebuggerPresent` import, no Denuvo, Themida
  or anti-cheat strings. Unlike AW2 it keeps MSVC RTTI and classic camera classes (vtables, exe-relative):
  `PlayerCamera` 0x769280 (BaseCamera > rend::PerspectiveView > rend::View, MessageReceiver at +0xd00), `TailCamera`
  0x76e560, `AttachedCamera` 0x752928, `RailCamera` 0x76a360, `RevealCamera` 0x76b8a8, `PathCamera` 0x767ab8 (all
  BasePlayerCamera), `FreeCamera` 0x75c320, `CinematicCamera` 0x756b30, `DeathCamera` 0x758270, `Character` 0x754780.
  Statics: `exe+0xb05df0` -> the PlayerCamera, `+0xb05e00` FreeCamera, `+0xb05e08` CinematicCamera, `+0xb05e18`
  DeathCamera. One TailCamera in play (others pooled: 8 Attached, 8 Path, 8 Rail).
- TailCamera: `+0x08` 3x3 rotation, rows right/up/forward (y up, heading atan2(x, z)), position `+0x2c`. Write test:
  kept ~0.3 s, then eased back to the old view over ~0.4 s (the smoothed output). `bp`: copied from a stack matrix at
  `exe+0x2f008b` in TailCamera's update `exe+0x2ef3a0` (chained .pdata chunk `exe+0x2effa8`), which builds it from a
  look direction: `exe+0x2f3d10` returns the global `[exe+0xb20980]`, `exe+0x13e350` reads its `+0x18`, then
  `exe+0x15f4d0` turns it into yaw/pitch, the pitch gets `TailCamera+0x14c` added and `exe+0x14c4c0` builds the matrix
  (plus a ~7 deg shoulder offset in yaw).
- Dead ends on the way: `Character+0xbc` (the character's world transform, its facing) and a float yaw at
  `0x6f1f650+0xfc`, written by `exe+0x23e1d0` from the PlayerCamera's matrix (a consumer); found by an angle scan
  (floats that changed by the turn, in deg/rad, twice).
- **The source: `[exe+0xb20980]`** (heap struct, no vtable): `+0x00` eye and `+0x0c` target positions, **`+0x18` the
  unit look direction** (y up, heading atan2(x, z), same convention as Control's and AW2's), `+0x24` yaw and `+0x28`
  pitch in radians (yaw = heading, pitch = -elevation; derived from `+0x18` by `exe+0x29b8a0`, write test undone).
  Write test `--yaw 20` on `+0x18`: kept, and TailCamera followed (dir -63.6 -> -43.6, TailCamera -50.6).
- Written by **`exe+0x29f1b0`** (rcx = an object holding the look struct at `+0x50`, rdx = a 3x3 rotation): dir = M x
  dir, then `exe+0x2f5390` stores it and recomputes `+0x24`/`+0x28`. So the look direction is the state the game turns
  each frame (as AW2's `exe+0x191a4b0`). Signature `48 8B DA 0F 29 74 24 30 48 8B F9 48 8D 54 24 20 48 8B 49 50 E8`
  (at +0xa, once in the exe). The global is set at `exe+0x2f3b68`/`+0x2f3b82` and read by the getter `exe+0x2f3d10`.
- Not checked yet: the pitch (write test, clamp), which cameras the look struct drives besides TailCamera (aiming,
  Attached/Rail/Path cameras, cutscenes), and whether the user saw the 20 deg jump on screen (the numbers say yes).
- Implemented: `awr_camera.cpp` hooks the look turn (`exe+0x29f1b0`, found by the signature above and the unwind
  table) for Alan Wake Remastered, at configure; the hook turns the direction at `[reader+0x50]+0x18` before the game's
  own turn (LookQueue, no dt: the average interval). The engine's AW2 and AWR paths share one `HookedCamera` table in
  `northlight_engine.cpp`. Checked before writing it: the method runs with the stick idle too (`bp --exec`, rcx a
  `PlayerInputReader`, vtable `exe+0x7694b8`, method at vtable+0xc8, called from `exe+0x180535` with rbx the Character).
- **User-verified in game: "perfecto"** (DualSense Wireless shown as XInput controller 0, Epic build; log: `look turn
  hooked at Game_f_x64_EOS.exe+0x29F1B0`, `look direction` found on entering play). Built, 695 checks pass.
