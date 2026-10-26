# **CemuX — Wii U Emulator (Dark Mode & GameBanana Modding Edition)**

[![Build Process](https://github.com/cemu-project/Cemu/actions/workflows/build.yml/badge.svg)](https://github.com/cemu-project/Cemu/actions/workflows/build.yml)
[![Discord](https://img.shields.io/discord/286429969104764928?label=Cemu&logo=discord&logoColor=FFFFFF)](https://discord.gg/5psYsup)
[![Matrix Server](https://img.shields.io/matrix/cemu:cemu.info?server_fqdn=matrix.cemu.info&label=cemu:cemu.info&logo=matrix&logoColor=FFFFFF)](https://matrix.to/#/#cemu:cemu.info)

**CemuX** is an enhanced community fork of [Cemu v2.6](https://github.com/cemu-project/Cemu) that modernizes the desktop UI and integrates first-class Wii U game modding out of the box:

### ✨ What's New in CemuX
- **Full Native Dark Mode (`System Default`, `Light`, `Dark`)**:
  - Works seamlessly on Windows 10/11 (DWM dark titlebars, `uxtheme.dll` dark menus & scrollbars) and wxWidgets 3.2.5.
  - Live-switchable from **`Options -> Theme`** or **`Options -> General settings -> UI Theme`**.
  - Theme-aware Game List, Graphic Packs tree, Title/Download Manager, Input Settings, Analog Stick preview, Memory Searcher, Logging Window, and PPC Debugger.
- **Comprehensive 3-Tab GameBanana Mod Manager & Cheats Studio**:
  - Accessible from **`Tools -> GameBanana Mod Manager...`**, **Right-clicking any game -> `Browse GameBanana mods...`**, or directly inside **Graphic Packs**.
  - **Tab 1 — Browse & Download Mods**: Pre-configured for **19 major Wii U titles** (*Breath of the Wild*, *Mario Kart 8*, *Super Smash Bros. for Wii U*, *Splatoon*, *Super Mario 3D World*, *New Super Mario Bros. U / Luigi U*, *The Wind Waker HD*, *Twilight Princess HD*, *Xenoblade Chronicles X*, *Super Mario Maker*, *Bayonetta 2*, *Hyrule Warriors*, *DKC: Tropical Freeze*, *Pikmin 3*, *Pokkén Tournament*, *Paper Mario: Color Splash*, *Yoshi's Woolly World*, *Captain Toad*, and *Wii U System*) across **9 categories** (*3D Models & Meshes*, *Textures & Skins*, *Maps, Levels & Courses*, *Gameplay Mods & Overhauls*, *Cheats, Codes & Patches*, *Audio, Music & Voice*, *UI, HUD & Menus*, *Shaders & Visual FX*). Includes live preview screenshots, HTML-cleaned descriptions, multi-file variant selection, and **1-click GraphicPack v7 auto-packaging & activation**.
  - **Tab 2 — Installed Mods & GraphicPacks Manager**: Enable/disable downloaded mods, import local `.zip` mod archives, open mod directories, or uninstall mods.
  - **Tab 3 — Cheats & Code Patches Studio**: Built-in Wii U cheat templates (*Infinite Stamina*, *Unbreakable Weapons*, *Infinite Arrows*, *Master Sword Always Charged*, *MK8 200cc Speed Boost*, *Smash Infinite Jumps*, *Splatoon Infinite Ink*, etc.) plus a live PowerPC `patches.txt` editor that compiles and activates custom cheat packs in one click.
- **11 Core Stability & UI Bug Fixes**:
  - Fixed a dangling-reference use-after-free and non-ASCII filter failure in `wxLogCtrl::PushEntry`.
  - Fixed a Win32 clipboard double-free (`GlobalFree` after `SetClipboardData`) in `SymbolListCtrl`.
  - Fixed a triple-added `wxStaticText*` sizer assertion/double-free in `EmulatedUSBDeviceFrame::AddDimensionsPage`.
  - Fixed overwritten code-region bounds and uninitialized stack buffer `tolower` UB in `DisasmCtrl`.
  - Fixed a broken wide-string preview check (`wbuffer.tellp()`) and unbounded memory scan in `RegisterCtrl`.
  - Fixed invalid `libzip` `freep` ownership and `zip_t` handle leaks in `DownloadGraphicPacksWindow`.
  - Fixed small titlebar icon resource loading (`M_WND_ICON16`) in `MainWindow` and `CURL`/`FileStream` leaks in `CemuUpdateWindow`.

---

This is the code repository of Cemu, a Wii U emulator that is able to run most Wii U games and homebrew in a playable state.
It's written in C/C++ and is being actively developed with new features and fixes.

Cemu is currently only available for 64-bit Windows, Linux & macOS devices.

### Links:
 - [Open Source Announcement](https://www.reddit.com/r/cemu/comments/wwa22c/cemu_20_announcement_linux_builds_opensource_and/)
 - [Official Website](https://cemu.info)
 - [Compatibility List/Wiki](https://wiki.cemu.info/wiki/Main_Page)
 - [Official Subreddit](https://reddit.com/r/Cemu)
 - [Official Discord](https://discord.gg/5psYsup)
 - [Official Matrix Server](https://matrix.to/#/#cemu:cemu.info)
 - [Setup Guide](https://cemu.cfw.guide)

#### Other relevant repositories:
 - [Cemu-Language](https://github.com/cemu-project/Cemu-Language)
 - [Cemu's Community Graphic Packs](https://github.com/cemu-project/cemu_graphic_packs)

## Download

You can download the latest Cemu releases for Windows, Linux and Mac from the [GitHub Releases](https://github.com/cemu-project/Cemu/releases/). For Linux you can also find Cemu on [flathub](https://flathub.org/apps/info.cemu.Cemu).

On Windows Cemu is currently only available in a portable format so no installation is required besides extracting it in a safe place.

The native macOS build is currently purely experimental and should not be considered stable or ready for issue-free gameplay. There are also known issues with degraded performance due to the use of MoltenVK and Rosetta for ARM Macs. We appreciate your patience while we improve Cemu for macOS.

Pre-2.0 releases can be found on Cemu's [changelog page](https://cemu.info/changelog.html).

## Build Instructions

To compile Cemu yourself on Windows, Linux or macOS, view [BUILD.md](/BUILD.md).

## Issues

Issues with the emulator should be filed using [GitHub Issues](https://github.com/cemu-project/Cemu/issues).  
The old bug tracker can be found at [bugs.cemu.info](https://bugs.cemu.info) and still contains relevant issues and feature suggestions.

## Contributing

Pull requests are very welcome. For easier coordination you can visit the developer discussion channel on [Discord](https://discord.gg/5psYsup) or alternatively the [Matrix Server](https://matrix.to/#/#cemu:cemu.info).
Before submitting a pull request, please read and follow our code style guidelines listed in [CODING_STYLE.md](/CODING_STYLE.md).

If coding isn't your thing, testing games and making detailed bug reports or updating the (usually outdated) compatibility wiki is also appreciated!

Questions about Cemu's software architecture can also be answered on Discord (or through the Matrix bridge).

## License
Cemu is licensed under [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.
