# MightyOS

A console-style launcher for Ubuntu with large tiles, built for a TV
("10-foot") setup and for keyboard, mouse or gamepad. See `PLAN.md` for the
full plan. This is milestone **M1, the navigation framework**. All content
is mock data until M2/M4.

## Build

Requirements: Qt >= 6.8 (Core, Gui, Qml, Quick, Test, QuickTest), CMake >= 3.21,
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
session. MightyOS reopens on the screen and tile you left: the focus path is
saved to `$XDG_STATE_HOME/mightyos/navstate.json` (normally
`~/.local/state/mightyos/`). Delete that file to start at Home.

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
| `tst_qml` | Qt Quick Test: `tst_tilerow.qml`, `tst_tilegrid.qml` (edges, bump, hover incl. a scrolled row under a resting cursor, click, wheel, paging, letter jump, icon rasterising), `tst_navstack.qml` (routing, bubbling, inert screens during transitions, system stepping, stable prompts, restoring stale paths, per-category positions), `tst_main.qml` (whole window: blocked while running, focus restore, 300 ms mute, crash toast, modal focus trap, Dialog defaults) |

Tests run with `XDG_STATE_HOME=build/test-state`, which a ctest fixture
empties before every run, so they never touch your real state.

## Controls

| Action | Keyboard | Mouse | Gamepad (M3) |
|---|---|---|---|
| Move focus | Arrow keys | Move the pointer over a tile | D-pad / stick |
| Select / open / play | Enter, Space | Left click (acts on the clicked tile) | A |
| Back | Esc, Backspace | Back button, a breadcrumb, the "Back" prompt | B |
| Options (context menu) | M, Menu | Right click | Start |
| Favorite | F | Context menu | X |
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
- Games and media apps launch the bundled test app. While it runs, MightyOS
  ignores input and shows an overlay. When it closes, focus is back on the
  same tile and input stays muted for 300 ms.

Screen mockups and focus maps: [docs/ui-mockups.md](docs/ui-mockups.md).

## Layout

```
CMakeLists.txt
docs/ui-mockups.md   screen mockups and focus maps
src/                 C++, static library mightyos_core = QML module MightyOS.Core
  main.cpp           CLI, engine, attaches InputRouter to the window
  launcher.{h,cpp}   runs one app at a time (QProcess, clean env, own session)
  inputrouter.{h,cpp} key/mouse -> actions, repeat policy, mute/blocked,
                     thread-safe postAction(), lastSource, cursor auto-hide
  navstate.{h,cpp}   focus-path persistence (navstate.json), profile-aware
  soundfx.{h,cpp}    UI sounds API (no-op stub until QtMultimedia is available)
qml/                 static library mightyos_ui = QML module MightyOS.UI
  Main.qml           window, safe area, top bar, routing to modals / NavStack
  NavStack.qml       screen stack, action routing, focus path save/restore
  NavScreen.qml      base type for screens (handleAction, save/restoreState)
  screens/           HomeScreen, SystemListScreen, GameGridScreen, CategoryScreen
  Tile, TileRow, TileGrid, CategoryBar, FocusRing, PointerArea, WheelStepper
  PromptBar, Breadcrumb, InfoHeader, ContextMenu, Dialog, Toast, Clock,
  LaunchOverlay, Icon (+ Icons.js shared icon table), Theme (singleton)
  mock/MockData.js   mock categories/systems/games/apps (shapes match M2/M4 models)
testapp/             mightyos-testapp, stand-in launch target (--quit-after, --exit-code, --crash)
tests/               tst_launcher, tst_inputrouter, tst_navstate, tst_qml + qml/
```

### Input pipeline

```
keyboard / mouse buttons --(window event filter)--+
SDL3 gamepad thread (M3) --postAction(), queued---+--> InputRouter --triggered(action, arg)-->
CEC remote thread (M3)   --postAction(), queued---+
    Main.route(): Dialog > ContextMenu > NavStack -> top screen.handleAction()
                                                   -> NavStack defaults (Back pops, LB/RB, Options, Favorite)
```

`InputRouter::postAction(action, arg, isRepeat, source)` is the only member
that may be called from another thread. Delivery always happens on the GUI
thread, and the blocked, mute and repeat checks run at delivery time.

### Profiles (M2)

Profiles don't exist yet, but the hooks are in place. `NavState.load/save`
take a profile id (`profiles/<id>/navstate.json`), and `NavStack.profileId`
selects it. The default, empty id uses the top-level file.
