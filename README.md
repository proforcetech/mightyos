# MightyOS

A console-style launcher for Ubuntu with large tiles, built for a TV
("10-foot") setup and for keyboard, mouse or gamepad. See `PLAN.md` for the
full plan. This is milestone **M0, the walking skeleton**.

## Build

Requirements: Qt >= 6.8 (Core, Gui, Qml, Quick, QtQuick.Shapes; Test for
the unit tests), CMake >= 3.21, Ninja and a C++20 compiler.

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
session.

## Test

```sh
ctest --test-dir build --output-on-failure    # Launcher unit tests
cmake --build build --target all_qmllint      # QML lint
```

## Controls

| Action | Keyboard | Mouse | Gamepad (M3) |
|---|---|---|---|
| Move focus | Left / Right | Move the pointer over a tile | D-pad / stick |
| Open | Enter / Space | Left click | A |
| Back | Esc / Backspace | — | B |

- Rows don't wrap around. At either end the focus "bumps".
- Hovering moves focus only after the pointer actually moves, so tiles that
  scroll under a resting cursor don't steal focus.
- **Media** and **Settings** launch a test app. The other tiles show a
  "coming soon" toast.
- While the app runs, MightyOS ignores input and shows an overlay. Close the
  app to come back. Focus returns to the same tile, and input stays muted
  for 300 ms.

The test app is the bundled `build/mightyos-testapp`. Close it with Esc,
Enter or a click.

## Layout

```
CMakeLists.txt
src/        C++: main.cpp (CLI, engine), launcher.{h,cpp} (QML singleton)
qml/        QML module MightyOS.UI (Main, Home, Tile, TileRow, FocusRing,
            Theme, InputMap, PromptBar, Clock, Toast, LaunchOverlay, Icon)
testapp/    mightyos-testapp, a stand-in launch target for development
tests/      tst_launcher (QtTest)
```

All input goes through the `InputMap` singleton
(`qml/InputMap.qml`). It turns keys into the named actions `navigate(dir)`,
`accept` and `back`. Screens only ever see actions, so the SDL3 gamepad
(M3) can feed the same `InputMap.dispatch()`.
