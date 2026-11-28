# Subspace - Rollback for Brawl Online NetPlay

Subspace is a rollback netcode core for online play of Super Smash Bros. Brawl. It is a fork of the Dolphin emulator (Dolphin x64 ver 2609) using Orca netcode: the emulator saves the gamestate each frame and rewinds as necessary. Currently Windows only.



## How To Play:
1. Download the Subspace .zip from Releases, extract it.
2. Get a Brawl file.
3. Drag it into Subspace.
4. Click NetPlay button.
5. Host or connect.

Note: If you've never used a GameCube Controller USB Adapter on your computer, you will need:
- [Zadig](https://zadig.akeo.ie/)
- [Tutorial](https://dolphin-emu.org/docs/guides/how-use-official-gc-controller-adapter-wii-u/)

**To build it yourself:** Download this source, then run `build-subspace.bat` to create the .exe (make sure you have Visual Studio 2026 with the "Desktop development with C++" workload).



## Features

### Gameplay
- Supports up to 4 player games
- No tripping
- Random input delay fix
- Default rules set to 3 stock, 8 min, Team Attack on
- Moves stale in Training Mode [offline only]

### QoL 
- Custom controls on CSS [NetPlay only]
- Sfx/music slider locked [NetPlay only]
- Boots to main menu (skips safety info/etc.) [offline]; boots directly to CSS [NetPlay]

### Connections
- Connection indicator in top left uses Brawl's classic icon; also displays the numerical value of your ping with the slowest player in the room (e.g. in a 1v1 it's with your opp, in a 2v2 it could be with any of the other 3 players)
- Connection indicators for every other player in the room so you can see how it is from their end(s)

### Extra
- Works with both (unmodified) USA Rev 1 and Rev 2 discs (currently only tested with .iso format)
- Fully unlocked save/included Gecko codes for identical offline play (includes a backup "everything unlocked" code in case save file is corrupted/missing)
- Added the songs Boss Battle Song 1 to Battlefield, Boss Battle Song 2 to FD, DJ KK to Smashville, and Online Practice Stage to Menu
- The main menu's Wi-Fi button functionality is restored, and With Friends/With Anyone lead to the CSS (irrelevant for now, will matter for future matchmaking) [NetPlay only]


## Changes From Dolphin
- Simply drag your Brawl disc image into Subspace and you're ready to play--so easy! (You can still double-click to choose the file directory yourself if you'd like)
- GameCube Controller Adapter is now the default option in Controller Settings
- NetPlay button is now on the main menu
- "Connection Type" now defaults to "Traversal Server"
- "NetPlay" tab in Settings that lets you adjust all the new NetPlay displays/options
- NetPlay Display Names/opponent connection indicators are now shown in-game; by default they are displayed below the damage HUD, but there are a few other cool options you can try. You can also turn them off entirely--they fade when the damage HUD fades and disappear when paused so they never get in the way
- A player whose connection has been lost may rejoin if the host clicks "Start" again--no need to close the game window and restart (does not currently work mid-game)


## NetPlay/Rollback Limitations
- Wiimote + Nunchuk not supported
- Rumble not supported
- Ports may not be reassigned while the game window is open


## Future Plans
- Ranked/casual matchmaking
- Wiimote + Nunchuk support
- Allow disconnected player(s) to rejoin mid-game and not just on menus 
- Linux support
- Fix bugs


## Bugs
- Changing specific Display Name Settings while the game window is open causes certain immaterial HUD textures to stay hidden once they're disabled--closing and reopening the window fixes this
- "Solo" modes (Classic, All-Star, etc.) only accept A presses on the CSS if there are 2 players (therefore, Training mode does not work); this glitch is NOT present for offline play--so you can still use Training Mode, or play Boss Battles by yourself if you really want to


## Credits/Licenses
- **[Dolphin](https://github.com/dolphin-emu/dolphin)**: the emulator Subspace is built on.
- **YouGame's Orca**: the rollback netcode Subspace is a fork of.
- **[Brawlback](https://github.com/Brawlback-Team)**: Orca's first prototype was a fork of Brawlback's Dolphin rollback branch. Orca uses its Brawl memory addresses and findings, and one block in `Source/Core/Core/HW/Memmap.cpp` follows its code (credited there).
- **Fonts:** [Liberation Serif](https://github.com/liberationfonts/liberation-fonts) (SIL Open Font License 1.1) and Roboto (Apache License 2.0); license files are in `Sys/Subspace/Fonts`.
- **License:** like Dolphin, GPL-2.0-or-later, with per-file SPDX tags; in aggregate the repository is GPLv3-compatible ([COPYING](COPYING), [LICENSES/](LICENSES)).

Subspace is not affiliated with or endorsed by the Dolphin project, YouGame, Brawl Online (Project+ / Project Melee / P+ / PM), Brawlback, or Nintendo.
