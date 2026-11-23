# Subspace - Brawl rollback for Dolphin

Subspace is a rollback netcode core for online play of Super Smash Bros. Brawl Rev 1 and Rev 2 (.iso not provided). It is a fork of the Dolphin emulator using Orca netcode: an exact whole-machine snapshot each frame.

- **Play:** Download the .zip from Releases, extract it. From there it's the same as the old Brawl netplay.
- **To build it yourself:** Run `build-netplay.bat` to create the .exe, then host or join through NetPlay.
- **How it works:** [ORCA_ARCHITECTURE.md](ORCA_ARCHITECTURE.md) explains the rollback design and how it compares with Slippi and Brawlback. [ORCA.md](ORCA.md) has the engineering detail: building, testing and each subsystem.
- **License:** like Dolphin, GPL-2.0-or-later, with per-file SPDX tags; in aggregate the repository is GPLv3-compatible ([COPYING](COPYING), [LICENSES/](LICENSES)).

## 1.0.2

Supports both singles and doubles. Ranked/casual matchmaking to come later (current ETA: NONE ;^).)
Fully unlocked save/codes implemented also allow for offline play.

### Codes used

- No tripping
- Random input delay fix
- Custom controls on CSS [netplay only]
- Boot to main menu
- Play Online: the main menu's Wi-Fi button opens Brawl's online menu without connecting to WFC, and With Friends/With Anyone lead to the normal character select [netplay only]
- Default rules set to 3 stock, 8 min, team attack on
- Sound settings forced to default [netplay only]
- Everything unlocked (backup if save file is missing/corrupted)

Subspace is not affiliated with or endorsed by the Dolphin project, YouGame, or Nintendo.
