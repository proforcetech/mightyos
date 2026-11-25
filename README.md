# MightyOS

A console-style launcher for Ubuntu with large tiles, built for a TV
("10-foot") setup and for keyboard, mouse or gamepad. See `PLAN.md` for the
full plan. Latest milestone: **M6, session** (labwc kiosk session, one
systemd scope per app, dormant mode, the Guide menu over games, crash
recovery and safe mode; see "Session"). Before that, M5 added media and
streaming, and **M4, emulators & Steam library**: system
definitions (`systems.d`), a ROM scanner with an SQLite cache, launching
through RetroArch or standalone Flatpak emulators with a per-game emulator
choice, Steam as a system (`appmanifest_*.acf`, `steam://rungameid`, exit
detection), opt-in SteamGridDB artwork, A–Z jumping in game grids and kids
profiles that only see allowed games. It builds on M3 (SDL3 gamepads,
Guide, HDMI-CEC remote) and M2 (apps, profiles, favorites, artwork).

## Build

Requirements: Qt >= 6.8 (Core, Gui, Qml, Quick, Concurrent, Sql, Network, Test, QuickTest),
SDL >= 3.2 (Ubuntu: `libsdl3-dev`), CMake >= 3.21, Ninja and a C++20 compiler. At run
time the Qt SVG image plugin is needed for the icons and button glyphs
(Ubuntu: `qt6-svg-plugins`) and the Qt SQLite driver for the library cache
(Ubuntu: `libqt6sql6-sqlite`; without it MightyOS runs, just without a cache).
Optional: `libsecret-1-dev` at build time stores the SteamGridDB API key in
the Secret Service instead of a 0600 file (`-DMIGHTYOS_WITH_LIBSECRET=OFF`
forces the file). Optional but needed for the Guide menu over a running game
in the session: `liblayershellqtinterface-dev` (pulls `layer-shell-qt`;
`-DMIGHTYOS_WITH_LAYERSHELL=OFF` leaves it out), see "Session". HDMI-CEC uses the kernel headers
(`linux/cec.h`) only; libcec is not used.

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Ninja compile jobs are capped by RAM (about one job per GB, see
`MIGHTYOS_COMPILE_JOBS`), because Qt's AOT-compiled QML is memory-hungry.
Override the cap with `-DMIGHTYOS_COMPILE_JOBS=N`. Turn the tests off with
`-DMIGHTYOS_BUILD_TESTS=OFF`. `-DMIGHTYOS_DEV_TOOLS=OFF` removes the
development-only `--fake-cec` (input from stdin or a FIFO) and
`--mock-library` options; it is
ON while MightyOS is in development and **must be OFF in packaged builds**
(M6). Even when built in, `mightyos` refuses `--fake-cec` inside the
MightyOS session (`XDG_SESSION_DESKTOP=mightyos`).

## Run

```sh
./build/mightyos               # 1280x720 window (current default)
./build/mightyos --windowed    # same, explicit
./build/mightyos --fullscreen
QT_QPA_PLATFORM=offscreen ./build/mightyos   # headless smoke test
./build/mightyos --no-gamepad --no-cec       # keyboard and mouse only
./build/mightyos --fake-cec fifo             # fake TV remote, see "TV remote"
./build/mightyos --mock-library              # bundled fake library; games run the test app
./build/mightyos --safe-mode                 # no artwork downloads, "Safe mode" banner
./build/mightyos --check-session             # setup warnings, then exit
./build/mightyos --signal=guide              # what the panic keys run (session only)
./build/session/mightyos-session             # the whole session (labwc), from a tty
./build/mightyos-inputtest --seconds 30      # no GUI: print pads and actions
```

Development runs default to a window; `--session-mode` (what `mightyos-session`
runs, see "Session") is fullscreen. `--fullscreen` maps the window normally
first and goes fullscreen after the first frame (some wlroots compositors
abort on a fullscreen request before the surface's first commit).
`mightyos --check-session` prints warnings about the setup (risky groups,
autologin, NAutoVTs, whether the installed labwc config is root-owned,
systemd scopes, layer shell, SysRq, the CEC udev rule, ROM folders Flatpak
emulators won't get) and works over SSH. With one profile MightyOS opens straight on Home; with two or more
it asks "Who's playing?" first, with the last used profile preselected.
Each profile reopens on the screen and tile it left.

### Files

| Path | What |
|---|---|
| `$XDG_CONFIG_HOME/mightyos/settings.json` | global settings: `{ "version": 1, "rumble": true, "tvRemote": "navigation", "showAllSystems": false }` |
| `/usr/share/mightyos/systems.d/*.json`, `$XDG_CONFIG_HOME/mightyos/systems.d/*.json` | system definitions (below); the user's replace the installed ones by id |
| `$XDG_CONFIG_HOME/mightyos/profiles/<id>/emulators.json` | that profile's emulator choices per game / per system |
| `$XDG_CONFIG_HOME/mightyos/profiles/<id>/allowed.json` | kids profiles: `{ "version": 1, "systems": [ids], "games": [keys] }` (no file: nothing allowed) |
| `$XDG_CONFIG_HOME/mightyos/secrets/steamgriddb.key` | the SteamGridDB API key when libsecret isn't used (dir 0700, file 0600) |
| `$XDG_DATA_HOME/mightyos/profiles/<id>/saves/<system>/` | game saves and states (RetroArch, `{saves}` emulators); Flatpak RetroArch keeps them in `~/.var/app/org.libretro.RetroArch/data/mightyos/profiles/<id>/` |
| `$XDG_DATA_HOME/mightyos/artwork/` | SteamGridDB images (`<sha1(key)>.png/jpg`, `index.json`), LRU-trimmed to 256 MiB |
| `$XDG_CACHE_HOME/mightyos/library.sqlite` | ROM scan cache, shared by all profiles (safe to delete) |
| `~/ROMs/<system>/` | default ROM folders |
| `$XDG_CONFIG_HOME/mightyos/gamecontrollerdb.txt` | optional extra SDL controller mappings (regular file, max 2 MiB) |
| `$XDG_CONFIG_HOME/mightyos/profiles.json` | profiles (id, name, avatar, colour, kind, createdAt, lastUsed) |
| `$XDG_CONFIG_HOME/mightyos/profiles/<id>/favorites.json` | that profile's favorites, in order |
| `$XDG_STATE_HOME/mightyos/profiles/<id>/navstate.json` | focus path (delete it to start at Home) |
| `$XDG_STATE_HOME/mightyos/profiles/<id>/history.json` | last played + launch count per item |
| `$XDG_CACHE_HOME/mightyos/thumbs/<sha1>.png` | icon thumbnails, LRU-trimmed to 256 MiB (safe to delete) |
| `$XDG_CACHE_HOME/mightyos/apps.json` | last app scan, loaded at startup before the rescan (safe to delete) |
| `$XDG_DATA_HOME/mightyos/apps/*.json`, `/usr/share/mightyos/apps/*.json` | app manifests (below) |

The defaults are `~/.config`, `~/.local/state`, `~/.cache` and
`~/.local/share`. Every write is atomic (`QSaveFile`); unreadable or
malformed files are ignored, and a corrupt `profiles.json` is moved aside to
`profiles.json.corrupt`; an unreadable `favorites.json`/`history.json`
becomes `.corrupt`, one with an unknown (e.g. newer) format version becomes
`.unsupported`, so the next write never destroys it. `settings.json` is
moved aside (`.corrupt`) only when it doesn't parse; one from a newer
MightyOS is read (known keys) but never written, and one that can't be
read (permissions, I/O error) is left alone and not overwritten. The M1
`navstate.json` becomes the first profile's on the first M2 start.

### Apps

`AppRegistry` scans `.desktop` files in `$XDG_DATA_HOME/applications`,
`~/.local/share/flatpak/exports/share/applications`,
`/var/lib/flatpak/exports/share/applications` and every
`$XDG_DATA_DIRS/applications`, in that order. The first file with a given
desktop-file id wins, even a `Hidden` or `NoDisplay` one, which is how a user
override hides a system entry. Entries need `Type=Application` and a usable
`Exec=`. They are skipped for `Hidden`, `NoDisplay`, `OnlyShowIn`/`NotShowIn`
(desktop names from `$XDG_CURRENT_DESKTOP`, else `MightyOS`), a missing
`TryExec`/program, or `Terminal=true` (there is no terminal in a TV session).
`Exec=` is split into an argument list per the Desktop Entry spec, without
a shell. Field codes are dropped or expanded, and anything that would need a
shell (unquoted `| > $ ; ~ *`, ...), uses field codes inside quotes,
contains control characters, or is malformed is rejected. Ids are
`flatpak:<appid>` only for files inside a Flatpak export dir (from
`X-Flatpak=` or the file name); everywhere else `desktop:<id>`, even with an
`X-Flatpak=` key, so a file can't impersonate an installed Flatpak.
`Categories=` with AudioVideo, Audio, Video or TV puts an app under Media.
The registry re-scans (debounced) when those directories change, so
installing an app shows up without a restart. The last result is cached in
`apps.json` and loaded synchronously at startup, so app tiles, favorites and
saved focus positions resolve on the first frame; until the first real
scan finishes, app keys that don't resolve count as "pending", not
"unavailable". Files are opened non-blocking and checked on the fd
(regular file, size cap), so a FIFO can't hang the scan.

Manifests add apps that have no `.desktop` file (the user dir overrides the
system dir per id; the id becomes `app:<id>`):

```json
{
  "version": 1,
  "id": "moonlight",
  "name": "Moonlight",
  "exec": ["flatpak", "run", "com.moonlight_stream.Moonlight"],
  "category": "media",
  "subtitle": "Game streaming",
  "icon": "com.moonlight_stream.Moonlight"
}
```

`exec` must be an array of strings; the arguments are passed verbatim,
without a shell. `category` is `media`, `games` or `apps` (the default). `icon` is a theme icon
name or an absolute path. Files over 64 KiB, unknown versions, invalid ids
and programs that aren't found are ignored.

Logging: categories `mightyos.apps`, `.profiles`, `.favorites`,
`.history`, `.artwork`, `.library` and `.settings` print warnings only by
default; `mightyos.gamepad`, `.cec` and `.input` also print info (a
controller or remote connecting, Guide held during a game). To see what the
registry found and why entries were skipped, or every input action:

```sh
QT_LOGGING_RULES="mightyos.apps.debug=true;mightyos.apps.info=true" ./build/mightyos
QT_LOGGING_RULES="mightyos.input.debug=true" ./build/mightyos
```

Logs contain ids and counts, never command lines.

### Library (emulators and Steam)

Systems come from `systems.d`: built in (a copy of `data/systems.d`), then
`$XDG_DATA_DIRS/mightyos/systems.d` (installed: `/usr/share/...`), then
`$XDG_CONFIG_HOME/mightyos/systems.d`; a later file with the same id
replaces the earlier one, `{ "version": 1, "id": "n64", "disabled": true }`
hides one. The defaults cover NES, SNES, N64, Game Boy / Color / Advance,
DS, 3DS, GameCube, Wii, Master System, Genesis, Saturn, Dreamcast, Arcade
(FinalBurn Neo / MAME), PS1, PS2, PSP and Steam. Example:

```json
{ "version": 1, "id": "gamecube", "label": "GameCube", "fullName": "Nintendo GameCube",
  "category": "emulators", "order": 60, "icon": "disc", "tint": "#8a6bd1",
  "extensions": ["iso", "gcm", "gcz", "rvz", "ciso", "m3u"],
  "romDirs": ["~/ROMs/gamecube"],
  "emulators": [
    { "id": "dolphin", "label": "Dolphin", "type": "flatpak",
      "app": "org.DolphinEmu.dolphin-emu", "args": ["-b", "-e", "{rom}"] },
    { "id": "x", "label": "X", "type": "retroarch", "core": "dolphin" } ],
  "defaultEmulator": "dolphin" }
```

Validation is strict (unknown keys, wrong types, path-like ids, relative
ROM dirs reject the file), and a definition (and its directory) must belong
to you or root and not be writable by group or others; a user file that
replaces an installed definition is logged. Emulator types: `retroarch` (`core`; native
`retroarch` with cores from `~/.config/retroarch/cores` or
`/usr/lib/*/libretro`, else Flatpak `org.libretro.RetroArch` with cores in
its own config dir), `flatpak` (`app` + `args`) and `native` (`program` +
`args`). `{rom}` (exactly once), `{saves}` (the profile's save folder) and
`{appdata}` (a per-profile folder inside a Flatpak's own data dir, e.g.
Dolphin's `-u` user folder) are replaced only as whole arguments; there is
never a shell. RetroArch gets an `--appendconfig` from
`$XDG_STATE_HOME/mightyos/profiles/<id>/retroarch/` (Flatpak: mounted
read-only) with fullscreen, the profile's save and state folders, network
and netplay features off, and kiosk mode for kids profiles. PS1 defaults to
RetroArch's Beetle PSX HW; DuckStation (an unverified community Flatpak)
remains a choice. See PLAN section 4 for which emulators keep saves per
profile.

The cache is read at startup before the saved screen is restored, so game
grids reopen on the same game. A ROM folder that is missing (an unplugged
drive) keeps its games, marked "Not connected", instead of emptying the
grid. ROMs are scanned on a worker thread (depth 4, symlinks only if they stay
inside the ROM folder, regular files only, hidden entries skipped) and cached
in `library.sqlite`, so grids fill instantly at the next start and a re-scan
only writes what changed. Titles are cleaned from file names (region and
revision tags become details: "Legend of Zelda, The (USA) (Rev 1)" shows as
"The Legend of Zelda"). Multi-disc games: an `.m3u` is the game and hides its
discs, a `.cue` hides its tracks, "(Disc 1)/(Disc 2)" files without a
playlist become one game. Re-scans: Options > Scan for games, and
automatically when a ROM folder (top level) or a Steam library changes.

Steam: native (`~/.steam/steam`, `~/.local/share/Steam`) and Flatpak roots,
every library in `libraryfolders.vdf`, installed games only, without Proton,
runtimes and redistributables, art from Steam's own library cache. A Steam
game launches with `steam steam://rungameid/<appid>` (or `flatpak run
com.valvesoftware.Steam ...`). Because that command exits immediately, the
end of the game is detected by polling (2 s) `registry.vdf`'s
`RunningAppID` and, as a fallback, a process with `SteamAppId=<appid>` in
its environment, which must agree with the registry (PLAN D20). While Steam
is still starting the game the overlay says so; holding Guide gives the
screen back at any time during a Steam launch; after 2 minutes without the
game MightyOS returns by itself. Kids profiles: Steam is closed
(`steam -shutdown`) after the game.

Emulators / Games show only systems with games, then a "+N more" tile
(Options > Show all systems / Hide systems without games). An empty grid
says where its files go. Options on a game: Emulator… (this profile's
choice for that game; on a system tile: the default for the system),
Details. A missing emulator gives a toast like "Install Dolphin from the App
Store to play this game." Nothing is installed automatically.

Artwork (Settings > Artwork): enter your own SteamGridDB API key (a plain
text field; there is no on-screen keyboard before M7, so this needs a
keyboard). It is then shown masked only. Without a key nothing is
requested. With one, grids you open fetch box art in the background (one
request at a time, about one API call per second, exponential backoff,
paused while a game runs); failures just leave the tinted tile.

## Session (M6)

MightyOS runs as its own Wayland login session (PLAN D1, D5): **labwc** as a
locked-down kiosk compositor with MightyOS as its only client.

```
mightyos-session                    the login session (installed to bin/; may be the login shell)
  └─ /usr/bin/labwc -C share/mightyos/labwc   rc.xml (generated), menu.xml, environment
       └─ mightyos-session --run    restart loop (labwc -S), passes --previous-status
            └─ mightyos --session-mode   fullscreen, crash counter, session lock
                 └─ systemd-run --user --scope -- <app>   one scope per launch
```

Why labwc and not cage: cage 0.2.1 (Ubuntu 26.04, wlroots 0.19) aborts
(`wlr_xdg_surface_schedule_configure: Assertion 'surface->initialized'`,
cage #456/#471, fixed only in cage 0.3.1 with wlroots 0.20) when a client
asks for fullscreen before its first commit; Kodi does that and took the
session down on the test PC. labwc 0.9.3 runs MightyOS and Kodi fine, and
also gives compositor-level panic keys and layer-shell overlays.

**Install** (`cmake --install build`; use `-DCMAKE_INSTALL_PREFIX=/usr` so
display managers find the session): `bin/mightyos`, `bin/mightyos-session`,
`share/wayland-sessions/mightyos.desktop`, `share/mightyos/labwc/` and,
never applied automatically, `share/mightyos/system/` (below). Install paths
may only contain `A-Z a-z 0-9 / . _ + -` (configure fails otherwise: they
end up in quoted shell strings and labwc commands). The script always runs
`/usr/bin/labwc` with `PATH=/usr/bin:/bin`. `build/session/mightyos-session`
is the same script for the build tree (build binary, generated
`build/session/labwc/`; only this one honours `MIGHTYOS_LABWC_DIR=<dir>`
to try another labwc config).

**Starting it.** Pick "MightyOS" at a display manager's login screen, or
boot straight into it. Autologin means **anyone at the keyboard is the kiosk
user, without a password**: use a dedicated kiosk user that `mightyos
--check-session` doesn't warn about, lock its password (`sudo passwd -l
KIOSKUSER`) or give it a strong one, and install the logind drop-in (no
spare text consoles). Recommended, shell-less ways (nothing to fall into
when the session ends; `mightyos-session` refuses `-c` commands, so `su -c`,
scp and sftp fail too):

- *Login shell + getty autologin* (no display manager):
  `echo /usr/bin/mightyos-session | sudo tee -a /etc/shells`,
  `sudo usermod -s /usr/bin/mightyos-session KIOSKUSER`, then the getty
  drop-in from `share/mightyos/system/getty/autologin.conf.example`
  (replace `KIOSKUSER`) and `sudo systemctl daemon-reload`. When the session
  ends, the login ends and getty starts it again.
- *greetd*: `share/mightyos/system/greetd/config.toml.example`
  (`[initial_session] command = "/usr/bin/mightyos-session"`).
- *GDM / SDDM* autologin also works (`AutomaticLoginEnable=true` /
  `[Autologin] User=… Session=mightyos.desktop`); the session ending returns
  to the display manager.

The **dev-box alternative** used on the test PC, a line in `~/.profile`
that starts the session on tty1, is *not* shell-less: when the session
ends, the login shell carries on and leaves an interactive shell on tty1
(with autologin: for anyone at the keyboard). Fine for a development box
with your own account, not for a kiosk.

The kiosk user should be a plain user: **not** in `sudo`, `video`, `input`,
`lxd`, `incus-admin`, `docker`, `libvirt` (PLAN D8, D16). MightyOS checks at
every start: a warning in the log and one toast per new set of such groups
(never blocks). `adm` / `systemd-journal` are reported by `--check-session`
as information (they read all logs). CEC access comes from the udev rule
below instead.

**Panic keys** (labwc keybinds in `rc.xml`; nothing else in the compositor
moves, closes or switches windows, opens menus or runs programs):

| Keys | Does |
|---|---|
| Ctrl+Alt+Home | Guide menu over the running app (same as holding the gamepad's Guide); Home when nothing runs |
| Ctrl+Alt+End | close the running app now (its scope gets SIGTERM, SIGKILL 3 s later) |
| Ctrl+Alt+Backspace | leave the session: a standard profile is asked "Leave MightyOS?" (Stay is the default, the TV remote can only cancel); a kids profile gets "Ask a parent"; while an app runs, the Guide menu opens instead |

They run `<bin>/mightyos --signal=guide|close|quit`, which signals only the
session instance: the process holding the `flock` on
`$XDG_RUNTIME_DIR/mightyos/session.pid` (0600 in a 0700 dir, written after
the signal handlers are in place), and only while that pid is still a
process named `mightyos` of this user. A second MightyOS instance doesn't
get the lock and never cleans up scopes. A client using the
keyboard-shortcuts-inhibit protocol (Moonlight while it captures the
keyboard) also suppresses the keys; the gamepad Guide button always works.

**Guide menu over a game.** Hold Guide (1 s) or press Ctrl+Alt+Home while an
app runs: Resume (focused; B and Guide resume too) and Close (games and apps
ask first, Cancel focused; a web app or stream closes at once). A Steam
launch offers Stop waiting / Back to MightyOS instead; "Back to MightyOS"
only stops tracking the game, its window stays on top until it is quit. With
`liblayershellqtinterface-dev` at build time (`layer-shell-qt` at run time)
the menu is a separate wlr-layer-shell window on the overlay layer above the
game, created when it opens and destroyed when it closes; it takes the
keyboard while open and gives it back to the game on Resume, and MightyOS's
own window stays dormant underneath. Without it the menu is drawn inside
MightyOS's own window, which the game covers, so in the session Guide then
falls back to the M5 behaviour (web apps and streams close, Steam stops
tracking, otherwise a hint after the app exits). The TV remote may open the
menu and resume, never confirm a Close (PLAN D18); in "full" mode two Home
presses close a web app or stream ("Press Home again to close" shows in the
menu). The running game still sees the gamepad while the menu is open
(MightyOS never grabs devices); games that pause on focus loss (RetroArch's
default, SDL games) ignore it.

**While an app runs** MightyOS is dormant: 1.5 s after the start its screens
are hidden, the clock stops, scene-graph caches, unused pixmaps and the
artwork memory cache are released; only the gamepad thread (Guide) and CEC
stay awake. Measured on the test PC with RetroArch running: 0 CPU ticks in
30 s. Every launch runs in its own systemd user scope
(`app-mightyos-<id>-<random>.scope`; Steam launches
`app-mightyos.tracked-…`), via `systemd-run --user --scope`, argv only.
Closing an app signals the scope it is in *now*: `flatpak run <id>` moves
itself into `app-flatpak-<id>-<n>.scope`, which is accepted only for that
exact app id; if `systemctl` fails, the process group gets the signal, and
the SIGKILL escalation looks again (cgroup, `cgroup.kill`, process group).
If `systemd-run` itself fails, the launch is retried once without a scope
and scopes stay off for the run. Without a systemd user manager (WSL) apps
run in their own process group.

**Crash recovery.** The session script restarts MightyOS when it ends with a
non-zero status and passes `--previous-status=<N>`; 10 quick failures in a
row end the session. MightyOS counts as a crash a status of 128 or more (a
signal) or any non-zero status before its window was up (failed startup), in
`~/.local/state/mightyos/session.json` (0600, boot-relative times; a reboot
starts afresh). An exit code after startup is not a crash: Qt exits with 1
when the compositor goes away, i.e. at every logout. 3 crashes within 10
minutes and it starts in **safe mode**: a "Safe mode" banner, no artwork
downloads, no image decoding at all (tiles show their vector icons), no
HDMI-CEC. A clean quit, SIGTERM, SIGINT or SIGHUP (logout) flushes
navigation state and settings, stops the running app's scope and ends the
session. The running launch's scopes are recorded in `session.json`; the
next session instance stops them and any leftover `app-mightyos-*.scope`
(never `app-mightyos.tracked-*`, a Steam client may still be in use). Core
dumps of the GUI are off (`RLIMIT_CORE` 0 and not dumpable, so no ptrace by
other processes of the user either); launched apps get the original core
limit back.

**Flatpak emulators** run `flatpak run --die-with-parent` in a strict
sandbox (D30): every filesystem the app asked for is reset; only `/run/udev`
(read-only), the system's ROM folders (read-only), MightyOS's RetroArch
config dir (read-only) and a `{saves}` dir in MightyOS's own data dir
(read-write) are added; GPU and input devices only; no session bus, system
bus, ssh-agent or X11 sockets; no network. ROM folders are resolved
(realpath) and must be inside your home folder (not hidden, not the home
folder itself) or under `/media`, `/run/media/<user>`, `/mnt` or `/srv`;
`--check-session` lists the ones that aren't passed. Dolphin is built in with
`"sandbox": "app"` (its own permissions, ROM folders still read-only) for the
GameCube adapter and Wiimotes; a definition in `~/.config/mightyos/systems.d`
can't relax the sandbox (only root-owned definitions can). The emulator
picker shows "full app access" for those.

**Wayland privileges under labwc.** labwc offers every native client the
wlroots protocols a desktop uses: virtual keyboard and pointer, screencopy,
layer-shell and foreign-toplevel management. Any app MightyOS starts
natively (not sandboxed) could therefore inject input into MightyOS, read
the screen, draw over it or close windows. Flatpak apps get only what their
Wayland socket allows (the same protocols, as labwc doesn't filter by
client). M8's PIN and admin password entry must take this into account (see
PLAN section 4).

**Optional system hardening** (`share/mightyos/system/`, copy deliberately,
as root):

| File | Install to | Effect |
|---|---|---|
| `udev/70-mightyos-cec.rules` | `/etc/udev/rules.d/` | CEC adapters for the seat's user (uaccess, D16), no `video` group needed. Note: every Flatpak with `devices=all` (most do, e.g. Kodi, Chrome) can then use the CEC adapter too |
| `sysctl.d/90-mightyos-sysrq.conf` | `/etc/sysctl.d/` | `kernel.sysrq = 0` (Ubuntu's 176 lets the keyboard reboot the box) |
| `logind.conf.d/90-mightyos-vt.conf` | `/etc/systemd/logind.conf.d/` | `NAutoVTs=0`, `ReserveVT=0`: no spare text consoles |
| `apport/report-ignore/mightyos` | `/etc/apport/report-ignore/` | Apport ignores this build's `mightyos` (generated with the install prefix) |
| `getty/autologin.conf.example` | `getty@tty1` drop-in | autologin on tty1 into the shell-less session (see above) |
| `greetd/config.toml.example` | `/etc/greetd/config.toml` | greetd starting the session (see above) |

VT switching: every wlroots compositor (labwc included) switches VTs on
Ctrl+Alt+F1..F12 and labwc has no setting for it. `mightyos-session` adds
`srvrkeys:none` to `XKB_DEFAULT_OPTIONS` (keeping options you set), which
turns those keys into plain F1..F12 (checked with libxkbcommon); with the
logind drop-in there is also no login on the other VTs. Ctrl+Alt+Delete
isn't seen by the kernel while the compositor owns the keyboard.

## Test

```sh
ctest --test-dir build --output-on-failure    # all tests (offscreen)
cmake --build build --target all_qmllint      # QML lint
```

| Test | Covers |
|---|---|
| `tst_launcher` | environment allowlist, launch/exit/exit code, crash (`--crash` -> -1), launch guards, process-group terminate; M6 scopes with fake `systemd-run`/`systemctl`/`flatpak` (unit names incl. tracked, argv after `--`, exit codes through the scope, SIGTERM then SIGKILL by unit, a Flatpak-moved scope of that app only, another app's scope refused, failing `systemctl` falls back to the group, failing `systemd-run` retries unscoped, recorded and stale scopes) and, where a user manager runs, a real scope whose processes ignore SIGTERM |
| `tst_session` | crash counter (only what the restart loop saw: signals and startup failures, never a logout's exit 1; window, cap, other boots, safe mode after 3, corrupt file moved aside, 0600, symlinks never followed), the session lock (flock, one holder, stale and symlinked pid files, private dir, `--signal` refuses non-MightyOS pids), warned groups and recorded units, risky/informational groups, SignalWatcher, core dumps off and the child's core limit restored, Session dormant / group warning, overlay fallback off Wayland, the generated `rc.xml` files locked down (well-formed, no defaults, Focus/Raise only and exactly the three `mightyos --signal` commands), `mightyos-session` (`sh -n`, refuses commands, the restart loop with a fake labwc and mightyos: `--previous-status`, exit 0 ends it, 10 failures give up) |
| `tst_inputrouter` | key table (incl. Ctrl+Home / HomePage = Guide), repeat policy, mute window, blocked, cross-thread `postAction` and its pending counter, `guideHeld` bypassing blocked/mute, pad style and device notices, rumble policy (bump once per hold), remote trust levels and restricted contexts, secure entry logs nothing (captured log), device-name sanitising and terminal escaping, mouse back button, text-field passthrough, cursor auto-hide |
| `tst_navstate` | save/load round trip, corrupt and stale files, path truncation, profile paths, debounce/flush |
| `tst_tilelistmodel` | model contract (roles, `indexOfKey`/`firstIndexForLetter` after reset, insert, remove, move; `get`; in-place updates), diff-based `setItems` (no reset for added/removed rows, reset fallback for reordering/duplicates), batch insert/remove, letter groups and LT/RT steps, a 20k-row diff, mock Library ids and structure |
| `tst_keyvalues` | VDF/ACF parser: Steam manifests and library folders, escapes, comments, conditionals, BOM, duplicates, malformed input, size/token/node/depth limits, 100k-deep input without recursion |
| `tst_systems` | Flatpak sandbox arguments (strict: filesystems reset, GPU + input only, no buses/ssh/X11/network; canonical ROM folders read-only, only allowed roots, symlinks resolved, rejected ones reported; our `{saves}` rw; `"sandbox": "app"` only from trusted layers, Dolphin built in), built-in definitions cover the D9 targets, strict rejection (ids, unknown keys, bidi labels, partial/unknown/duplicate placeholders, bad Flatpak ids, relative programs and dirs), whole-element substitution, layers/override/disable, symlinked definitions ignored, RetroArch native/Flatpak/core-missing/not-installed plans, Flatpak and native plans, Steam plans |
| `tst_romscanner` | title cleaning, ids, symlinks escaping the root rejected (and inner ones followed, loops broken), FIFOs skipped, depth limit, `.m3u`/`.cue`/"(Disc N)" handling, incremental reuse, launch-time path re-check, SQLite round trip and diff, bad rows and a garbage or symlinked cache file, 20k ROMs (scan, cache, rescan) |
| `tst_steam` | manifests (installed only, appid checks), tools skipped, sanitised names, root dedup through symlinks, library folders, Flatpak path mapping, old and new librarycache art, registry and process-environment checks, the run tracker (start, lagging registry, exit, timeout) and Launcher hand-over, failure and stopWaiting |
| `tst_library` | the real Library on temp dirs: models from scan, "+N more", Steam, ids never paths, the cache serving the next start, re-scan as a diff with one debounced `changed`, show all, kids profiles (nothing without allowed.json, then only what it allows), per-profile emulator choice, launching (runs, missing emulator, missing Steam, vanished file), deleting a profile keeps or deletes saves |
| `tst_scraper` | API key: validation, mask, 0600 file in a 0700 dir, refuses group/other-readable files, symlinked files and dirs; host allowlist, URL building (term is one path segment), JSON parsing of hostile responses, image validation; scraper with a scripted fetcher: no key no requests, full flow with the key only on API calls, Steam by appid, invalid images dropped, 401 stops until a new key, 429/503/offline backoff, negative cache, LRU disk budget and orphan cleanup |
| `tst_desktopentry` | `Exec=` parsing per the spec (quotes, escapes, `%f %F %u %U %i %c %k %%`, deprecated codes, rejected shell syntax and malformed values), file parsing, locales, lists |
| `tst_appregistry` | one tile for an app installed natively and as a Flatpak (whole id or exact name, never part of an id; the Flatpak's; the native one still resolves), desktop-file-id precedence, Hidden/NoDisplay/TryExec/OnlyShowIn/NotShowIn/Terminal, subdirectory ids, Flatpak ids, icon validation, oversized/broken files, manifests and their validation, async scan, watcher re-scan (incl. a dir created later), no argv in logs |
| `tst_profiles` | first run, create/rename/appearance/kind, delete (with its data) and last-profile protection, startup rule, last-used ordering, invalid ids in the file, corrupt file, failed atomic write, M1 navstate migration |
| `tst_favorites` | toggle, saved order, move, unavailable targets (and their recovery), per-profile isolation, file validation; history counts, order, per profile, cap |
| `tst_padinput` | positional button mapping, stick deadzone/hysteresis and dominant axis (1.3x margin; a jittering diagonal never flips), token bucket, trigger hysteresis, repeat timing (350 ms, 90 ms accelerating to 40 ms) with a fake clock, no burst after a stall, D-pad + stick without double steps, Guide press/hold, latching on app start/exit, InputGate: only `guideHeld` while an app runs, Guide = Home, coalescing of queued repeats (a stalled GUI thread doesn't overshoot) |
| `tst_gamepadservice` | SDL thread start/stop/restart without hangs (no controller needed), join on `aboutToQuit` and in the destructor, rumble refused while an app runs, glyph style from SDL's gamepad types, Steam virtual pad id, user mapping file (loaded; oversized and FIFO ignored without hanging) |
| `tst_padhub` | PadHub with pipes as device nodes: wake-up fd indexing across several pads and removals, the control eventfd, POLLHUP closes the fd and falls back to SDL's wait until reopened, no fd / no eventfd means SDL's wait, hidraw nodes, latching on app start and exit (and pads connected while running), the service-thread rumble target, glyph style posted before each pad's action, keyboard-like evdev nodes refused |
| `tst_cec` | CEC UI command mapping, key names, key message parsing (broadcast key frames dropped), follower replies (OSD name, power, menu, deck status, Feature Abort for unknown directed opcodes, active source only after `<Set Stream Path>` to us, nothing for broadcasts), remote repeat without acceleration, lost release stops scrolling within ~0.5 s, late refresh resumes, Root Menu, latch, the fake backend end to end into InputRouter on a fake clock (deterministic), "only Guide while running", press rate limit, removal notices (device lost, stop), FIFO and runtime-dir checks, no device and non-CEC devices refused, libcec stub |
| `tst_settings` | defaults, rumble and tvRemote round trips, unknown keys kept, parse errors moved aside, newer versions read but never written, unreadable files left alone, failed writes |
| `tst_artwork` | icon theme lookup (inheritance, best size, SVG preference, pixmaps), decode at the requested size, memory and disk cache hits, re-decode on change, untrusted specs, async responses |
| `tst_qml` | Qt Quick Test: `tst_tilerow.qml`, `tst_tilegrid.qml` (edges, bump, hover incl. a scrolled row under a resting cursor, click, wheel, paging, letter jump, icon rasterising), `tst_navstack.qml` (routing, bubbling, inert screens during transitions, system stepping, stable prompts, restoring stale paths, per-category positions), `tst_main.qml` (whole window: blocked while running, focus restore, 300 ms mute, crash toast, modal focus trap, Dialog defaults), `tst_profiles.qml` (picker only with 2+ profiles, last used preselected, switching resets and restores the stack, X toggles favorites that appear in order, reordering, unavailable favorites, a launch fills Recent, create/edit/delete in the picker), `tst_library.qml` (A–Z: typed letters and LT/RT letter steps with the letter badge, letters aren't actions; kids profiles; deleting a profile with saves defaults to keeping them; Settings > Artwork key entry: secure entry, invalid key refused, masked after saving, Esc cancels), `tst_promptbar.qml` (glyph style per device: keys, Xbox, PlayStation, Nintendo by position, remote with named colour keys; every glyph has an accessible name; theme-scaled), and in `tst_main.qml` Guide (Home from anywhere, closes menus/dialogs safely, blocked while running; held: the Guide menu, B resumes, Close asks with Cancel focused, the remote can only cancel; remote Home in the menu: navigation only never closes, full closes on the second press; dormant while an app runs, the overlay doesn't wake it, focus restored after; the Ctrl+Alt+Home / End / Backspace signals, "Leave MightyOS?" and "Ask a parent"), the Controllers vibration and TV remote toggles (not answerable from the remote), remote navigation-only mode and remote-restricted confirmations, rumble on bumps/errors only after gamepad input, connect/disconnect toasts, every Text in the window is plain text, markup in a device name is shown literally |

Tests run with `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_DATA_HOME`,
`XDG_CACHE_HOME` and `XDG_DATA_DIRS` under `build/test-xdg/`, which a ctest
fixture empties before every run, so they never touch your real files and
never see the system's `.desktop` files.

## Controls

| Action | Keyboard | Mouse | Gamepad | TV remote (HDMI-CEC) |
|---|---|---|---|---|
| Move focus | Arrow keys | Move the pointer over a tile | D-pad / left stick | Arrows |
| Select / open / play | Enter, Space | Left click (acts on the clicked tile) | A (Cross; B on Nintendo) | OK / Select, Enter |
| Back | Esc, Backspace | Back button, a breadcrumb, the "Back" prompt | B (Circle; A on Nintendo) | Back / Exit |
| Options (context menu) | Tab, Menu, Shift+F10 | Right click | Start / Menu (Options; +) | Red, Contents Menu |
| Favorite (toggle; games, apps, systems) | Insert, Ctrl+D | Context menu | X (Square; Y on Nintendo) | Blue |
| Search (M7) | `/`, Ctrl+F | | Y (Triangle; X on Nintendo) | Yellow |
| Quick panel (M8) | F12 | Click the clock | Back / View (Create; −) | Green, Setup Menu |
| Previous / next category (in a game grid: previous / next system) | PgUp / PgDn | Click a category | LB / RB (L1/R1; L/R) | Rewind / Fast forward |
| Game grids: previous / next letter (A–Z); other grids: page | Home / End | Wheel | LT / RT (L2/R2; ZL/ZR) | Channel up / down |
| Jump to a letter (game grids, All apps) | A–Z, 0–9 | | | |
| Home (Favorites) | Ctrl+Home, HomePage key | | Guide (Xbox / PS / Home button) | Home / Root Menu |
| Guide overlay while a game runs (M6) | | | hold Guide 1 s | Home / Root Menu |

Letter keys are never actions (M4): F, M and Q used to be Favorite, Options
and Quick panel, which made typing "M" in a game list open a menu. Now every
bare letter or digit jumps to the first title starting with it, and the
actions moved to keys that aren't letters.

The TV remote column assumes Settings > Controllers > TV remote = **full**.
The default is **navigation only** (move, OK, Back, Channel, Rewind /
Fast forward, Home); Options, Favorite, Search and the quick panel are then
ignored from the remote, and "off" ignores it entirely (PLAN D18). In
confirmations that change something irreversibly (power off, delete a
profile), in the Controllers dialog itself and, from M8, in secure screens
(PIN pad, admin keyboard), the remote can only cancel (Back / Home).

Gamepad buttons are **positional** (SDL3): the bottom face button is always
Select, whatever its label, so a Nintendo pad selects with B and goes back
with A, where those buttons physically are. The prompt bar shows the
buttons of the device used last: key caps, Xbox letters in their colours,
PlayStation symbols, Nintendo letters, or the remote's OK / Back / colour
keys.

- Every prompt in the bottom bar can also be clicked.
- The wheel steps the focus through the row or grid under the pointer, or
  the focused one over empty space.
- Rows and grids don't wrap around. At an edge the focus "bumps".
- Hover moves focus only after the pointer really moves. The cursor hides
  after 3 s without mouse movement and on any key press. The first move that
  brings it back doesn't steal focus.
- Only Navigate auto-repeats. A held Enter can't launch twice. A held
  D-pad, stick or remote arrow repeats after 350 ms, every 90 ms,
  speeding up to every 40 ms. The stick needs half a push (and is released
  below 40%); the triggers page once per pull.
- Dialogs and the context menu trap focus. B closes them, and dialogs start
  on (and B picks) the non-destructive button.
- LB/RB returns to Home and switches category, except in a system's game
  grid, where it steps to the previous/next system (SNES -> N64) in place.
- Apps launch with the command from their `.desktop` file or manifest.
  Games launch with their emulator or Steam (with `--mock-library`: the
  bundled test app). While something runs,
  MightyOS ignores input and shows an overlay; controllers keep working in
  the game (MightyOS never grabs them), and only a held Guide reaches
  MightyOS (it will open the Guide overlay in M6; for now a toast mentions
  it after the game). When it closes, focus is back on the same tile and
  input stays muted for 300 ms; whatever was still held in the game does
  nothing until it is released. Successful launches
  appear in the "Recent" row on Home > Favorites.
- Favorites: X / Insert toggles the focused game, app or system (a star shows on
  its tile). Home > Favorites and the Favorites screen show them in your
  order. Options > Move left / Move right reorders them. A favorite whose
  app or game is gone stays as a greyed "Unavailable" tile; Accept on it
  offers to remove it.
- Profiles: Home > Options > Switch profile, Settings > Profiles, or a click
  on the profile in the top bar. The picker creates profiles (pick a name
  from suggestions, an avatar, a colour and Standard/Kids), renames or
  re-styles them, and deletes them (after a confirmation; the last one
  can't be deleted; if it has game saves you choose between "Delete, keep
  saves" and "Delete with saves", and Back cancels). Kids profiles get a
  badge and only see the systems and games in their `allowed.json` (an
  editor arrives in M8); they have no App Store, Settings, All apps or
  Artwork, and can't open the profile picker ("Ask a parent": leaving needs
  the PIN, which arrives in M8). A kids profile used last starts without the
  picker. This is a UI restriction, not a security boundary (PLAN
  section 4). Settings > All apps lists every app the registry found.

- Controllers: any number, plugged in or out at any time (a toast says so;
  not for those present at startup). Settings > Controllers lists them and
  turns vibration on or off (default on): a short, light pulse at the end
  of a row (once per hold) and on errors, only on the controller used
  last, only after gamepad input, never while a game runs. It also sets the
  TV remote's trust level (navigation only / full / off).

Screen mockups and focus maps: [docs/ui-mockups.md](docs/ui-mockups.md).

## Gamepads

`GamepadService` runs SDL3 (`SDL_INIT_GAMEPAD` only, no window) on its own
thread and feeds `InputRouter::postAction(..., Gamepad)`. The pure mapping
and timing code is `PadInput` (`src/padinput.*`).

- **Mappings:** SDL's built-in controller database, then
  `SDL_GAMECONTROLLERCONFIG` / `SDL_GAMECONTROLLERCONFIG_FILE` from the
  environment, then `~/.config/mightyos/gamecontrollerdb.txt` if it exists
  (SDL format, e.g. lines from the community `gamecontrollerdb.txt`;
  regular file, at most 2 MiB). Nothing is downloaded. Launched games don't
  inherit the `SDL_*` variables (Launcher's environment allowlist).
- **No grabs:** SDL and MightyOS's own wake-up fds only read the device
  nodes, so games get the controller as usual. Device access comes from the
  seat (`uaccess` ACL on `/dev/input/event*`); no group membership needed
  (PLAN D16: the kiosk user is in neither `video` nor `input`).
- **Idle cost:** without a video subsystem `SDL_WaitEvent` polls every
  1 ms (about 2.5% of a core with a pad open on the test PC). The service
  (its SDL-free core is `PadHub`) therefore sleeps in `poll()` on its own
  read-only wake-up fds of the pads' nodes (`/dev/input/eventN`, or
  `/dev/hidrawN` for SDL's HIDAPI drivers: DualSense, DS4, Switch Pro, Xbox
  over Bluetooth), inotify on `/dev/input` and an eventfd, and pumps SDL
  only on input, hotplug or a due repeat/rumble: 0 CPU ticks in 30 s idle.
  While a hidraw pad rumbles, SDL is pumped every 10 ms. evdev nodes that
  report letter keys (keyboard-like) are refused as wake-up fds. Only a pad
  without any usable wake-up fd (or a node that hung up and hasn't come
  back) makes the service use `SDL_WaitEventTimeout` while it is connected;
  that is logged (`mightyos.gamepad`), and `mightyos-inputtest` shows
  `wake=sdl-poll` for such a pad.
- **Steam:** SDL hides Steam Input's virtual gamepads (28de:11ff) unless
  `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD` is set, and MightyOS
  skips them explicitly too, so a pad is never seen twice. Steam's
  *Desktop Layout* (pad -> keyboard/mouse while Steam runs) would still
  double the input as key presses; turn it off in Steam (Settings >
  Controller > Desktop Layout) until M6 handles Steam.

## TV remote (HDMI-CEC)

`CecService` reads the TV remote through the Linux CEC framework
(`/dev/cecN`). Without a `/dev/cec*` device it does nothing and logs
nothing. Most PC GPUs have no CEC on HDMI; a USB adapter such as the
Pulse-Eight works through the kernel's `pulse8-cec` driver
(`inputattach --pulse8-cec /dev/ttyACM0`); libcec is not used yet.

- **Address:** if the adapter has no logical address, MightyOS claims a
  Playback one; if all three are taken (the claim ends as Unregistered,
  15) it clears that and claims a Recording address instead. It listens as
  a non-exclusive follower. The kernel's remote-to-keyboard passthrough is
  left off; if an address configured by someone else has it on, MightyOS
  ignores CEC key messages (logged), so keys never arrive twice.
- **What goes on the bus:** after a claim the **kernel** broadcasts
  `<Report Physical Address>` and answers the core queries (`<Give Physical
  Address>`, `<Get CEC Version>`, `<Give Device Vendor ID>`). MightyOS
  itself only replies, at most 5 messages per second, and never to a
  broadcast except as below: `<Set OSD Name>` "MightyOS", power status
  "on", menu status "active", deck status "stop" (mandatory for Playback
  devices), and `<Feature Abort>` for other opcodes sent to it. **PLAN
  D17:** MightyOS never makes itself the active source (no `<Active
  Source>`, `<Image View On>` or TV power-on at startup). Only when the TV
  routes to its physical address (`<Set Stream Path>`) does it answer with
  `<Active Source>`, and it answers `<Request Active Source>` only while it
  is the active source. The opt-in setting from D17 (send `<Active
  Source>` after a local press, `<Inactive Source>` on exit) comes with the
  M8 CEC settings.
- **Trust (PLAN D18):** key frames addressed to broadcast are dropped
  (TVs address them to the device; any device on the bus could broadcast),
  presses are limited to about 10 per second, and the remote has its own
  trust level in Settings > Controllers: *navigation* (default), *full* or
  *off* (see "Controls").

Device access (**PLAN D16**): `/dev/cec*` is `root:video 0660` on Ubuntu.
MightyOS uses a seat-local udev rule, never `video` group membership; the
rule ships with the M6 session packaging. Until then, for a test with a
CEC adapter, as an admin:

```sh
echo 'SUBSYSTEM=="cec", KERNEL=="cec[0-9]*", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-mightyos-cec.rules
sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=cec
```

`uaccess` gives access only to the user on the active local seat. With it,
programs of that user can also send CEC commands to the TV and other
devices on the bus (power, input switching); the kiosk user must not be in
`video` or `input` (an install-time check in M6). MightyOS changes nothing
itself.

Fake remote for development and tests, only with `MIGHTYOS_DEV_TOOLS=ON`
(key names: `select`/`ok`, `back`/
`exit`, `up`, `down`, `left`, `right`, `home`/`root-menu`, `setup-menu`,
`contents-menu`, `red`, `green`, `yellow`, `blue`, `channel-up`,
`channel-down`, `rewind`, `fast-forward`, `0`-`9`; `+key` holds, `-key`
releases):

```sh
./build/mightyos --fake-cec fifo &         # creates $XDG_RUNTIME_DIR/mightyos-fake-cec (0600)
echo right > "$XDG_RUNTIME_DIR/mightyos-fake-cec"
printf 'up\nselect\n' | ./build/mightyos-inputtest --fake-cec --seconds 3
```

The FIFO is only used if `$XDG_RUNTIME_DIR` is your own directory with mode
0700, and the FIFO is yours with mode 0600; it is opened once, read-write,
relative to that checked directory.

## Hands-on checklist (gamepad and remote)

`mightyos-inputtest` needs no display and works over SSH. It prints one
line per controller change and per action:

1. **Detection.** `./build/mightyos-inputtest --seconds 3`. Expected, with
   the Xbox Series X pad on USB:
   `connected id=1 name="Xbox Series X Controller" type=xboxone style=Xbox guid=0300f5a35e040000120b000009050000 rumble=yes path=/dev/input/event4 wake=fd (at start)`.
2. **Every button.** `./build/mightyos-inputtest --seconds 60`, then press, one
   at a time: A, B, X, Y, Menu, View, LB, RB, LT, RT, each D-pad direction,
   the left stick in each direction, Guide. Expected, in order:
   `action Accept`, `Back`, `Favorite`, `Search`, `Options`, `QuickPanel`,
   `PrevCategory`, `NextCategory`, `PageUp`, `PageDown`, `Navigate up/down/
   left/right` (D-pad and stick alike), `Guide`, each with
   `source=Gamepad repeat=no`, plus one `style Xbox` line before the first.
   The right stick, stick clicks and the Share button print nothing.
3. **Repeat.** Hold D-pad right for about 2 s: one `repeat=no`, then after
   ~0.35 s a stream of `repeat=yes` that speeds up. Release: it stops at once.
   A trigger pulled and held prints exactly one `PageDown`. Nudging the
   stick slightly prints nothing. Holding the stick on a diagonal prints one
   direction and keeps repeating it, never alternating.
4. **Hotplug.** During a 30 s run, unplug the pad (`disconnected ...`) and
   plug it back in (`connected ...`, without `(at start)`).
5. **Rumble.** `./build/mightyos-inputtest --seconds 20 --rumble`, then
   press A: `rumble requested` and a short, light buzz.
6. **Guide while a game runs.** `./build/mightyos-inputtest --seconds 20 --running`:
   no button prints anything (A and the D-pad included); hold Guide for 1 s:
   `guideHeld source=Gamepad` once per hold.
7. **Visual test** in the session (see "Session"). On tty1, logged in as
   the test user: `~/mightyos-dev/build/session/mightyos-session`. Check that the
   prompt bar shows Xbox glyphs (green A, red B, ...) after the first pad
   press and key caps after a key press; that LB/RB switch categories,
   Guide goes to Home > Favorites from a game grid, and the end of a row
   bumps with a short rumble (once per hold, not on every repeat);
   Settings > Controllers lists the pad, turns vibration off and on and
   switches the TV remote level; launching a game and holding Guide there
   (or Ctrl+Alt+Home) opens the Guide menu over it (needs layer-shell-qt).
   Ctrl+Alt+Backspace asks "Leave MightyOS?" and leaves the session.
8. **Remote without hardware.** `printf 'up\n+right\n' | ./build/mightyos-inputtest --fake-cec --seconds 3`:
   `Navigate up`, `Navigate right repeat=no`, then `repeat=yes` lines
   (source=Remote). `printf 'red\nyellow\n' | ./build/mightyos-inputtest --fake-cec --seconds 2`
   prints both as `(dropped)` (navigation only); with `--tv-remote full`
   they come through as `Options` and `Search`.

## Layout

```
CMakeLists.txt
data/mock/library.json  mock systems + games (tests, --mock-library)
data/systems.d/      default system definitions (built in and installed)
docs/ui-mockups.md   screen mockups and focus maps
session/             mightyos-session.in (the one shell script), mightyos.desktop.in,
                     labwc/ (rc.xml.in, menu.xml, environment), system/ (optional hardening,
                     autologin examples)
src/                 C++, static library mightyos_core = QML module MightyOS.Core
  main.cpp           CLI, engine, image provider, attaches InputRouter to the window,
                     starts (and joins on quit) the gamepad and CEC threads
  launcher.{h,cpp}   runs one app at a time (QProcess, clean env, own session,
                     systemd user scope; terminate by scope or process group)
  session.{h,cpp}    Session singleton: session/safe mode, dormant, Guide overlay state
  sessionstate.{h,cpp} crash counter, recorded units, group check (session.json);
                     SessionLock: the flock()ed pid file and `--signal`
  signalwatcher.{h,cpp} SIGTERM/INT/HUP/USR1/USR2 into the event loop (self-pipe)
  processhardening.{h,cpp} no core dumps / not dumpable; children get the core limit back
  guideoverlayhost.{h,cpp} the layer-shell Guide overlay window (LayerShellQt, optional)
  inputrouter.{h,cpp} key/mouse -> actions, repeat policy, mute/blocked,
                     thread-safe post*(), lastSource, gamepadStyle, guideHeld,
                     rumble policy, cursor auto-hide
  padinput.{h,cpp}   pure pad logic: mapping, stick/trigger hysteresis, repeat,
                     Guide hold, token bucket; InputGate (posting, coalescing,
                     app-running rule)
  padhub.{h,cpp}     SDL-free core of the pad thread: pads, wake-up fds, poll(),
                     latching, rumble target
  gamepadservice.{h,cpp} SDL3 thread: hotplug, mappings, rumble
  textsanitize.{h,cpp} cleaning of device names, terminal escaping
  cecinput.{h,cpp}   pure CEC logic: UI command mapping, parsing, replies, repeat
  cecservice.{h,cpp} CEC worker + backends (kernel /dev/cecN, fake, libcec stub)
  settings.{h,cpp}   global settings.json (rumble, tvRemote)
  feedback.{h,cpp}   SoundFx Bump/Error -> InputRouter::rumble()
  navstate.{h,cpp}   focus-path persistence (navstate.json), per profile
  soundfx.{h,cpp}    UI sounds API (no-op stub until QtMultimedia is available)
  tilelistmodel.{h,cpp} the model contract every row/grid uses
  library.{h,cpp}    systems + games: registry, scan worker, cache, Steam, kids filter,
                     emulator choice, launching (or the mock JSON)
  systemregistry.{h,cpp} systems.d parsing, validation, layers
  romscanner.{h,cpp} ROM walk, title cleaning, multi-disc, launch-time path check
  librarycache.{h,cpp} library.sqlite (QtSql)
  keyvalues.{h,cpp}  Valve KeyValues (VDF/ACF) parser with limits
  steamlibrary.{h,cpp} Steam roots, libraries, manifests, art, running checks
  steamtracker.{h,cpp} Steam game exit detection (LaunchTracker)
  emulatorlaunch.{h,cpp} RetroArch / Flatpak / native / Steam argv planning
  apikeystore.{h,cpp} SteamGridDB key: libsecret or a 0600 file
  steamgriddb.{h,cpp} HTTPS fetcher (allowlist, caps), API helpers, image checks
  artworkscraper.{h,cpp} Scraper singleton: queue, rate limit, backoff, disk LRU
  desktopentry.{h,cpp} Desktop Entry parser, Exec= -> argv without a shell
  appscanner.{h,cpp} .desktop + manifest scanning (pure, runs on a worker)
  appregistry.{h,cpp} installed apps: async scan, watcher, models, argv
  iconlocator.{h,cpp} thread-safe icon theme lookup
  artworkprovider.{h,cpp} image://art/ async provider, disk + memory cache
  profileservice.{h,cpp} profiles.json, active profile, per-profile dirs
  favorites.{h,cpp}  per-profile favorites (+ CatalogResolver)
  history.{h,cpp}    per-profile launch history, Recent model
  itemresolver.h, ids.{h,cpp}, jsonfile.{h,cpp}  key resolution, id rules, atomic JSON
qml/                 static library mightyos_ui = QML module MightyOS.UI
  Main.qml           window, safe area, top bar, startup rule, routing to modals / picker / NavStack
  NavStack.qml       screen stack, action routing, focus path save/restore
  NavScreen.qml      base type for screens (handleAction, save/restoreState, favorites)
  screens/           HomeScreen, SystemListScreen, GameGridScreen, CategoryScreen, AppListScreen,
                     ArtworkScreen
  ProfilePicker, ProfileEditor, ChoiceRow, ProfileChip   profiles UI
  Tile, TileRow, TileGrid, CategoryBar, FocusRing, PointerArea, WheelStepper
  PromptBar, ButtonGlyph (+ Glyphs.js: per-device glyphs), Breadcrumb, InfoHeader, ContextMenu, Dialog, Toast, Clock,
  LaunchOverlay, Icon (+ Icons.js shared icon table), Theme (singleton)
  GuideMenu, GuideOverlay  the Guide menu over a running app (in-window / layer-shell window)
  Catalog.js         fixed Home categories and Store/Settings sections
testapp/             mightyos-testapp, stand-in launch target (--quit-after, --exit-code, --crash)
tools/inputtest.cpp  mightyos-inputtest: prints pads, remote and actions (no GUI)
tests/               tst_launcher, tst_inputrouter, tst_navstate, tst_tilelistmodel,
                     tst_desktopentry, tst_appregistry, tst_profiles, tst_favorites,
                     tst_artwork, tst_padinput, tst_padhub, tst_gamepadservice, tst_cec,
                     tst_settings, tst_keyvalues, tst_systems, tst_romscanner, tst_steam,
                     tst_library, tst_scraper, tst_webapps, tst_moonlight, tst_media,
                     tst_session, tst_qml + qml/; fakesystemd.cpp (fake systemd-run, systemctl,
                     flatpak, labwc, mightyos)
```

### Input pipeline

```
keyboard / mouse buttons --(window event filter)-----------------------------+
SDL3 thread: PadState (repeat, hysteresis) -> InputGate --postAction, queued-+--> InputRouter --triggered(action, arg)-->
CEC backend thread -> CEC worker: RemoteState -> InputGate --postAction------+        |  guideHeld (while running)
    Main.route(): Guide -> close modals, Home                                         |  rumbleRequested -> SDL thread
                  Dialog > ContextMenu > ProfilePicker > NavStack -> top screen.handleAction()
                                                   -> NavStack defaults (Back pops, LB/RB, Options, Favorite)
```

`InputRouter`'s `post*()` members are the only ones that may be called from
another thread. Delivery always happens on the GUI thread, and the
blocked, mute and repeat checks run at delivery time. Each input thread
keeps at most one Navigate repeat per direction in the queue (a pending
counter the router decrements on delivery), so a busy GUI thread never
scrolls past where the user let go. The threads are joined on
`aboutToQuit`, before the engine (and InputRouter) is destroyed.

### Data flow

```
.desktop files, manifests --AppScanner (worker thread)--> AppRegistry --+
systems.d + ROM dirs + Steam --scan worker (+ library.sqlite)--> Library -+--> TileListModel --> TileRow / TileGrid
favorites.json / history.json --(keys)--> Favorites / History ---------+        (key, title, subtitle, icon,
                                  (resolved via AppRegistry + Library)            artwork, kind, available, ...)
icon names / paths, Steam art, SteamGridDB files --> image://art/<id> --> ArtworkProvider (worker, disk + memory cache) --> Tile
```

Rows never loop over a model in JavaScript: `indexOfKey()` is hash-backed,
`get(i)` returns one row, and `firstIndexForLetter()` / `letterStep()` run
in C++. `setItems()` is a diff, so a re-scan that adds or removes games
inserts and removes rows instead of resetting; a real reset (reordering) keeps
the focus on the same key, and a moved favorite keeps its focus.
