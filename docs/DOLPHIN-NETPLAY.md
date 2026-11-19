# Rollback NetPlay through Dolphin's NetPlay window

This branch plays YG Dolphin's rollback online the way Dolphin NetPlay always worked: no YouGame app,
account or server. One player hosts a NetPlay lobby, the others join it with the host code (or the
host's IP), the host picks vanilla Brawl and presses Start. Each player's game then opens in its own
window and plays with rollback.

## Playing

Everyone needs the **same build** (Dolphin refuses lobby players on a different version) and the
same Brawl disc (NTSC, RSBE01).

1. Set up controllers as usual: *Options > Controller Settings*, GameCube port 1.
2. **Host:** *Tools > Start NetPlay...* (or right-click Brawl in the game list > *Host with NetPlay*).
   On the *Host* tab pick Brawl, keep *Traversal Server* selected, press *Host*. Share the host code
   shown in the lobby.
3. **Everyone else:** *Tools > Start NetPlay...* > *Connect* tab, choose *Traversal Server*, paste
   the host code, press *Connect*. (*Direct Connection* with the host's IP and port also works if the
   host forwarded the port.)
4. **Host:** *Assign Controller Ports* if needed: every player who should play needs a GameCube port.
   Then press **Start**.

What happens on Start: the host's game opens first; as soon as it is up, everyone else's opens and
joins it (seats follow the lobby: host is port 1, then the others in port order, up to four). With
two players it is a normal friends room; with three or four it is YG's four-seat room.

* A player who joined the lobby late can be added while the game runs: assign them a port and press
  Start again.
* Closing your game window takes you out of the game; the lobby stays open. Quitting the lobby ends
  the game for you, and the host quitting ends it for everyone.
* Games without a YG Dolphin profile (anything but vanilla Brawl) still start the old way.
* The lobby's buffer setting doesn't apply: rollback picks its own input delay.

If a game closes on its own, the lobby says why; the engine's full log is
`<Dolphin user folder>/Logs/orca-engine.log` (File > Open User Folder).

### What every player's game starts from

Rollback games only meet when every player's engine runs the same game: same build, same Brawl
revision, same save, same per-game settings (the engine compares a fingerprint of them and refuses
a mismatch: "Your friend is running a different YG Dolphin build or setup"). So the lobby never lets
a player's own Dolphin data in: it starts the engine in `<Dolphin user folder>/Rollback`, rebuilt for
every game with

* the player's controller, hotkey and video settings (they don't change the game),
* the build's **standard save**, `Sys/Orca/Saves/<game ID>` (laid out as the save's title folder,
  `content/` and `data/`), and no save if the build has none,
* no per-game settings (`GameSettings`).

Playing Brawl offline, changing its properties or having a different save can't split players any
more. To give everyone unlocked characters and stages, replace `Data/Sys/Orca/Saves/RSBE01` with a
different save (copied from a Dolphin user folder's `Wii/title/00010000/52534245`) and ship the new
build to everyone.

## Building (Windows)

You now build two programs: the Dolphin window (`dolphin-emu`, `Dolphin.exe`) and the YG engine
(`dolphin-nogui`, `DolphinNoGUI.exe`). They land in the same `Binaries` folder, which is where the
lobby looks for the engine.

The Dolphin window needs Dolphin's Qt package, a submodule YG's own build skipped:

```
git submodule update --init --depth 1 Externals/Qt
```

Then, from a *Developer PowerShell for VS*:

```
cmake --preset visualstudio-release-x64 -DENABLE_QT=ON -DENABLE_NOGUI=ON -DUSE_SYSTEM_LIBS=OFF ^
  -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF ^
  -DUSE_DISCORD_PRESENCE=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_MGBA=OFF -DUSE_UPNP=OFF ^
  -DENABLE_VULKAN=OFF -DENABLE_CLI_TOOL=OFF
cmake --build --preset visualstudio-build-release-x64 --target dolphin-emu dolphin-nogui --parallel
```

(In PowerShell use a backtick `` ` `` instead of `^` to continue lines, or put it on one line.)

Run `build\release\x64\Binaries\Dolphin.exe`.

## How it works

`Source/Core/Core/Orca/Local/` (see `LocalProto.h` for the wire format):

* **RoomServer** runs inside the host's `NetPlayServer`. It speaks the protocol of YouGame's friends
  rooms, as YG's own room fixture (`Tools/orca/doubles-rooms.py`) does: rosters, relayed messages,
  ping/pong and the keyframe store.
* **Gateway** runs inside every player's `NetPlayClient`: a server on 127.0.0.1 that the engine uses
  as its site (`ORCA_SITE`, with `ORCA_LOCAL_ROOM=1`). Its ticket, room WebSocket and keyframe
  requests travel to the room as `MessageID::OrcaRoom` packets over the lobby's own connection, the
  one Dolphin's traversal server (or direct IP) set up. Inputs go through the host, as Dolphin
  NetPlay's always did (no direct links).
* The lobby's Start launches `DolphinNoGUI` per player with `ORCA_SESSION=1 ORCA_LOCAL_ROOM=1
  ORCA_SITE ORCA_ROOM ORCA_JOIN ORCA_NAME`, `-u <the Dolphin user folder>/Rollback -e <the game>`
  (see "What every player's game starts from").

`Tools/orca/local-room-test.cpp` exercises RoomServer and Gateway end to end with the engine's own
libcurl WebSocket and HTTP code (build notes at its top).
