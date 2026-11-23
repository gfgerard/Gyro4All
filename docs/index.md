# <img src="https://github.com/gfgerard/Gyro4All/raw/main/logo.png" alt="Gyro4All logo" width="64px" />Gyro4All

**Gyro4All** brings native gyroscope aiming to PC games across multiple engines and renders—translating controller motion directly to the camera without relying on right-stick emulation or remappers that break native controller features.

## Quick Links

- [Supported Games & Profiles](GAMES.md) — Engine support, per-game installation paths, and notes.
- [Advanced Configuration](ADVANCED.md) — Custom camera profiles, `Gyro4All.ini` options, and diagnostic flags.
- [Troubleshooting](TROUBLESHOOTING.md) — Address any problems.
- [Credits](CREDITS.md) — Third-party licenses and disclaimers.

## Key Highlights

- **Native Camera Movement:** No Steam input or remappers nedded, no mixed input. Direct gyro-to-camera translation with zoom/FOV compensation and automatic pause behavior in menus, cutscenes, or when alt-tabbed.
- **Broad Engine Coverage:** Support across Unreal Engine (UE3, UE4, UE5), Unity (including profile generation / camera probe tools), Glacier 2, Northlight, RAGE, and more.
- **Controller Compatibility:** Support for gyro-capable controllers (such as DualSense and Switch Pro) alongside controller translation/emulation options when games expect standard XInput/DS4 devices.

## Getting Started

1. Download the latest release from the [Gyro4All Releases](https://github.com/gfgerard/Gyro4All/releases) page.
2. Optional: check the download against the release's SHA256SUMS.txt. In PowerShell: `Get-FileHash .\Gyro4All-vX.Y.Z.zip -Algorithm SHA256` must print the same hash. 
   The nightly prerelease is an automatic, untested build of the latest code; use a regular release unless you want to try a fix early.
3. Place the mod files in the target game's binary/executable folder as indicated in [Supported Games & Profiles](GAMES.md) (or rename/load as an `.asi` plugin if using an ASI loader).
4. Launch the game, open the in-game settings panel (e.g., `F10`), and tune sensitivity, activation mode, and calibration.

## Contributing & Feedback

- **Report issues or bugs:** Open an issue on [GitHub Issues](https://github.com/gfgerard/Gyro4All/issues).
- **Request a game:** Submit a game support request using the repository's issue templates.
- **Source code:** Browse the repository at [gfgerard/Gyro4All](https://github.com/gfgerard/Gyro4All).