# Gyro4All: credits and third-party licenses

Gyro4All is released under the [MIT License](../LICENSE.txt).

## GyroLib

Gyro4All depends on and redistributes **[GyroLib](https://github.com/lud-berthe/GyroLib)** (version 1.3.1,
commit `9addfd9`), which provides all controller input, gyro processing and the settings panel.

```text
MIT License

Copyright (c) 2026 GyroLib contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Components inside `gyrolib.dll`

`gyrolib.dll` embeds or compiles in the following components. Their full notices are taken unchanged from the
GyroLib SDK (`share/doc/GyroLib/licenses/`) and ship in every Gyro4All release under `Gyro4All/licenses/`:

| Component | License | Notice file |
|---|---|---|
| [SDL 3.4.16](https://github.com/libsdl-org/SDL) (altered build by GyroLib; the alteration notice ships as `SDL-CHANGES.md`, patches are in the GyroLib SDK under `share/doc/GyroLib/sdl-changes`) | zlib | `SDL.txt`, `SDL-CHANGES.md` |
| HIDAPI (vendored in SDL) | BSD-style | `HIDAPI-LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | MIT | `ImGui.txt` |
| [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers) by Julian "Jibb" Smart | MIT | `GamepadMotion.LICENSE` |
| ProggyClean / ProggyForever fonts | MIT | `FONTS-LICENSE.txt` |
| stb by Sean Barrett | MIT / public domain | `STB-LICENSE.txt` |
| GyroLib | MIT | `LICENSE` |

`gyro4all.dll` (the settings panel of DirectX 11, 10 and 9 games) links GyroLib's static panel with the same Dear ImGui
1.92.9b, plus ImGui's Win32, DX9, DX10 and DX11 backends from that release (MIT, the same `ImGui.txt` notice), and uses
the ProggyForever font (`FONTS-LICENSE.txt`).

Gyro4All's own license is included in the same folder as `Gyro4All-LICENSE.txt`. Gyro4All's proxy DLL (`winmm.dll`) statically links
[MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2-Clause, includes Hacker Disassembler Engine by
Vyacheslav Patkov); its notice ships as `MinHook-LICENSE.txt`.

GyroLib's own provenance record is `THIRD_PARTY.md` in its documentation (included in the SDK under
`share/doc/GyroLib/`). At runtime GyroLib
extracts its embedded `SDL3.dll` to `%LOCALAPPDATA%\GyroLib\runtime\<hash>\`, verifies it, and loads it from
there. It does not replace or touch a game's own SDL.

## RE Engine reference: REFramework

The RE Engine module reads the engine's type database, `via.Application`'s per-frame steps and the static singleton
slots with the data layouts documented by **[REFramework](https://github.com/praydog/REFramework)** by praydog
(MIT License). No REFramework code or binary is included. Games with Capcom's anti-tamper (see [Games](GAMES.md)) need REFramework
installed next to Gyro4All.

## Frostbite reference: Frosty Tool Suite

The Frostbite module (work in progress) reads the engine's TypeInfo data with the layouts used by the SDK generator of
**[Frosty Tool Suite](https://github.com/FrostyToolsuite/FrostyToolsuite)** and its Dead Space fork by
[RichardHafer](https://github.com/RichardHafer/FrostyToolsuite). No Frosty code or binary is included.

## Disclaimer

Gyro4All is an unofficial fan project. It is not affiliated with or endorsed by Epic Games, Capcom, Electronic Arts, Sony Interactive
Entertainment, Valve, or the developers and publishers of any game mentioned here. Unreal Engine, RE Engine, Resident Evil, Frostbite, Dead Space, DualSense,
PlayStation and Steam are trademarks of their respective owners; they are named only to describe compatibility.
No game files, assets or decompiled code are included in this repository or its releases. Use at your own
risk, in offline single-player games only.

Development is AI assisted (Claude Code + Open Code with Qwen3.8).
