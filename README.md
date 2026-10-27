# <img src="logo.png" alt="Gyro4All logo" width="64px" />Gyro4All

*Formerly [GyroUE](https://github.com/gfgerard/Gyro4All).*

## Introduction
The purpose of this project is to provide a mod that natively enables gyro in games developed with 
(almost) any engine.

> [!NOTE]
> Tested on the [supported games](docs/GAMES.md); other games often work too (especially Unreal Engine ones).
> Feel free to [suggest other games](https://github.com/gfgerard/Gyro4All/issues/new?template=game_support.md) to be supported (or add support yourself and make a pull request).
> Bugs and crashes are possible. **Offline single-player games only**: never use it in online or anti-cheat protected
> games.


## Main Features

- **Native gyro camera**: the gyro turns the game's own camera directly in memory, on top of the sticks. No mouse
  emulation, no Steam Input needed, no mixed-input UI flicker.
- **In-game settings panel (`F10` / `start`+`select`)**: sensitivity, activation (always on, hold, toggle), invert, smoothing,
  acceleration curves, calibration.
- **Customizable sensitivity profiles** for aiming down sights, scopes and more (ADS, ALT_ADS, OTHER), on a hold or toggle button.
- **Flick stick** and an optional **camera recenter** button.
- **Stick camera** (`StickCamera=1`, off by default): Gyro4All turns the camera with the right stick itself, with no
  speed cap, for games whose own stick turn is too slow. See [Advanced](docs/ADVANCED.md#stick-camera).
- **Universal controller support**: DualSense, Switch Pro and other pads shown to the game as an Xbox controller when
  it doesn't support them.
- **DualSense to DS4** automatic emulation when the game only supports Dualshock 4.
- **Nintendo face button swap** option when emulating xinput.

## Contents

- [Installation / uninstall / update](#installation--uninstall--update)
- [Using it](#using-it)
- [Controllers the game doesn't support (universal controller support)](#controllers-the-game-doesnt-support)
- [Supported games](docs/GAMES.md): tested games and the ones we want to support next
- [Current scope](#current-scope): what Gyro4All does and doesn't do (yet)

More:

- [Problems?](docs/TROUBLESHOOTING.md): common problems and how to report one
- [Advanced](docs/ADVANCED.md): technical details, game profiles, adding [support for a new game](docs/ADVANCED.md#finding-the-camera-of-a-new-game), building from source
- [Credits and licenses](docs/CREDITS.md)

**What you need**

- Windows 10/11 64-bit.
- A controller with a gyro. Tested with a **DualSense over USB/BT, Nintendo Switch Pro controller**.
- Close other controller tools while playing (**DS4Windows, DSX, reWASD**). You can use Steam Input 
  if you want but the mod should take care of input in any game automatically.


## Installation / uninstall / update

#### Install

1. **Download** `Gyro4All-vX.Y.Z.zip` from the [Releases](https://github.com/gfgerard/GyroUE/releases) page.
2. **Find the game's real exe folder.**  Usually it's the one with a file ending in **`-Win64-Shipping.exe`**, at 
   `<Game>\<Something>\Binaries\Win64`, **not** the game's main folder (that exe is only a launcher).
   Tip: search the game folder for `Shipping.exe`. It's where other mods like Optiscaler or Reshade go.
   **RE Engine games** (Capcom, e.g. Resident Evil 7) have no `Binaries` folder: their exe is in the game's main
   folder, next to `re_chunk_000.pak`.
   
   Examples:

   | Game | Folder |
   |---|---|
   | Pacific Drive | `Pacific Drive\PenDriverPro\Binaries\Win64` |
   | Cronos: The New Dawn | `...\Cronos\Binaries\Win64` |
   | Life is Strange Remastered | `...\LIS\Binaries\Win64` |
   | Gears of War: E-Day (Game Pass) | `...\Content\FairlightConcept\Binaries\WinGDK` (rename `winmm.dll` to `version.dll`, see the note below) |
   | Resident Evil 7 biohazard | `RESIDENT EVIL 7 biohazard` (next to `re7.exe`) |

3. **Extract the zip into that folder.** You should end up with:

   ```text
   Binaries\Win64\
   ├── <Game>-Win64-Shipping.exe     (the game's exe, already there)
   ├── winmm.dll                     ← Gyro4All
   └── Gyro4All\                     ← Gyro4All folder (settings, profiles, log)
   ```

4. **Play.** Once you're in control of your character, move the controller: the camera turns. Press **`F10`**
   for the settings panel.

> [!NOTE]
> **Nothing happens?** Another mod may already use a `winmm.dll`, or another program (e.g. RivaTuner) makes the
> game load the Windows one first (.log and .ini files should appear inside the **Gyro4All** folder).
> Rename Gyro4All's `winmm.dll` to **`version.dll`** (it works under both names) and try again.

#### Update

Extract the new zip over the old files and overwrite. Your settings (`gyrolib.ini`, `Gyro4All.ini`)
are kept.

**Coming from GyroUE** (this project's former name, up to 0.8.2)? Extract the zip into the same folder and overwrite
`winmm.dll`. On the first start Gyro4All copies your GyroUE settings (`GyroUE.ini` becomes `Gyro4All.ini`,
`gyrolib.ini`) and your own profiles into `Gyro4All\`, and renames the old folder to `GyroUE.migrated`; you can delete
it once everything works. If you had renamed GyroUE's DLL to `version.dll`, delete it or rename Gyro4All's `winmm.dll`
to `version.dll` too (only one of them runs).

#### Uninstall

Delete `winmm.dll` (or `version.dll` if you renamed it) and the `Gyro4All` folder. Nothing else was
changed.

## Using it

- **Gyro is on during gameplay** and pauses by itself in menus, the pause screen, the main menu and when you alt-tab.
- **Settings panel: press `F10`** (some games only accept it from the pause menu). Change sensitivity,
  how the gyro activates (always on, hold a button or trigger, toggle), invert axes, smoothing, acceleration
  curves and more. Games with several cameras (e.g. on foot / in car) get one tab each. Settings are saved
  automatically. GyroLib features that need hooks Gyro4All doesn't have yet: see [Current scope](#current-scope).
- **Sensitivity profiles (ADS, ALT_ADS, OTHER).** Besides **Exploration** (the default, always on), the panel has
  an **ADS**, an **ALT_ADS** (for games with a second aim, e.g. a scope) and an **OTHER** tab. They are **off until
  you give them a button and a mode** in `Gyro4All\Gyro4All.ini`: until then their tab reads e.g. "ADS - disabled" and
  its description says which keys to set. An enabled profile starts with the Exploration settings
  (ALT_ADS with the ADS settings), so nothing changes until you edit its tab, and replaces them while its button is
  held, or from one press to the next. A game's profile can override these settings; restart the game after a
  change. They only apply to the main camera, not to other camera views such as a car.

  | Profile setting | Meaning |
  |---|---|
  | `AdsProfileButton=` | Button for the ADS profile: `LT RT LB RB LS RS A B X Y` (or `L2 R2 L1 R1 L3 R3 Cross Circle Square Triangle`), `Back Start Up Down Left Right Touchpad Misc`, paddles `LP1 LP2 RP1 RP2`. Empty = disabled. |
  | `AdsProfileMode=` | `Hold`: active while the button is held. `Toggle`: one press on, the next off; it also turns off on pause, menus and cutscenes, since Gyro4All can't see whether the game is still aiming. Empty = disabled. |
  | `AltAdsProfileButton=`, `AltAdsProfileMode=` | Same for the ALT_ADS profile. |
  | `OtherProfileButton=`, `OtherProfileMode=` | Same for the OTHER profile. |

  Example: `AdsProfileButton=LT` and `AdsProfileMode=Hold`.

  When two profiles are active, the one activated last wins. The button still does its normal action in the game.
- **First time: calibrate.** Leave the controller still on a table for a few seconds while in a menu or the pause
  screen. Gyro4All learns the sensor drift by itself and keeps it. The panel also has a **"Recalibrate gyro"**
  button.
- **No panel?** DirectX 11, 10 and 9 games now get the same `F10` panel too (drawn by `Gyro4All\gyro4all.dll`; DX11
  verified in Mortal Shell and Jedi: Fallen Order, DX10 and DX9 not yet). If it doesn't open, open `Gyro4All\gyrolib.ini` with Notepad while the game
  runs and save: changes apply within a second. Every setting is explained in GyroLib's
  [settings reference](https://github.com/lud-berthe/GyroLib).
- No gyro-camera movement after installation? see [add support for a new game](docs/ADVANCED.md#finding-the-camera-of-a-new-game) or [ask for a profile for it](https://github.com/gfgerard/GyroUE/issues)

### Controllers the game doesn't support

> [!NOTE]
> **version >=0.8.0**: Without Steam Input or a remmaper you can play any game Gyro4All automatically emulates XInput if needed.
> Tested with a DualSense and a Switch Pro controller (Bluetooth).

Many PC games only understand Xbox controllers (XInput). Without Steam Input/xbox emulation (e.g. games from the Epic Games Store)
a DualSense, a Switch Pro controller or a DirectInput pad then does nothing in the game, except for Gyro4All's gyro.
Gyro4All can **show your controller to the game as an Xbox controller**: buttons, sticks, triggers and rumble.

- It's automatic (`XInput=1` in `Gyro4All\Gyro4All.ini`). Gyro4All only does it when the game doesn't read the controller
  by itself and no Xbox controller is present, so games with native DualSense support, Steam Input, DS4Windows/DSX and
  real Xbox pads are left alone. In games that only support PlayStation controllers natively, other controllers
  (Switch Pro, DirectInput pads) are still shown as Xbox ones.
- A game whose own PlayStation support only knows the DualShock 4 (e.g. Mortal Shell) ignores a DualSense. Gyro4All
  notices it, shows the DualSense as an Xbox controller for that session and tells you on screen to restart: from then
  on the game sees it as a DualShock 4 (PlayStation prompts, rumble, light bar). This is saved in the game's profile.
  If after the restart the game ignores the DualShock 4 too, Gyro4All notices it, goes back to the Xbox controller at
  once and saves that instead.
- About 2 seconds after the game starts, a notice says *"Controller shown to the game as an Xbox controller"*.
- The game shows Xbox button icons.
- Settings in `Gyro4All\Gyro4All.ini` (a game's profile can override them; restart the game after a change):

  | Controller input setting | Meaning |
  |---|---|
  | `XInput=1` | `0` off, `1` auto (default), `2` always: the game gets only the Xbox controller, even if it supports the DualSense itself (Gyro4All keeps it from opening PlayStation/Nintendo controllers, so you lose PlayStation icons, adaptive triggers and haptics). Still off while a real Xbox controller, Steam Input or DS4Windows/DSX is active. |
  | `XInputNintendoLabels=0` | Face buttons: `0` by position (the bottom button is Xbox A), `1` swapped A&harr;B and X&harr;Y, so a Nintendo controller's printed A is Xbox A. Applies to any controller. |
  | `XInputTouchpadBack=1` | `1` the DualSense touchpad click also presses View (Back), like the Create button. `0` the game ignores it. |
  | `NativeFlickStick=0` | Experimental, off by default. `1` GyroLib's flick stick also works in games with their own PlayStation controller support (see [Current scope](#current-scope)). Needs `XInput=1`: with `XInput=2` the game doesn't read the controller itself (flick stick then works through the Xbox controller). `0` off. |

## Supported games

The list of tested games, and of the games and engines we want to support next, is in
[docs/GAMES.md](docs/GAMES.md).

> [!TIP]
> **Your game isn't listed?** Many games made with the tested engines work without any setup: just try it. If the camera
doesn't follow the gyro, Gyro4All tells you on screen, and [Advanced](docs/ADVANCED.md#finding-the-camera-of-a-new-game) explains how to add support.

## Current scope

Gyro4All does one thing: **turn the game's camera with the gyro**. GyroLib can do more, but some of its
features only work if the mod also hooks the game in specific ways (hide the game's own stick input, filter
its button actions, move its camera on request, draw the panel). GyroLib leaves those hooks to the mod, and
Gyro4All may no implement them yet:

| Feature | In GyroUE | Details |
|---|---|---|
| Gyro camera with calibration, smoothing, curves and an activation button | ✅ | The core of the mod. |
| Separate settings per camera (e.g. on foot / in car) | ✅ | Needs a game profile (shipped for the supported games). |
| Settings panel (`F10`) | ✅ DirectX 12 and 11; ⚠️ DirectX 10 and 9: new, not verified yet | DX12: GyroLib's own DX12 panel. DX11, DX10 and DX9: the same panel (GyroLib's static ImGui panel) drawn into the game's frames by `Gyro4All\gyro4all.dll`, loaded only in those games. Not there yet: the Steam Input mouse option (it needs GyroLib's DX12 window bridge). If the panel doesn't open, edit `gyrolib.ini`. |
| Flick stick | ✅ In Xinput mode (auto / **XInput=2** to force it), and (⚠️ experimental) in games with their own PlayStation controller support | Needs the game's own right-stick rotation suppressed. GyroUE can do that for the controller it translates to XInput ([see above](#controllers-the-game-doesnt-support)), and for a DualSense/DualShock 4 the game reads itself through Sony's pad library (`NativeFlickStick=1`, off by default, with `XInput=1`: the game keeps PlayStation icons, haptics and adaptive triggers; the flick options appear after you move the right stick once in game). Otherwise GyroLib's panel hides the flick options, and the `context.N.flick.*` keys in `gyrolib.ini` are just stored preferences, don't edit them by hand. |
| Different sensitivity when aiming down sights or scoped | ✅ | 4 different profiles configurable with hold/toggle buttons
| Gyro button long press | ✅ | A button used to hold or toggle the gyro if pressed for more than 200ms. Do not use a button that has a long press action in-game. |
| Camera recenter button | ✅ **Off by default**; ⚠️ best effort in games with a custom camera profile | Levels the camera's pitch (up/down) when you press the button you pick in the settings panel's **Recenter** row (`context.N.camera.recenter_button` in `gyrolib.ini`, one per camera); its **+** sets how long it takes (0 = instant). Reliable with the generic camera. With a game profile's custom camera (e.g. a car camera) it may stop a bit above or below level, or not work well: it's optional, just leave it Off there. The button still does its normal action in the game: pick one the game doesn't need. |