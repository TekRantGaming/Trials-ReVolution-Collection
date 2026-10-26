<div align="center">

<img src="docs/images/header.jpg" alt="Trials The Recompilation Collection: all 3 games on PC" width="100%">

### Trials HD, Trials Evolution and Trials Fusion on PC, running natively, in one launcher made for the couch.

[<img src="https://img.shields.io/badge/Download-Windows%20x64-f06a12?style=for-the-badge&logo=windows&logoColor=white&labelColor=101012" alt="Download for Windows" height="44">](https://github.com/TekRantGaming/trials-recompilation-collection/releases/latest)
&nbsp;
[<img src="https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?style=for-the-badge&logo=discord&logoColor=white&labelColor=101012" alt="Join the Discord" height="44">](https://discord.gg/8eakYSvBZS)

[![Latest release](https://img.shields.io/github/v/release/TekRantGaming/trials-recompilation-collection?style=flat-square&label=release&color=f06a12&labelColor=101012)](https://github.com/TekRantGaming/trials-recompilation-collection/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/TekRantGaming/trials-recompilation-collection/total?style=flat-square&color=2a8ff2&labelColor=101012)](https://github.com/TekRantGaming/trials-recompilation-collection/releases)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-5b7a8c?style=flat-square&labelColor=101012)
![Games](https://img.shields.io/badge/games-3-f06a12?style=flat-square&labelColor=101012)

<br>

<img src="docs/images/launcher-home.jpg" alt="The launcher's Home screen" width="100%">

<img src="docs/images/trials-hd-scaffold.jpg" alt="Trials HD: the scaffolding in the warehouse" width="32%">
<img src="docs/images/trials-evolution-fire.jpg" alt="Trials Evolution: an explosion beside the track" width="32%">
<img src="docs/images/trials-fusion-city.jpg" alt="Trials Fusion: the start of a city track" width="32%">

<sub>The original Xbox 360 game code, translated to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own copy of each Xbox Live Arcade game.</sub>

</div>

<br>

## At a glance

| | | |
| :--- | :--- | :--- |
| 🏍️ **All 3 games** in one launcher | 🖥️ **Up to 4K and beyond**, at 60, 120 or an **unlimited** frame rate | ✨ **AMD FSR 1 and NVIDIA NIS** upscaling |
| 🎮 **Any controller**, or the keyboard with your own keys | 🏆 **Achievements** with notifications in game | ↩️ **Back to the launcher** any time: hold Back + Start, or Esc |
| 📦 **Install from your own packages**, found on your PC by themselves | 🔓 **The full game, always**: never the trial version | 🔄 **Updates itself** and every game |
| 💾 **Save backups** and restore in one click | 🛋️ **A console style dashboard** with sounds, made for a controller | 🪟 **Borderless fullscreen**, windowed, any monitor |
| 🚂 **Add to Steam** with its own artwork, ready for Big Picture | 💬 **[Join the Discord](https://discord.gg/8eakYSvBZS)** right from Home | 🦍 **Other TRG Projects**, with King Kong Recompiled ready to install |

## The games

<div align="center">

### Trials HD

<img src="docs/images/trials-hd-start.jpg" alt="The start of a warehouse track" width="49%">
<img src="docs/images/trials-hd-scaffold.jpg" alt="Climbing the scaffolding" width="49%">
<sub><b>Where it all began.</b> One bike, one rider and a warehouse full of obstacles, all down to throttle, brake and balance.</sub>

<br><br>

### Trials Evolution

<img src="docs/images/trials-evolution-fire.jpg" alt="Riding past an explosion" width="49%">
<img src="docs/images/trials-evolution-wheelie.jpg" alt="A wheelie over the dirt jumps" width="49%">
<sub><b>The tracks leave the warehouse.</b> Huge outdoor tracks, explosions and the Career from the D License up.</sub>

<br><br>

### Trials Fusion

<img src="docs/images/trials-fusion-city.jpg" alt="The start of a city track" width="49%">
<img src="docs/images/trials-fusion-ramp.jpg" alt="Racing up a ramp" width="49%">
<sub><b>Tricks and tracks in a bright future.</b> Futuristic cities, new bikes and the tracks of the whole Career.</sub>

</div>

<br>

## Getting started

**You need:** Windows 10 or 11 (64 bit) with a DirectX 12 graphics card, and the Xbox Live Arcade package of each game you want to play, copied from your own console (see [Which packages you need](#which-packages-you-need)).

1. Download **TrialsCollection-...-windows-x64.zip** from the [latest release](https://github.com/TekRantGaming/trials-recompilation-collection/releases/latest) and unzip it anywhere. It holds the launcher and all three games, and everything stays in that folder, so it's portable.
2. Run **trials_launcher.exe**. It opens in fullscreen on Home. Windows may warn about an unknown publisher the first time: choose **More info**, then **Run anyway**.
3. Pick a game and press **Play**. The first time, the launcher asks for your copy of that game: it finds packages on your PC by itself (or choose the package file, or a folder it was already extracted to), checks it really is that game and copies the game files next to the game.
4. Press **Play** again, and from then on it starts straight away.

While you play, **hold Back and Start** on the controller, or **hold Esc**, to come back to the launcher. **No game files are included**: they come from your own copies. Xbox Live Arcade games run as their trial unless they were bought, so the collection always unlocks the full game: please only use copies you own. Your saves are kept in `Documents\trials_hd`, `Documents\trials_evolution` and `Documents\trials_fusion`.

### Which packages you need

Each game is one file on your console's storage, in `Content\0000000000000000\<title ID>\000D0000`. Only the title ID is checked, so any copy of these games works. These are the packages the ports were made and tested with:

| Game | Title ID | Media ID | Version | Package | SHA1 | Installed |
| --- | --- | --- | --- | --- | --- | --- |
| Trials HD | `5841095A` | `22E417BF` | `0.0.1.2` | 217,092,096 bytes | `CAD16E3104B3C584773E1A3F813DF923642CBA94` | about 210 MB |
| Trials Evolution | `58410AAD` | `739182AD` | `0.0.0.3` | 1,316,257,792 bytes | `6BD8FBCDFC45911F1317D462DC24440C1F277BA1` | about 1.3 GB |
| Trials Fusion | `58411401` | `5AF34C9D` | `0.0.0.3` | 1,632,141,312 bytes | `5695B9CB78B1A1925043874DAA83BBF7564CE2AA` | about 1.6 GB |

The base games only: downloadable content and title updates are not supported yet. The launcher's install screen shows every detail for each package, MD5 and CRC32 too. To check a package yourself, run this in PowerShell and compare the result with the SHA1 above:

```powershell
Get-FileHash "C:\Games\Trials HD\6E133151FC02CFA6D6653C9AAE46D1DC8014605158" -Algorithm SHA1
```

## The launcher

Everything is set up before a game starts, with a controller, the keyboard or the mouse. Each game has its own page and its own settings.

<table>
<tr>
<td width="55%"><img src="docs/images/launcher-home.jpg" alt="Home screen"></td>
<td valign="middle">

### Home
- **All three games** side by side, with their box art and artwork
- Each game shows at a glance whether it's **ready to play**, its version and your **achievements**
- **Settings** for the launcher, and **Other TRG Projects**
- **Add to Steam**: adds the collection to your Steam library as a non Steam game, with its own artwork (cover, banner, header, logo and icon), and switches Steam Input off for it so your controller keeps working
- **Discord**: joins the official server
- Plays with a **controller**, the **keyboard** or the **mouse**, with interface sounds

</td>
</tr>
<tr>
<td valign="middle">

### Game page
- **Play**, the only place a game starts
- **Settings** and **Saves and install** for that game
- Your **achievements** so far, your **save backups** and the **release notes** of its latest version

</td>
<td width="55%"><img src="docs/images/launcher-game.jpg" alt="A game's page"></td>
</tr>
<tr>
<td><img src="docs/images/launcher-install.jpg" alt="Install from your own copy"></td>
<td valign="middle">

### Install from your own copy
- The launcher **finds packages** on your PC by itself, or you **choose the package file** or an **extracted folder**
- A card shows **exactly which package works**: title ID, media ID, version, size, SHA1, MD5 and CRC32
- It checks the package really is that game, then copies the files with a progress bar

</td>
</tr>
<tr>
<td valign="middle">

### Display and Graphics
- **Fullscreen** or **windowed**, window size, **choose the monitor**, vertical sync
- **Frame rate**: 30, **60** (the default, like the Xbox 360), 120, 144, 165, 240 or **no limit**. The games still run at the right speed
- **Upscaler**: AMD FSR 1 or NVIDIA NIS
- **Render resolution** in Trials HD from the original 720p up to 6x, also through the upscaler's Native, Quality, Balanced and Performance modes
- **FXAA** and **texture filtering** up to 16x
- **Keep the original shape** with black bars, or stretch to fill

</td>
<td width="55%"><img src="docs/images/launcher-graphics.jpg" alt="Graphics page"></td>
</tr>
<tr>
<td><img src="docs/images/launcher-controller.jpg" alt="Controller page"></td>
<td valign="middle">

### Controller and keyboard
- **Stick deadzone** and **vibration** strength
- **Remap any button** on your controller, in the menus too
- **Keyboard** play with your own key bindings

</td>
</tr>
<tr>
<td valign="middle">

### Achievements
- All the achievements of each game, with their pictures and gamerscore
- **Notifications in game** when you unlock one, with their sound on or off and its volume
- A **test button** to see and hear it

</td>
<td width="55%"><img src="docs/images/launcher-achievements.jpg" alt="Achievements page"></td>
</tr>
<tr>
<td><img src="docs/images/launcher-projects.jpg" alt="Other TRG Projects"></td>
<td valign="middle">

### Other TRG Projects
- The other **TekRant Gaming** ports, with their artwork and a link to each
- **King Kong Recompiled** downloads and plays right from here

</td>
</tr>
</table>

### Saves and updates

Each game's **Saves and install** page backs up your saves and restores them, opens the save, backup and game folders, reinstalls from your copy or removes the game. When the launcher opens it checks GitHub for new versions of itself and of each game and **installs them by themselves**, restarting into a new launcher once nothing else is going on; your games, saves and settings stay as they are. Turn that off in the launcher's own **Settings**.

## Xbox 360 vs PC

| | Xbox 360 | PC |
| --- | :---: | :---: |
| Resolution | 1280 x 720 | Trials HD up to 6x (7680 x 4320); Evolution and Fusion at their original resolution, upscaled to your screen |
| Frame rate | 60 fps | 30, 60, 120, 144, 165, 240 or no limit |
| Upscaling | none | AMD FSR 1 or NVIDIA NIS |
| Anti aliasing | console | FXAA, or higher resolutions |
| Texture filtering | console | up to 16x |
| Display | TV | borderless fullscreen or windowed, any monitor |
| Controls | Xbox 360 controller | any controller, remapping, keyboard |
| Achievements | console | notifications in game, with sound |
| Three games | three downloads | one launcher |

## Good to know

### Hotkeys

| Key | Action |
| --- | --- |
| Hold <kbd>Back</kbd> + <kbd>Start</kbd>, or hold <kbd>Esc</kbd> | back to the launcher while playing |
| <kbd>F11</kbd> | the launcher fullscreen or windowed |

### Known limits

- **Trials Evolution and Trials Fusion draw at their original resolution.** Their tracks are textured by the GPU itself, and that does not survive a higher render resolution yet; the upscaler still scales the picture to your screen.
- **No online play**: leaderboards, multiplayer and Track Central need Xbox Live.
- **Base games only** for now: no downloadable content or title updates.

### Reporting a problem

Please open an [issue](https://github.com/TekRantGaming/trials-recompilation-collection/issues) with the game, what happened, your CPU, graphics card and the resolution and frame rate you play at, or ask on the [Discord](https://discord.gg/8eakYSvBZS).

## About the games

*Trials HD* (2009), *Trials Evolution* (2012) and *Trials Fusion* (2014) were made by RedLynx. Trials HD and Trials Evolution were published by Microsoft Studios on Xbox Live Arcade, Trials Fusion by Ubisoft. Each is a physics driven motorbike game: throttle, brake and the rider's balance against tracks full of ramps, loops and explosions.

This collection runs the Xbox 360 versions natively on Windows and adds the options you would expect from a PC release.

<details>
<summary><b>Building from source (for developers)</b></summary>

See [BUILDING.md](BUILDING.md). The game code is generated from each game's own `default.xex`, so building a game needs your own copy too.

</details>

## Credits

- **Trials HD**, **Trials Evolution** and **Trials Fusion** by RedLynx, published by Microsoft Studios and Ubisoft.
- [**ReXGlue SDK**](https://github.com/rexglue/rexglue-sdk), which does the code translation and runs the games, built on the work of [**Xenia**](https://github.com/xenia-project/xenia) and [**XenonRecomp**](https://github.com/hedge-dev/XenonRecomp).
- [**Trials HD Recompiled**](https://github.com/snufflyyy/trials-hd-recompiled) by snufflyyy (MIT): the first recompilation of Trials HD, its function list, the frame smoothing above 60 FPS and the package reader. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- [**Dear ImGui**](https://github.com/ocornut/imgui), [**SDL**](https://www.libsdl.org) and [**stb**](https://github.com/nothings/stb) for the launcher.
- The [**TRG Launcher**](https://github.com/TekRantGaming/trg-launcher) (updater and settings), shared by the TekRant Gaming ports.
- Game artwork in the launcher from [**SteamGridDB**](https://www.steamgriddb.com), downloaded when the launcher runs.
- The Discord and Steam marks from [**Simple Icons**](https://simpleicons.org) (CC0). Discord and Steam are trademarks of their owners.
- Upscaling: [**AMD FidelityFX Super Resolution 1**](https://github.com/GPUOpen-Effects/FidelityFX-FSR) and the [**NVIDIA Image Scaling SDK**](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling), both MIT licensed.

> [!NOTE]
> **AI disclosure:** this collection was made almost entirely with Claude Code (Anthropic). The repository owner directed and tested the work; the AI did the analysis, code, tools and documentation.

> [!IMPORTANT]
> This project is not affiliated with or endorsed by RedLynx, Ubisoft, Microsoft, Valve or Discord. All trademarks belong to their owners. It contains no game code or assets. Do not open issues asking for game files.
