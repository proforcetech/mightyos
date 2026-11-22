# MightyOS — Project Plan

A lightweight console-style launcher (like Batocera or SteamOS) for Ubuntu. It uses large, horizontally scrolling tiles and works with a keyboard, keyboard plus mouse, or a gamepad. It covers emulators, games, streaming and local media, and a Flathub-based App Store.

Sources: `guide.txt`, plus design reports from the gui-expert and security-expert agents (2026-11-21).

---

## 1. Decisions log

| # | Decision | Notes |
|---|---|---|
| D1 | Runs as its **own Wayland login session**. You can pick it at the login screen or boot straight into it. | During development it runs in a window under WSLg |
| D2 | Target hardware: **x86 PCs** | Mini PCs, desktops, handhelds. Performance budget: Intel N100-class iGPU |
| D3 | **Qt 6 / QML, with the backend in C++17/20** | Uses less memory than PySide6 (no Python runtime) and links directly to SDL3, libflatpak, AppStreamQt and PolkitQt6. Most of the work is in QML anyway |
| D4 | App Store source: **Flatpak/Flathub**, installed **per user (`--user`)** | Installing needs no root. Flathub is the only remote, with GPG verification |
| D5 | Compositor: **cage by default**. gamescope is an option, or a per-game wrapper | cage hands focus back to MightyOS when an app closes. gamescope without Steam relies on undocumented atoms, so it gets a spike in M9 |
| D6 | Gamepad input through **SDL3** (or SDL2 via sdl2-compat) | QtGamepad was removed in Qt 6. All input is converted into one set of Qt key events |
| D7 | Admin auth: **PIN for everyday locked actions, the real password for system-level ones** | The security review says a PIN must never act as the root password. See §4 |
| D8 | **No `sudo` anywhere.** Privileged actions go through polkit and a small D-Bus helper | The MightyOS session user is not in the `sudo` group. A separate admin account exists |
| D9 | Emulation targets: Nintendo retro, Sega/arcade, Sony (PS1/PS2/PSP), GameCube/Wii/DS/3DS | Via RetroArch cores plus standalone emulators (Dolphin, PCSX2, PPSSPP, DuckStation, melonDS, Azahar), all as Flatpaks |
| D10 | Media: Kodi/local, Jellyfin/Plex, streaming web apps, Moonlight game streaming | Web apps run in a kiosk-mode browser Flatpak. Netflix and similar services need Widevine, so use Chrome or Chromium |

**Conflict resolved:** the GUI design suggested passing the PIN through polkit/PAM for system-wide installs. The security review rejects that because it would weaken every polkit decision on the system. Instead, installs are `--user` (no prompt needed), and the PIN is checked **only** by our own helper for our own action IDs.

---

## 2. Architecture

```
QML UI (module MightyOS.UI)
  Theme · FocusRing · Tile · TileRow · TileGrid · CategoryBar · PromptBar
  Dialog · ContextMenu · OnScreenKeyboard · PinPad · Toast · NavStack
  Screens: Home, SystemList, GameGrid, Media, Store, StoreDetail, Settings
----------------------------------------------------------------------------
C++ singletons / models exposed to QML
  AppRegistry  FavoritesModel  LibraryModel  StoreService  Launcher
  InputRouter  Settings  PrivilegedClient  ArtworkProvider  SoundFx
----------------------------------------------------------------------------
Workers / system
  RomScanner + SQLite · SDL3 gamepad thread · libflatpak + AppStreamQt
  systemd-run --user scopes · PolkitQt6 agent · NetworkManager/BlueZ/logind D-Bus
----------------------------------------------------------------------------
mightyos-helper (system D-Bus service, hardened systemd unit)
  fixed typed methods only · polkit checks · PIN verify (argon2id) + lockout
```

- QML talks to C++ through `QML_SINGLETON` services and `QAbstractListModel` models. Slow work runs on worker threads, so the UI thread never blocks.
- **Files** (XDG locations, JSON format):
  - `~/.config/mightyos/`: `settings.json`, `favorites.json`, `input.json`, `systems.d/`
  - `~/.local/share/mightyos/`: user app manifests, scraped artwork
  - `~/.local/state/mightyos/`: `history.json`, `navstate.json`
  - `~/.cache/mightyos/`: `library.sqlite`, thumbnails
  - `/usr/share/mightyos/`: default systems and apps
  - ROMs: `~/ROMs/<system>/` by default
- **App IDs:** `flatpak:<appid>`, `desktop:<id>`, `rom:<system>:<sha1(relpath)>`, `steam:<appid>`, `moonlight:<host>/<app>`. A favorite whose target is missing shows as a greyed "Unavailable" tile instead of disappearing.

## 3. Input and navigation

| Action | Gamepad | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad / left stick | Arrows | Hover (after the pointer actually moves) |
| Select | A | Enter / Space | Left click |
| Back | B | Esc / Backspace | Back button / breadcrumb |
| Favorite | X | F | Context menu |
| Search | Y | `/`, Ctrl+F | Search tile |
| Options | Start | Menu / M | Right click |
| Quick panel (volume, Wi-Fi, power) | Select | Q | Clock area |
| Previous / next category | LB / RB | PgUp / PgDn | Click the category |
| Jump A–Z in grids | LT / RT | Home / End, letter keys | Wheel |
| Guide overlay (Resume / Close game) | Guide (hold) | panic chord (TBD) | — |

Focus rules:
- Exactly one element has focus at a time, shown by an animated focus ring.
- Each level remembers its focus position, so going back returns you to where you were. This survives restarts.
- Rows don't wrap around; you get a "bump" at the ends.
- Dialogs keep focus inside them, and their default button is the non-destructive one.
- Input is ignored for 300 ms after a game exits.
- The mouse cursor hides after 3 s.

Long game lists use a **grid with A–Z jump** rather than a single horizontal row. Screen mockups are in the gui-expert report; they'll be copied to `docs/ui-mockups.md` in M1.

## 4. Security model (summary)

| Action | Auth required |
|---|---|
| Browse, launch, update installed apps, power off, mount USB drives, pair Bluetooth | None |
| Install or remove store apps, change Wi-Fi, change parental settings, change timezone | **PIN**, checked by `mightyos-helper` |
| Add remotes, Flatpak sandbox overrides, apt/system updates, set or reset the PIN | **Admin password** via our own polkit agent (on-screen keyboard) |

- PIN storage: argon2id (64 MiB, t=3), in a root-only `0600` file. Lockout persists across restarts, with back-off after 3 failures and a full disable after 10. There is no master PIN and no reset backdoor.
- **Parental lock is a UI feature, not a security boundary**, and it will be labelled that way.
- Launching apps:
  - Use argument lists with `--` and never a shell.
  - Clear the environment before launch (strip `LD_PRELOAD` and similar).
  - Put each app in its own `systemd-run --user --scope`.
  - Give ROM folders read-only Flatpak overrides.
  - Turn emulator network features off by default.
- Session hardening:
  - Disable spare VTs and restrict SysRq.
  - Fall back to safe mode after repeated crashes.
  - Disable core dumps for the GUI and exclude it from Apport.
  - Never log secrets; logs get tests that check for this.

## 5. Milestones

| M | Name | Contents | Owner(s) | Gate |
|---|---|---|---|---|
| **M0** | Walking skeleton | CMake + `qt_add_qml_module`; `--windowed`/`--fullscreen` flags; hard-coded category tiles; keyboard navigation and focus ring; launch a test app with QProcess and return focus on exit | gui-expert | code-reviewer |
| **M1** | Navigation framework | Tile, TileRow, TileGrid, NavStack with focus memory, PromptBar, mouse support, Theme with resolution-independent scaling and safe area, SoundFx | gui-expert | code-reviewer |
| **M2** | Data layer | AppRegistry (`.desktop` files and manifests), favorites (X to toggle, reorder), async artwork and cache, navstate persistence | general-purpose + gui-expert | code-reviewer |
| **M3** | Gamepad | SDL3 thread, key repeat, hotplug, button glyphs (Xbox/PS/Nintendo), rumble. **Needs real hardware** (or usbipd-win on WSL) | gui-expert | code-reviewer |
| **M4** | Emulators | System definitions, RomScanner + SQLite, game grid, RetroArch and standalone emulator launching, per-game core override | general-purpose + gui-expert | code-reviewer + security-expert (launch hygiene) |
| **M5** | Media & streaming | Kodi, Jellyfin, Plex tiles; web-app tiles (kiosk browser); Moonlight host/app tiles | gui-expert | code-reviewer |
| **M6** | Session | cage session files, scope-based launching, dormant mode while an app runs, Guide overlay, crash recovery, VT/SysRq hardening | general-purpose | security-expert |
| **M7** | App Store | AppStream browse and search with on-screen keyboard, detail page, permission review, libflatpak install/update/remove with progress | gui-expert + general-purpose | security-expert + code-reviewer |
| **M8** | Settings & privileges | Display, Audio, Network, Controllers/Bluetooth; polkit agent; `mightyos-helper`; PinPad; first-boot setup wizard | gui-expert + general-purpose | **security-expert (required)** |
| **M9** | Polish & performance | Target 60 fps, under 150 MB RSS, under 1% idle CPU; software renderer fallback; gamescope spike; Steam library import | gui-expert | code-reviewer |

M0 to M2 can be built entirely on the WSL dev box. M3 and later need a real x86 test machine for gamepad, session and Flatpak testing.

## 6. Risks

1. **No global hotkey under cage while a game has focus.** A keyboard-only user can't get out of a frozen app. Options: an evdev panic chord (needs `input` group access, which is a security trade-off) or a wlroots kiosk config (labwc/sway). Decide in M6.
2. **gamescope without Steam** is fragile, so it's optional (D5).
3. **Steam:** detecting when a game exits is heuristic, and Steam Input can create duplicate controllers.
4. **WSLg limits:** no gamepad passthrough by default, and systemd, polkit and Flatpak behave differently there.
5. **Ubuntu 26.04 packages:** need to confirm SDL3, AppStreamQt6, PolkitQt6 and cage are available.
6. **DRM streaming:** Netflix and similar services need a Widevine-capable browser.

## 7. Open questions

- Q1: Artwork scraping source (ScreenScraper, TheGamesDB, SteamGridDB)? Some need API keys.
- Q2: Is Steam a full library source (games show up in Games) or just a tile that opens Steam's own UI? Proposal: just a tile at first, library import in M9.
- Q3: One user, or multiple profiles (for example kids' profiles with their own parental lock)?
- Q4: HDMI-CEC TV-remote support: in scope?
- Q5: Is a real x86 test machine available, and roughly when?
- Q6: Panic-chord approach (Risk 1).

## 8. Status

- [x] Vision captured (`guide.txt`)
- [x] Core decisions D1–D10
- [x] GUI and security design reviews
- [ ] Dev toolchain installed on the WSL box
- [ ] M0 walking skeleton
