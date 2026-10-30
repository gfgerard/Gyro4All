# Gyro4All: games

- [Supported games](#supported-games): tested and working
- [Games and engines to be tested next](#games-and-engines-to-be-tested-next): not supported yet

## Supported games

> [!TIP]
> **Your game isn't listed?** Many games made with the tested engines work without any setup: just try it. 
> In Unreal Engine if the camera doesn't follow the gyro, Gyro4All tells you on screen, and [Advanced](ADVANCED.md#finding-the-camera-of-a-new-game) explains how to add support by yourself.
> If the game isn't working you can [suggest adding support for it here](https://github.com/gfgerard/Gyro4All/issues/new?template=game_support.md)

| Game | Status | Engine | What to know |
|---|---|---|---|
| 007 First Light | ✅ Works (tested on the Steam version) | Glacier 2 | Install in `Retail`, next to `007FirstLight.exe`. |
| Alan Wake 2 | ✅ Works | Northlight | Install next to `AlanWake2.exe` in the game's main folder. Rename `msvcp140.dll` and `msvcp140_2.dll` in that folder (e.g. to `.bak`: incompatible old VC++ runtime shipped). |
| Battlefield 4 | ✅ Works (tested on the EA app version, Game Pass) | Frostbite 3 | Install next to `bf4.exe` in the game's main folder. Turn off the EA app's in-game overlay for the game (it freezes on start otherwise). Move the right stick once in game to start the gyro. On foot only so far (vehicles not tested). Campaign only: never play multiplayer with it (PunkBuster). |
| Battlefield Hardline | ✅ Works (tested on the EA app version, Game Pass) | Frostbite 3 | Install next to `bfh.exe` in the game's main folder. Move the right stick once in game to start the gyro (again after cutscenes). Off in vehicles. Campaign only: never play multiplayer with it (PunkBuster). |
| Control (2019, Ultimate Edition) | ✅ Works (tested on the Steam version, DX12) | Northlight | Install next to `Control_DX12.exe` in the game's main folder. DX11 (`Control_DX11.exe`) not tested. |
| Cronos: The New Dawn | ✅ Works | Unreal Engine 5.5.4 | |
| Crysis (2007) | ✅ Works (tested on the EA app version, 1.2.1, 64-bit) | CryEngine 2 | Install in `Bin64`, next to `Crysis64.exe`. 64-bit only. |
| Crysis Remastered | ✅ Works | CryEngine | Install in `Bin64`, next to `CrysisRemastered.exe`. Rename `MSVCP140.dll`.|
| Crysis 2 Remastered | ✅ Works | CryEngine | Install in `Bin64`, next to `Crysis2Remastered.exe`. Rename `MSVCP140.dll`.|
| Dead Island 2 | ✅ Works (tested on the Epic version) | Unreal Engine 4 | Install in `DeadIsland\Binaries\Win64`. Single-player only: never play online co-op with it. |
| Dead Space (2023) | ✅ Works (tested on the Steam version) | Frostbite | Install next to `Dead Space.exe` in the game's main folder.|
| Dishonored (Definitive Edition) | ✅ Works (tested on the Epic version) | Unreal Engine 3 | Install in `Binaries\Win64`, next to `Dishonored.exe`. |
| DOOM (2016) | ✅ Works (tested on the Steam version, Vulkan and OpenGL) | id Tech 6 | Install in the game's main folder, next to `DOOMx64vk.exe` and `DOOMx64.exe`. Gyro turns off in the pause menu, the map and during glory kills. Campaign only: never play multiplayer or SnapMap online with it. |
| DOOM Eternal | ✅ Works (tested on the Steam version) | id Tech 7 | Install next to `DOOMEternalx64vk.exe` in the game's main folder. Turn Steam Input off for the game. Campaign only: never play BattleMode online with it. |
| DOOM: The Dark Ages | ✅ Works (tested on the Steam version) | id Tech 8 | Install next to `DOOMTheDarkAges.exe` in the game's main folder. Turn Steam Input off for the game. |
| Gears of War: E-Day (Game Pass) | ✅⚠️ Campaign only, **with EasyAntiCheat disabled** | Unreal Engine 5.6.1 | Install in `Binaries\WinGDK`. The gyro starts a few seconds after the main menu appears. Never play online with it. |
| Ghostrunner | ✅ Works | Unreal Engine 4.26.2 | |
| Ghostrunner II | ✅ Works | Unreal Engine 4.27.2 | |
| Ghostwire: Tokyo | ✅ Works | Unreal Engine 4.27.2 | |
| Hitman 3 (World of Assassination) | ✅ Works (tested on the Epic version) | Glacier 2 | Install in `Retail`, next to `HITMAN3.exe`. Offline / single-player only: never play the online modes with it. |
| Hogwarts Legacy | ✅ Works | Unreal Engine 4 | Needs custom profile |
| Indiana Jones and the Great Circle | ✅ Works (tested on the Game Pass / Microsoft Store version) | Motor (id Tech 7 based) | Install next to `TheGreatCircle.exe` |
| Kena: Bridge of Spirits | ✅ Works | Unreal Engine 4 | DX12 tested |
| Kingdom Come: Deliverance | ✅ Works (tested on the Epic version, 1.9.7) | CryEngine (Warhorse's) | Install in `bin\Win64MasterMasterEpicPGO`, next to `KingdomCome.exe`, **as `version.dll`** (rename `winmm.dll`). Gyro4All shows your controller to the game as an Xbox one. `F10` panel works (DX11). |
| Life is Strange Remastered | ✅ Works | Unreal Engine 4.23.1 | DX11: the `F10` panel isn't verified in this game yet; you can always change settings in `gyrolib.ini` (see [Using it](../README.md#using-it)). |
| Luto | ✅ Works | Unreal Engine 5.5.4 | |
| Mass Effect (Legendary Edition) | ✅ Works (tested on the Steam version) | Unreal Engine 3 | Install in `Game\ME1\Binaries\Win64`. |
| Mass Effect 2 (Legendary Edition) | ✅ Works (tested on the Steam version) | Unreal Engine 3 | Install in `Game\ME2\Binaries\Win64`. |
| Mass Effect 3 (Legendary Edition) | ✅ Works (tested on the Steam version) | Unreal Engine 3 | Install in `Game\ME3\Binaries\Win64`. |
| Mass Effect: Andromeda | ✅ Works | Frostbite 3 | |
| Metro 2033 Redux | ✅ Works (tested on the Steam version) | 4A Engine | Install next to `metro.exe` in the game's main folder. |
| Metro: Last Light Redux | ✅ Works (tested on the Epic Games Store version) | 4A Engine | Install next to `metro.exe` in the game's main folder. |
| Metro Exodus Enhanced Edition | ✅ Works (tested on the Steam version) | 4A Engine | Install next to `MetroExodus.exe` in the game's main folder. |
| Monster Hunter Wilds | ⚠️ Partially tested | RE Engine | Game seems to implement only Steam Input for controller support. Functionality should be already covered by Steam. |
| Mortal Shell | ✅ Works | Unreal Engine 4.25.0 | `F10` panel works (DX11). The game's own PlayStation support only knows the DualShock 4: its profile makes Gyro4All show your DualSense (USB or Bluetooth) to it as one, with PlayStation icons, rumble and light bar. |
| Out of sight | ✅ Works | Unreal Engine 5.5.4 | |
| Robocop: Rogue city | ✅ Works | Unreal Engine 5.2.0 | |
| The Outer Worlds: Spacer's Choice Edition | ✅ Works | Unreal Engine 4.27.2 | Native DualSense support: Gyro4All leaves the controller to the game. |
| Pacific Drive | ✅ Works | Unreal Engine 4.27 | Gyro on foot **and** in the car. |
| PAYDAY 3 | ✅ Works | Unreal Engine 5.5.4 | ✅⚠️ **Always-online co-op game: use at your own risk**, in private or solo lobbies only. Its profile lets the gyro follow the camera during actions that take it over. |
| Pragmata | ✅ Works, **with [REFramework](https://github.com/praydog/REFramework)** | RE Engine | Install next to `PRAGMATA.exe` in the game's main folder, **together with a recent REFramework**: the game has Capcom's anti-tamper, which crashes the game about 30 s after start without it (an older REFramework build crashed it too). **Turn off the game's own gyro option** (it works while aiming with L2): Gyro4All's gyro also works while aiming, and the two would add up. |
| Resident Evil 2 (remake) | ✅ Works (tested on the Game Pass & Steam versions) | RE Engine | Install next to `re2.exe` in the game's main folder **as `version.dll`** (rename `winmm.dll`): this version never loads a `winmm.dll` from its folder. Gyro in free look and while aiming; off in the inventory, the pause menu, cutscenes and fixed or puzzle cameras. REFramework is not needed. The game reads the DualSense through DirectInput; Gyro4All can't show a controller as an Xbox one in this version. |
| Resident Evil 3 (remake) | ✅ Works (tested on the Game Pass version) | RE Engine | Install next to `re3.exe` in the game's main folder **as `version.dll`** (rename `winmm.dll`), as for RE2. Gyro in free look and while aiming; off in the inventory, the pause menu, cutscenes and fixed cameras. REFramework is not needed. The game reads the DualSense through DirectInput; Gyro4All can't show a controller as an Xbox one in this version. |
| Resident Evil 4 (remake) | ✅ Works | RE Engine | Install next to `re4.exe` in the game's main folder. Gyro with the player's own camera (on foot and the boat); other cameras (inventory, merchant, cutscenes) are left to the game. Tested with REFramework installed (not known to be needed). |
| Resident Evil 7 biohazard | ✅ Works | RE Engine | Install next to `re7.exe` in the game's main folder. Gyro in free look; off in cutscenes and scripted camera moments. The game has no DualSense support: its profile always shows your controller as an Xbox one (Xbox icons). Tested with Steam Input off. |
| Resident Evil Requiem (RE 9) | ✅ Works, **with [REFramework](https://github.com/praydog/REFramework)** | RE Engine | Install next to `re9.exe` in the game's main folder, **together with REFramework**: the game has Capcom's anti-tamper, which crashes the game when a mod changes it, and REFramework switches it off. Only tested with REFramework installed. Gyro in first and third person. |
| Resident Evil Village (RE 8) | ✅ Works | RE Engine | Install next to `re8.exe` in the game's main folder. Gyro in free look; off in the pause menu, the inventory, cutscenes and scripted camera moments. The game doesn't read the DualSense: Gyro4All shows it as an Xbox controller (Xbox icons). Tested with REFramework installed (not known to be needed). The third-person mode (Gold Edition) is not supported yet. |
| Returnal | ✅ Works | Unreal Engine 4.25.1 | Tip: in the settings panel, set the gyro to work only while L2/R2 is held. |
| Severed Steel | ✅ Works (tested on the demo) | Unreal Engine 4 | DX11: the `F10` panel isn't verified in this game yet. Rename `MSVCP140.dll` and `MSVCP140_2.dll` in its `Binaries\Win64` folder (e.g. to `.bak`): it ships an old VC++ runtime that GyroLib can't use. Until you do, Gyro4All leaves the gyro off and tells you so on screen. |
| Star Wars Jedi: Fallen Order | ✅ Works | Unreal Engine 4 | |
| Terminator: resistance | ✅ Works | Unreal Engine 4 | DX11: the `F10` panel isn't verified in this game yet |
| Titanfall 2 | ✅ Works (tested on the EA app version) | Source (Respawn) | Install in `bin\x64_retail`, **not** next to `Titanfall2.exe` (it never loads a `winmm.dll` from the main folder). Campaign only: never play multiplayer with it. |
| Valor Mortis | ✅ Works | Unreal Engine 5.7.3 | |
| Visage | ✅ Works | Unreal Engine 4.24.3 | DX11: the `F10` panel isn't verified in this game yet (or change settings in `gyrolib.ini`). |
| Warhammer 40,000: Space Marine 2 | ✅⚠️ Solo / private lobbies only, **with EasyAntiCheat disabled** (tested on the Steam version) | Swarm Engine | Install in `client_pc\root\bin\pc`, next to `Warhammer 40000 Space Marine 2 - Retail.exe`. Rename `msvcp140.dll` in that folder (e.g. to `.bak` incompatible VC++ old runtime shipped). |
| Wolfenstein: The New Order | ✅ Works (tested on the Game Pass / Microsoft Store and GOG versions) | id Tech 5 | Install next to `WolfNewOrder_x64.exe` in the game's main folder |
| Wolfenstein II: The New Colossus | ✅ Works (tested on the Game Pass / Microsoft Store version) | id Tech 6.5 | Install next to `NewColossus_x64vk.exe`  | 

## Games and engines to be tested next

Engine versions from [PCGamingWiki](https://www.pcgamingwiki.com/). Not supported yet. A game moves up to [Supported games](#supported-games) once it is confirmed working.

| Game | Engine | Year | Notes |
|---|---|---|---|
| Halo: The Master Chief Collection | Blam! / Saber 3D / Halo | 2014 |
| Halo: Campaign Evolved | Unreal Engine 5 | 2026 |
| Alan Wake Remastered | Northlight | 2021 |
| Borderlands 3 | Unreal Engine 4 | 2019 |
| Borderlands 4	| Unreal Engine 5 | 2025 |
| Crysis 2| CryEngine 3 | 2011 |
| Crysis 3 | CryEngine 3 | 2013 |
| Kingdom come deliverance II | CryEngine V | 2025 |
| Far Cry 4 | Dunia 2 | 2014 | |
| Far Cry 5 | Dunia 2 | 2018 | |
| Far Cry 6 | Dunia 2 | 2021 | |
| Far Cry 3 | Dunia 2 | 2012 | 32bit |
| Resident Evil Revelations / Revelations 2 | MT Framework | 2012 / 2015 | 32bit |
| Resident Evil 5 | MT Framework | 2009 | 32bit |
| Resident Evil 6 | MT Framework | 2012 | 32bit |
| BioShock | Unreal Engine 2.5 (modified) | 2007 | 32bit |
| BioShock 2 | Unreal Engine 2.5 (modified) | 2010 | 32bit |
| BioShock Infinite | Unreal Engine 3 | 2013 | 32bit |
