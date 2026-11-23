# MightyOS

A console-style launcher for Ubuntu with large tiles, built for a TV
("10-foot") setup and for keyboard, mouse or gamepad. See `PLAN.md` for the
full plan. This is milestone **M2, data layer & profiles**: real installed
apps (`.desktop` files and manifests), profiles with a picker, per-profile
favorites and history, and offline artwork. Systems and games are still a
mock library until M4.

## Build

Requirements: Qt >= 6.8 (Core, Gui, Qml, Quick, Concurrent, Test, QuickTest), CMake >= 3.21,
Ninja and a C++20 compiler. At run time the Qt SVG image plugin is needed
for the icons (Ubuntu: `qt6-svg-plugins`).

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Ninja compile jobs are capped by RAM (about one job per GB, see
`MIGHTYOS_COMPILE_JOBS`), because Qt's AOT-compiled QML is memory-hungry.
Override the cap with `-DMIGHTYOS_COMPILE_JOBS=N`. Turn the tests off with
`-DMIGHTYOS_BUILD_TESTS=OFF`.

## Run

```sh
./build/mightyos               # 1280x720 window (current default)
./build/mightyos --windowed    # same, explicit
./build/mightyos --fullscreen
QT_QPA_PLATFORM=offscreen ./build/mightyos   # headless smoke test
```

The default switches to fullscreen in M6, when MightyOS becomes its own
session. With one profile MightyOS opens straight on Home; with two or more
it asks "Who's playing?" first, with the last used profile preselected.
Each profile reopens on the screen and tile it left.

### Files

| Path | What |
|---|---|
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
`.unsupported`, so the next write never destroys it. The M1 `navstate.json` becomes the first profile's
on the first M2 start.

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
`.history`, `.artwork` and `.library` print warnings only by default. To see
what the registry found and why entries were skipped:

```sh
QT_LOGGING_RULES="mightyos.apps.debug=true;mightyos.apps.info=true" ./build/mightyos
```

Logs contain ids and counts, never command lines.

## Test

```sh
ctest --test-dir build --output-on-failure    # all tests (offscreen)
cmake --build build --target all_qmllint      # QML lint
```

| Test | Covers |
|---|---|
| `tst_launcher` | environment allowlist, launch/exit/exit code, crash (`--crash` -> -1), launch guards |
| `tst_inputrouter` | key table, repeat policy, mute window, blocked, cross-thread `postAction`, mouse back button, text-field passthrough, cursor auto-hide |
| `tst_navstate` | save/load round trip, corrupt and stale files, path truncation, profile paths, debounce/flush |
| `tst_tilelistmodel` | model contract (roles, `indexOfKey`/`firstIndexForLetter` after reset, insert, remove, move; `get`; in-place updates), mock Library ids and structure |
| `tst_desktopentry` | `Exec=` parsing per the spec (quotes, escapes, `%f %F %u %U %i %c %k %%`, deprecated codes, rejected shell syntax and malformed values), file parsing, locales, lists |
| `tst_appregistry` | desktop-file-id precedence, Hidden/NoDisplay/TryExec/OnlyShowIn/NotShowIn/Terminal, subdirectory ids, Flatpak ids, icon validation, oversized/broken files, manifests and their validation, async scan, watcher re-scan (incl. a dir created later), no argv in logs |
| `tst_profiles` | first run, create/rename/appearance/kind, delete (with its data) and last-profile protection, startup rule, last-used ordering, invalid ids in the file, corrupt file, failed atomic write, M1 navstate migration |
| `tst_favorites` | toggle, saved order, move, unavailable targets (and their recovery), per-profile isolation, file validation; history counts, order, per profile, cap |
| `tst_artwork` | icon theme lookup (inheritance, best size, SVG preference, pixmaps), decode at the requested size, memory and disk cache hits, re-decode on change, untrusted specs, async responses |
| `tst_qml` | Qt Quick Test: `tst_tilerow.qml`, `tst_tilegrid.qml` (edges, bump, hover incl. a scrolled row under a resting cursor, click, wheel, paging, letter jump, icon rasterising), `tst_navstack.qml` (routing, bubbling, inert screens during transitions, system stepping, stable prompts, restoring stale paths, per-category positions), `tst_main.qml` (whole window: blocked while running, focus restore, 300 ms mute, crash toast, modal focus trap, Dialog defaults), `tst_profiles.qml` (picker only with 2+ profiles, last used preselected, switching resets and restores the stack, X toggles favorites that appear in order, reordering, unavailable favorites, a launch fills Recent, create/edit/delete in the picker) |

Tests run with `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_DATA_HOME`,
`XDG_CACHE_HOME` and `XDG_DATA_DIRS` under `build/test-xdg/`, which a ctest
fixture empties before every run, so they never touch your real files and
never see the system's `.desktop` files.

## Controls

| Action | Keyboard | Mouse | Gamepad (M3) |
|---|---|---|---|
| Move focus | Arrow keys | Move the pointer over a tile | D-pad / stick |
| Select / open / play | Enter, Space | Left click (acts on the clicked tile) | A |
| Back | Esc, Backspace | Back button, a breadcrumb, the "Back" prompt | B |
| Options (context menu) | M, Menu | Right click | Start |
| Favorite (toggle; games, apps, systems) | F | Context menu | X |
| Search (M7) | `/`, Ctrl+F | | Y |
| Quick panel (M8) | Q | Click the clock | Select |
| Previous / next category (in a game grid: previous / next system) | PgUp / PgDn | Click a category | LB / RB |
| Page up / down in grids | Home / End | Wheel | LT / RT |

- Every prompt in the bottom bar can also be clicked.
- The wheel steps the focus through the row or grid under the pointer, or
  the focused one over empty space.
- Rows and grids don't wrap around. At an edge the focus "bumps".
- Hover moves focus only after the pointer really moves. The cursor hides
  after 3 s without mouse movement and on any key press. The first move that
  brings it back doesn't steal focus.
- Only Navigate auto-repeats. A held Enter can't launch twice.
- Dialogs and the context menu trap focus. B closes them, and dialogs start
  on (and B picks) the non-destructive button.
- LB/RB returns to Home and switches category, except in a system's game
  grid, where it steps to the previous/next system (SNES -> N64) in place.
- Apps launch with the command from their `.desktop` file or manifest.
  Games (mock until M4) launch the bundled test app. While something runs,
  MightyOS ignores input and shows an overlay. When it closes, focus is back
  on the same tile and input stays muted for 300 ms. Successful launches
  appear in the "Recent" row on Home > Favorites.
- Favorites: X / F toggles the focused game, app or system (a star shows on
  its tile). Home > Favorites and the Favorites screen show them in your
  order. Options > Move left / Move right reorders them. A favorite whose
  app or game is gone stays as a greyed "Unavailable" tile; Accept on it
  offers to remove it.
- Profiles: Home > Options > Switch profile, Settings > Profiles, or a click
  on the profile in the top bar. The picker creates profiles (pick a name
  from suggestions, an avatar, a colour and Standard/Kids), renames or
  re-styles them, and deletes them (after a confirmation; the last one
  can't be deleted). Kids profiles get a badge; their restrictions arrive
  in M8. Settings > All apps lists every app the registry found.

Screen mockups and focus maps: [docs/ui-mockups.md](docs/ui-mockups.md).

## Layout

```
CMakeLists.txt
data/mock/library.json  mock systems + games (same ids as M1), until M4
docs/ui-mockups.md   screen mockups and focus maps
src/                 C++, static library mightyos_core = QML module MightyOS.Core
  main.cpp           CLI, engine, image provider, attaches InputRouter to the window
  launcher.{h,cpp}   runs one app at a time (QProcess, clean env, own session)
  inputrouter.{h,cpp} key/mouse -> actions, repeat policy, mute/blocked,
                     thread-safe postAction(), lastSource, cursor auto-hide
  navstate.{h,cpp}   focus-path persistence (navstate.json), per profile
  soundfx.{h,cpp}    UI sounds API (no-op stub until QtMultimedia is available)
  tilelistmodel.{h,cpp} the model contract every row/grid uses
  library.{h,cpp}    systems + games (mock JSON until M4)
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
  screens/           HomeScreen, SystemListScreen, GameGridScreen, CategoryScreen, AppListScreen
  ProfilePicker, ProfileEditor, ChoiceRow, ProfileChip   profiles UI
  Tile, TileRow, TileGrid, CategoryBar, FocusRing, PointerArea, WheelStepper
  PromptBar, Breadcrumb, InfoHeader, ContextMenu, Dialog, Toast, Clock,
  LaunchOverlay, Icon (+ Icons.js shared icon table), Theme (singleton)
  Catalog.js         fixed Home categories and Store/Settings sections
testapp/             mightyos-testapp, stand-in launch target (--quit-after, --exit-code, --crash)
tests/               tst_launcher, tst_inputrouter, tst_navstate, tst_tilelistmodel,
                     tst_desktopentry, tst_appregistry, tst_profiles, tst_favorites,
                     tst_artwork, tst_qml + qml/
```

### Input pipeline

```
keyboard / mouse buttons --(window event filter)--+
SDL3 gamepad thread (M3) --postAction(), queued---+--> InputRouter --triggered(action, arg)-->
CEC remote thread (M3)   --postAction(), queued---+
    Main.route(): Dialog > ContextMenu > ProfilePicker > NavStack -> top screen.handleAction()
                                                   -> NavStack defaults (Back pops, LB/RB, Options, Favorite)
```

`InputRouter::postAction(action, arg, isRepeat, source)` is the only member
that may be called from another thread. Delivery always happens on the GUI
thread, and the blocked, mute and repeat checks run at delivery time.

### Data flow

```
.desktop files, manifests --AppScanner (worker thread)--> AppRegistry --+
data/mock/library.json ---------------------------------> Library -----+--> TileListModel --> TileRow / TileGrid
favorites.json / history.json --(keys)--> Favorites / History ---------+        (key, title, subtitle, icon,
                                  (resolved via AppRegistry + Library)            artwork, kind, available, ...)
icon names / paths --> image://art/<id> --> ArtworkProvider (worker, disk + memory cache) --> Tile
```

Rows never loop over a model in JavaScript: `indexOfKey()` is hash-backed,
`get(i)` returns one row, and `firstIndexForLetter()` runs in C++. A model
reset (re-scan, profile switch) keeps the focus on the same key; a moved
favorite keeps its focus.
