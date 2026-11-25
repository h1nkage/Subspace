# Subspace - Brawl rollback for Dolphin

Subspace is a rollback netcode core for online play of Super Smash Bros. Brawl (.iso not provided). It is a fork of the Dolphin emulator (Dolphin x64 ver 2609) using Orca netcode: the emulator saves the gamestate each frame and rewinds as necessary.

- **Play:** Download the .zip from Releases, extract it. From there it's the same as the old Brawl NetPlay.
- **To build it yourself:** Download this source, then run `build-netplay.bat` to create the .exe.
- **How it works:** [ORCA_ARCHITECTURE.md](ORCA_ARCHITECTURE.md) explains the rollback design and how it compares with Slippi and Brawlback. [ORCA.md](ORCA.md) has the engineering detail: building, testing and each subsystem.
- **License:** like Dolphin, GPL-2.0-or-later, with per-file SPDX tags; in aggregate the repository is GPLv3-compatible ([COPYING](COPYING), [LICENSES/](LICENSES)).

## Version 1.0.3 Updates
- Any player may now be assigned any port (previously the host was always forced to Port 1 even if reassigned)
- Buffer range of 1-12 (for really laggy 2v2 games)
- Added the songs Boss Battle Song 1 to Battlefield, Boss Battle Song 2 to FD, DJ KK to Smashville, and Online Practice Stage to Menu
- Signal icon updated to use Brawl's built-in sprites (blue, yellow, red) 
- Clearer error codes/Dolphin messages (e.g., "hinkage's game stopped responding" for a disconnect)

## Established Features
- Supports up to 4 player games
- Works with both (unmodified) USA Rev 1 and Rev 2 discs (currently only tested with .iso format)
- Fully unlocked save/included Gecko codes for identical offline play (includes a backup "everything unlocked" code in case save file is corrupted/missing)
- No tripping
- Random input delay fix
- Default rules set to 3 stock, 8 min, Team Attack on
- Boots to main menu (skips safety info/etc.) [offline]; boots directly to CSS [NetPlay]
- Custom controls on CSS [NetPlay only]
- Sfx/music slider locked [NetPlay only]
- The main menu's Wi-Fi button functionality is restored, and With Friends/With Anyone lead to the CSS (irrelevant for now, will matter for future matchmaking) [NetPlay only]

## Limitations for NetPlay
- Ports may not be reassigned while the game window is open
- Buffer changes do not take effect while the game window is open
- Wiimote + Nunchuk not supported
- Rumble not supported
- A player whose connection has been lost may rejoin if the host clicks "Start" again (no need to close the game window and restart); however, the disconnected player cannot be re-added mid-game
- "Solo" modes (Classic, All-Star, etc.) only accept A presses on the CSS if there are 2 players (therefore, Training mode does not work); this glitch is NOT present for offline play--so you can still play Boss Battles by yourself in case you really want to

## Future Plans
- Ranked/casual matchmaking to come later (current ETA: NONE ;^) )
- Add Wiimote + Nunchuk support by mapping inputs to be read as if they're a GCC (still wouldn't support shake to smash attack)
- Test other disc image format(s) (e.g. .rvz)
- Fix the "Solo" modes not accepting A presses on the CSS if there's only one player
- Allow disconnected player(s) to rejoin mid-game and not just on menus 
- Add separate Host/Join NetPlay buttons on Dolphin main menu (unless in-game matchmaking gets done first)

Subspace is not affiliated with or endorsed by the Dolphin project, YouGame, or Nintendo.
