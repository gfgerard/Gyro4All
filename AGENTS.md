# AGENTS.md

This file provides guidance to AI code agents when working with code in this repository.

## Project

Gyro4All (formerly GyroUE; renamed in 0.9.0) is a gyro-aiming mod for PC games, today Unreal Engine (UE4/UE5) ones:
DualSense (and other SDL-supported pads) motion sensors turn the in-game camera, with no per-game code in the common
case. Older MODLOG entries and files from users still say GyroUE: Gyro4All migrates an old `GyroUE\` folder on its
first start and still reads `[GyroUE]` INI sections (`src/core/settings.cpp`). How it works today:
- gyro math and controller input come from **GyroLib** (https://github.com/lud-berthe/GyroLib, MIT;
  reviewed at v1.2.2, now pinned to **1.3.1** in `tools/gyrolib-sdk.sh`); we write the engine side;
- Gyro4All is a **standalone system-DLL proxy** (`winmm.dll`, which also exports all of `version.dll` and can
  be renamed to it) with its own Unreal reflection reader. It needs no UE4SS: UE4SS was only the loader of
  the first prototype (MODLOG 2026-10-08) and is not used or shipped;
- most UE games turn the camera through the reflected `PlayerController.ControlRotation`, so the generic
  view needs no profile;
- per-game differences (custom cameras, vehicles, auto-recentering) go in small per-game INI profiles
  (`profiles/<exe stem>.ini`), not code. The camera probe (`Probe=1`) finds and writes them.
- The code is split into an engine-neutral **core** (`src/core/`: loader, hooks, GyroLib wiring, XInput
  translation, native pad handling, overlay, notices, settings, the per-frame host) and one **engine module**
  per game engine (`src/engines/<engine>/`, today only `unrealEngine/`), behind the `Engine` interface
  (`src/core/engine.hpp`). Only `src/engines/unrealEngine/` knows Unreal. All modules are linked into the one
  `winmm.dll`; the engine is picked per game at startup (detection, or `Engine=` in `Gyro4All.ini`).

Read these first:
- `MODLOG.md`: the running journal and the source of truth for paths, versions, signatures, findings and
  next steps. Record every new finding there (exact class, function, property and offset names), and
  record dead ends too.
- `docs/ADVANCED.md`: how it works, the profile format, the camera probe ("Finding the camera of a new
  game") and the log reference.

## Finding the camera of a new game

Start with the engine's own route (reflection, the engine module's probe, `tools/<engine>/`). **As soon as that doesn't
find the camera** (no reflection, no known global, value searches that only turn up copies the game rewrites), switch to
`tools/camtrace/` before spending more time on whole-memory searches: find a copy of the view at a fixed address,
write-test it, and follow the writes back with one-hit hardware breakpoints until a write sticks and the view follows
(steps in its README). It found Metro Exodus's view in three breakpoints after hours of value searches had failed. Ask
the user before attaching the debugger and have them save first; never on games with anti-debugging (Themida, Denuvo).

## Ground rules

- Never commit or ship game files, extracted assets, decompiled output or GyroLib's build artifacts that
  we didn't build. A release ships only our own code (with MinHook linked in), GyroLib and their license
  notices (`Gyro4All\licenses\`).
- Ask before installing Gyro4All or any loader (e.g. UE4SS) into a game folder, editing game or registry
  settings, changing Steam Input settings, or deleting anything. Back up saves first with `um backup create`.
- Single-player / offline games without anti-cheat only (no EAC, BattlEye or kernel anti-cheat).
- Don't present untested behaviour as working. "Builds" is not "works in game"; verify in game and say
  which pad, connection (USB/BT) and Steam Input state it was tested with.
- Do not write local system paths, device hardware addresses, users or any sensitive information 
  into code comments nor *.md files
- Do not increase release version automatically. Ask for version to use if needed.

## Architecture

```
DualSense --HID--> SDL3 (inside gyrolib.dll)
          --> GyroLib: calibration, gravity fusion, gyro space, smoothing, curves, activation,
                       views (contexts), flick stick, settings panel
          --> gl_update(now, host_state, view) -> out.yaw_degrees / out.pitch_degrees
          --> Gyro4All (winmm.dll proxy), on the game thread once per frame (end of the PeekMessageW pump):
              core (src/core/): GyroLib, gate (focus, pause, cursor, view), XInput / native pad, status
              engine module (src/engines/<engine>/, through Engine): the rest; for Unreal (unrealEngine/):
                engine  <- GUObjectArray / FNamePool found by shape, reflection layout detected at runtime
                scene   <- GEngine -> GameViewport -> World -> OwningGameInstance -> LocalPlayers[0]
                           -> PlayerController, Pawn, PlayerCameraManager (view target, POV)
                host    <- focus, WorldSettings pauser, bShowMouseCursor, no gameplay view (DefaultPawn)
                view    <- the profile view whose ViewTargetClass matches the view target, else the default
                apply   -> view rotation += delta, written in memory (default: PlayerController.ControlRotation;
                           profiles: another reflected rotator, raw offsets, or WorldRotation=1)
```

Key rules:
- All GyroLib calls on **one owner thread**: the game thread, from the `PeekMessageW` hook (`dllmain.cpp`).
  The `gl_context` is created lazily on the first frame (`core/host.cpp`, `start_gyro`), never in `DllMain`;
  `DllMain` only registers hooks and reads (or creates) `Gyro4All.ini` and the profile. Exception: the DX12
  panel's render calls run on Gyro4All's own render worker thread (`overlay.cpp`), because UE moves
  `Present` between threads.
- GyroLib output is already integrated degrees: don't multiply by dt.
- Rotations are written straight into memory as degrees: float rotators in UE4, double in UE5 (the
  property's element size says which). Not through `AddYawInput`/`AddPitchInput`: UE4 multiplies them by
  `InputYawScale` (2.5; pitch -2.5) and UE5 deprecated those scales.
- Every engine memory access is guarded (`core/mem.hpp`, used by `ue.cpp`): a bad pointer returns "not
  found" instead of crashing the game.
- `src/core/` never includes anything from `src/engines/` (its CMake target has no include path there).
  Engine-specific knowledge (class names, window classes, module naming, profile view sections, probes)
  goes through `Engine` or stays in the engine module.
- One Gyro4All per install folder (session mutex): other exes in that folder that load the proxy stay plain
  system-DLL proxies.
- Steam Input may hide the physical pad or add its own gyro-to-mouse: test with it off and on, and watch
  for doubled motion.

## Layout

- `src/`: the mod, MSVC x64, CMake. `CMakeLists.txt` (the version, the `winmm` DLL target),
  `engine_registry.cpp` (the list of engine modules linked in).
- `src/core/`: engine-neutral (CMake object library `gyro_core`).
  - `dllmain.cpp`: entry point, engine selection and the per-frame `PeekMessageW` hook. `proxy.cpp`/`proxy.def`:
    forwarding to the System32 DLL with the proxy's own name (`winmm.dll` or `version.dll`).
  - `engine.hpp`/`engine.cpp`: the `Engine` interface every engine module implements, and `select_engine`.
  - `host.cpp`: the per-frame logic: GyroLib, the gyro gate (focus, pause, cursor, view), XInput/native pad,
    status line; calls the engine to read the scene and turn the view.
  - `settings.cpp`: `Gyro4All.ini` (created with the defaults) and the profile's `[Gyro4All]` overrides.
  - `hooks.cpp`: import-table hooks (regular and delay-load) on the game modules (exe plus the game's own
    DLLs of modular builds, prefix from the engine); inline MinHook hooks for protected exes.
  - `mem.cpp`: guarded memory reads/writes and the game modules' writable sections. `util.hpp`.
  - `camera_match.hpp`: which of several rotations of one camera the gyro turns (Unreal's `Match=Camera` views);
    header-only, unit-tested.
  - `xinput.cpp`: the SDL controller shown to the game as XInput. `nativepad.cpp`: the game's own
    PlayStation pad access (Sony's pad library), for `XInput=2` and native flick stick.
  - `overlay.cpp`: GyroLib's DX12 settings panel. `notify.cpp`: on-screen notices (GDI). `deps.cpp`:
    Visual C++ runtime check before loading GyroLib. `log.cpp`, `perf.cpp`.
- `src/engines/unrealEngine/`: the Unreal Engine module (object library `gyro_unreal`).
  - `ue_engine.cpp`: `UnrealEngine` (detection, scene, profile views, applying the turn, follow check).
  - `ue_probe.cpp`: the camera probe (automatic captures, F7/F8, probe key and AutoProbe during play, live test,
    profile writing and views learned during play).
  - `ue_profile.cpp`: the profile's views (read at start and on every file change) and the views the probe adds. No
    game memory access, unit-tested.
  - `ue.cpp`: Unreal memory layout and reflection, and the probe's snapshot/diff.
- Adding an engine: create `src/engines/<engine>/` with a class implementing `Engine` (`core/engine.hpp`) and an
  `Engine& <engine>::engine()` accessor, an object library in its `CMakeLists.txt`, then `add_subdirectory` and
  link it in `src/CMakeLists.txt` and add it to `src/engine_registry.cpp`.
- `profiles/`: per-game INI profiles, `<exe stem>.ini` (format in `docs/ADVANCED.md`, "Game profiles").
- `docs/`: user documentation, shipped in the release zip.
- `games/<Game>/`: per-game modding plan (look for MODDING_PLAN.md) notes and scripts (git-ignored).
- `third_party/`: the GyroLib SDK and MinHook (git-ignored; versions in MODLOG).
- `tools/`: build, package, deploy, benchmark and test tools (see Tooling). Engine-specific debug and camera-finding
  tools go in `tools/<engine>/` (same names as `src/engines/`), each with a `README.md` of step-by-step instructions:
  `tools/reEngine/` (`tdbdump.py`, `camera_finder.lua`), `tools/unrealEngine/` (the built-in camera probe),
  `tools/unrealEngine3/` (`ue3tool.py`: UE3 reflection, scene, snapshot diff and camera write test on a running game).
  Engine-neutral: `tools/camtrace/` (`camtrace.py`: copies of the view at fixed addresses, write tests, one-hit
  hardware write breakpoints through cdb, code references and signatures).
- `tests/`: unit tests of the parts that need no game (`profile_tests.cpp`, target `gyro4all_tests`, never shipped).
- `bench/`: benchmark method and results.
- `build/release/`: installable release zips, `Gyro4All-v<version>.zip` (git-ignored, like all of `build/`).
- In a game's `Binaries\Win64`: `winmm.dll` (or `version.dll`) and `Gyro4All\` with `gyrolib.dll`,
  `profiles\`, `Gyro4All.ini` (mod settings) and `gyrolib.ini` (GyroLib settings), both created on first run,
  and the logs (`Gyro4All.log`, `Gyro4All-probe.log`).

## Tooling

- `um` (universal-modder CLI) is on PATH: `um scan "<game>"`, `um backup ...`, `um win ...` (launch,
  screenshot, drive the game), `um publish check`.
- Visual Studio 2026 Community (v18, MSVC 14.51, cl 19.51). VS's bundled `cmake` and `ninja` are on the
  user PATH (added 2026-10-08; needs an app/terminal restart to show up). `cl`/`link` are not, because
  they also need `INCLUDE`/`LIB`: run them through `tools\vsdev.cmd`, which loads `vcvars64.bat` and then
  runs its arguments, e.g. `cmd //c 'tools\vsdev.cmd cl /nologo /EHsc foo.cpp'` from Git Bash.
  CMake with the default Visual Studio generator finds MSVC by itself; `-G Ninja` needs `vsdev.cmd`.
- Python 3.13 and uv are available for helper scripts. git is available; gh is not.
- Build Gyro4All: `bash tools/build-gyro4all.sh`. Build + unit tests: `bash tools/test-gyro4all.sh`. Build + release zip: `bash tools/package-gyro4all.sh` (see
  "Releases" below). Install into any UE game:
  `bash tools/deploy-gyro4all.sh "<game>\<Project>\Binaries\Win64" [remove]` (installs `winmm.dll`; prefix
  `PROXY=version.dll` to install under that name when `winmm.dll` doesn't load or another mod owns it; Gears of War:
  E-Day needs `version.dll`).
  Sensor test tool: `tools/gyroprobe` (`gyroprobe.exe [seconds]`, `--settings`).
  RE Engine type database (TDB 69/70/81/83) from a game exe, offline: `python -I tools/reEngine/tdbdump.py "<game>\<exe>" --type
  app.PlayerCamera [--methods]`, `--find <regex>` (types), `--field <regex>` (fields), `--all <out.txt>`.
  A method's code in the running game (TDB 71+, read-only): `uv run --with numpy --with capstone python -I
  tools/reEngine/methoddump.py "<game>\<exe>" --type <class> [--method <name>]`.
- Performance: `bench/BENCHMARKS.md` (method, results, optimisation analysis); tools `tools/bench.sh`
  (modes vanilla / mod / mod-nopanel, PresentMon capture with Ctrl+Shift+F9 marks, report).
- MinHook 1.3.4 must be in `third_party/minhook-1.3.4` to build (see MODLOG 2026-10-09 for the clone command); it
  is only used for protected exes (inline hooks).
- To update GyroLib: download the release SDK into `third_party/GyroLib-<ver>/sdk`, verify
  SHA256SUMS, bump `tools/gyrolib-sdk.sh`, rebuild, and redeploy (users keep their `gyrolib.ini`).

## Releases

Every time you build a new version of Gyro4All, also produce its release zip:

1. Bump the version in `project(Gyro4All VERSION x.y.z ...)` in `src/CMakeLists.txt`. That is the
   only place it lives: the DLL's log line (`Gyro4All x.y.z in ...`) and the zip name both come from it.
   Never hard-code the version anywhere else.
2. Run `bash tools/package-gyro4all.sh`. It builds, checks that `winmm.dll` carries that exact version,
   and writes `build/release/Gyro4All-v<version>.zip`. Extract the zip into a game's
   `<Project>\Binaries\Win64` folder: `winmm.dll` plus `Gyro4All\` (`gyrolib.dll`, all `profiles/`,
   `licenses/`, and `readme/` with `README.md` and `docs/` (`TROUBLESHOOTING.md`, `ADVANCED.md`, `CREDITS.md`, `GAMES.md`), as in the repository). It ships no `gyrolib.ini` or `Gyro4All.ini` (both created on first run), so users keep
   their settings when they update.
3. Record the new version and what changed in `MODLOG.md`.

The zip is the asset to attach to a GitHub release tagged `v<version>`. Never put game files, extracted
assets or `gyrolib.ini`/`Gyro4All.ini`/logs into it; if the script's file list shows anything else, fix the script.
