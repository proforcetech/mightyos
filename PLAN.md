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
| D11 | Artwork comes from **SteamGridDB** | Each user supplies their own API key, entered in Settings via the on-screen keyboard. The key is stored in libsecret, or as a `0600` file if that isn't available, and is never logged. Steam games use Steam's local library art first |
| D12 | **Steam is a game "system", like an emulator.** Games → [Steam] → one tile per installed game | Steam games come from parsing `appmanifest_*.acf` files. They use the same SystemList → GameGrid screens as emulators. Other PC launchers (Heroic, Lutris) can join later the same way |
| D13 | **Multiple user profiles**, plus a **kids/restricted mode** | Each profile has its own favorites, history, saves folder, theme and per-profile settings. A kids profile only shows allowed systems, games and apps, filtered by content rating, and can't use the store or settings. Leaving a kids profile requires the PIN. See §4 for its limits |
| D14 | **HDMI-CEC TV remote** support | The TV remote's D-pad, OK, Back and Home buttons go into InputRouter as the same actions as other devices. Uses libcec or the kernel CEC framework (`/dev/cec*`). **Most PC GPUs don't expose CEC on their HDMI port**, so this usually needs a USB-CEC adapter such as a Pulse-Eight, or a mini PC with built-in CEC |
| D15 | License: **MIT** | `LICENSE` at the repo root. Third-party pieces keep their own licenses (Qt is LGPL and dynamically linked; SDL3 is zlib) |

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
- **Profiles:** `ProfileService` decides which profile is active. Each profile's data lives under `.../mightyos/profiles/<profile-id>/` inside the config, data and state directories below. Global settings and the library cache are shared between profiles.
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
- **Parental lock and kids mode are UI features, not a security boundary**, and will be labelled that way. All profiles run as the same Unix user, so a determined child who reaches a terminal or browser could get around them. The mitigations:
  - kids profiles have no store, settings, browser or "Close to desktop"
  - leaving a kids profile requires the PIN, checked by the helper
  - the session starts in the last profile, or a chosen default kids profile.
  A stronger later option is a separate Unix user per kids profile; that would be a session switch.
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
| **M1** | Navigation framework | Tile, TileRow, TileGrid, NavStack with focus memory, PromptBar, mouse support, Theme with resolution-independent scaling and safe area, SoundFx. **Carried over from the M0 review:** (a) move InputMap into a C++ `InputRouter` singleton that owns the key table, repeat policy and blocked/mute state, with a thread-safe queued entry point for SDL in M3; (b) send actions to the top NavStack screen so unhandled actions bubble up, and make mouse clicks activate their own tile; (c) save focus as a path {screen,row,index}; (d) a static `mightyos_core` library shared by the app and the tests; (e) Qt Quick Test coverage for input rules and Launcher crash tests; (f) refresh the clock when the window becomes active; (g) a shared JS icon table | gui-expert | code-reviewer |
| **M2** | Data layer & profiles | AppRegistry (`.desktop` files and manifests), **ProfileService + profile picker screen** (create, rename, avatar, choose), per-profile favorites (X to toggle, reorder), async artwork and cache, navstate persistence | general-purpose + gui-expert | code-reviewer |
| **M3** | Gamepad & TV remote | SDL3 thread, key repeat, hotplug, button glyphs (Xbox/PS/Nintendo), rumble. **HDMI-CEC** via libcec or `/dev/cec*`, feeding the same actions. **Needs real hardware** (gamepad, and a CEC adapter or CEC-capable PC) | gui-expert | code-reviewer + security-expert (device access) |
| **M4** | Emulators & Steam library | System definitions, RomScanner + SQLite, game grid, RetroArch and standalone emulator launching, per-game core override. **Steam as a system** (`.acf` parsing, launching via `steam://rungameid`, detecting game exit). **SteamGridDB scraper** (API key, rate limits, cache) | general-purpose + gui-expert | code-reviewer + security-expert (launch hygiene, API key) |
| **M5** | Media & streaming | Kodi, Jellyfin, Plex tiles; web-app tiles (kiosk browser); Moonlight host/app tiles | gui-expert | code-reviewer |
| **M6** | Session | cage session files, scope-based launching, dormant mode while an app runs, Guide overlay, crash recovery, VT/SysRq hardening | general-purpose | security-expert |
| **M7** | App Store | AppStream browse and search with on-screen keyboard, detail page, permission review, libflatpak install/update/remove with progress | gui-expert + general-purpose | security-expert + code-reviewer |
| **M8** | Settings, privileges & kids mode | Display, Audio, Network, Controllers/Bluetooth, CEC settings; polkit agent; `mightyos-helper`; PinPad; first-boot setup wizard; **kids/restricted profiles** (allow-lists, rating filter, PIN required to leave) | gui-expert + general-purpose | **security-expert (required)** |
| **M9** | Polish & performance | Target 60 fps, under 150 MB RSS, under 1% idle CPU; software renderer fallback; gamescope spike | gui-expert | code-reviewer |

M0 to M2 can be built entirely on the WSL dev box. M3 and later need a real x86 test machine for gamepad, session and Flatpak testing.

## 6. Risks

1. **No global hotkey under cage while a game has focus.** A keyboard-only user can't get out of a frozen app. Options: an evdev panic chord (needs `input` group access, which is a security trade-off) or a wlroots kiosk config (labwc/sway). Decide in M6.
2. **gamescope without Steam** is fragile, so it's optional (D5).
3. **Steam:** detecting when a game exits is heuristic, and Steam Input can create duplicate controllers.
4. **WSLg limits:** no gamepad passthrough by default, and systemd, polkit and Flatpak behave differently there.
5. **Ubuntu 26.04 packages:** need to confirm SDL3, AppStreamQt6, PolkitQt6 and cage are available.
6. **DRM streaming:** Netflix and similar services need a Widevine-capable browser.
7. **CEC hardware:** most PC GPUs have no CEC on HDMI, so it needs an adapter (D14).
8. **SteamGridDB terms and rate limits:** each user has to supply their own API key, so there's no shared key in the binary.

## 7. Open questions

- ~~Q1–Q4~~ answered → D11–D14.
- Q5: Is a real x86 test machine available, and roughly when?
- Q6: Panic-chord approach (Risk 1).

## 8. Status

- [x] Vision captured (`guide.txt`)
- [x] Core decisions D1–D10
- [x] GUI and security design reviews
- [x] Dev toolchain installed on the WSL box (Qt 6.10.2, CMake 4.2, g++ 15, SDL3 3.4.2)
- [x] M0 walking skeleton, reviewed. Fixed: test app is bundled-only; launch-state guard; child process hygiene (own session, PDEATHSIG, clean fds/signals, group terminate); edge-bump repeat; "crashed" toast; TZ passthrough
- [ ] M1 navigation framework
