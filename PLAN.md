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
| D16 | **CEC access via a seat-local udev uaccess rule**, never `video` group membership | Rule file `/etc/udev/rules.d/70-mightyos-cec.rules`: `SUBSYSTEM=="cec", KERNEL=="cec[0-9]*", TAG+="uaccess"`. The kiosk user must not be in `video` or `input` (install-time check). Emulator Flatpaks get `--nodevice=all --device=input`. Ships with M6 session packaging (security-expert, M3 review) |
| D17 | **No automatic CEC Active Source.** Opt-in setting, default off | When enabled: send only after a fresh local gamepad or keyboard press; never at startup, on crash restart, in safe mode, or from Remote input; at most once every 60 s; send `<Inactive Source>` on exit |
| D18 | **Input sources have trust levels.** The TV remote (CEC) is lower trust | Secure screens (PIN pad, admin on-screen keyboard, Bluetooth pairing confirm, leaving kids mode, store install confirm, Close game) ignore `Source::Remote` for everything except **Back/Home (cancel)**, so a remote-only user can never confirm, enter a digit or toggle anything there, but also can't be trapped. CEC presses are rate-limited and broadcast frames are dropped. Settings: TV remote off / navigation only / full |
| D19 | **Letters are never actions** (M4). Bare A–Z / 0–9 jump to that letter in sorted grids; Favorite = Insert / Ctrl+D, Options = Tab / Menu / Shift+F10, Quick panel = F12. Gamepad LT/RT (keyboard Home/End, remote CH±) step to the previous/next letter group in game grids | F/M/Q clashed with type-to-jump. Non-letter action keys keep every key's meaning the same on every screen, and leave letters free for search type-ahead in M7 |
| D20 | **Steam exit detection is a poll** (2 s): running = `registry.vdf` says so (`RunningAppID` / `apps/<id>/Running`) **and** a live process agrees (one with `SteamAppId=<appid>`, or the Steam client named in `steam.pid`); the registry must change after the launch before the game counts as seen (a stale value from a crash doesn't); 2 min start timeout; holding Guide ends any Steam launch, seen or not | `steam steam://rungameid/...` exits at once (or becomes the client). Limits: registry lag, launchers that detach completely, a crashed game while the client stays up is noticed only when Steam updates the registry |
| D21 | **SteamGridDB key storage**: libsecret when built with it and a Secret Service answers, else `~/.config/mightyos/secrets/steamgriddb.key` (dir 0700, file created O_EXCL 0600, fstat-verified, refused if group/other-readable or a symlink). Key I/O runs off the GUI thread | A kiosk session may have no unlocked keyring. Neither the WSL box nor the test PC has `libsecret-1-dev`, so the file backend is the tested one |
| D22 | **Web apps run in a Chromium-family kiosk browser**, defined in `webapps.d` (layers and trust checks as `systems.d`) | Order for DRM services (Netflix, Disney+, Prime Video, Spotify): Chrome → Edge → Brave (Chromium has no Widevine); otherwise Chrome → Chromium → Brave → Edge; a definition's `browser` goes first. Flatpak (`com.google.Chrome`, `com.microsoft.Edge`, `com.brave.Browser`, `org.chromium.Chromium`) before native. Native browsers only from fixed paths (`/opt/google/chrome`, `/usr/bin/…`, `/opt/brave.com`, `/opt/microsoft`), root-owned and not group/other writable (file and real dir), never PATH; snaps skipped (`/snap/…`, `/usr/bin/snap`, wrapper scripts mentioning `/snap/`). Flatpak runs get `--nodevice=all --device=dri --device=input` (input kept: YouTube's TV UI reads gamepads) `--nofilesystem=host/home/xdg-documents/xdg-download`. argv: `--user-data-dir=<per profile> --kiosk --no-first-run --no-default-browser-check --password-store=basic --hide-crash-restore-bubble --noerrdialogs --ozone-platform-hint=auto [kids limits] [--user-agent=…] --app=<url>`. URLs: printable ASCII, `https://` only, no credentials, DNS host or strict dotted-quad IPv4 (no `127.1` / `0x7f.1` / `0177.0.0.1`) on the definition's host allowlist |
| D23 | **Per-profile browser data**: Flatpak `~/.var/app/<browser>/data/mightyos/profiles/<id>`, native `$XDG_DATA_HOME/mightyos/profiles/<id>/browsers/<browser>` | MightyOS never creates, reads or trusts anything there (S1 lesson); kids profiles don't inherit logins |
| D24 | **Moonlight hosts/apps come from Moonlight's own `Moonlight.conf`** (read-only, 1 MiB cap, own INI parser, `[hosts]` only); never `moonlight list` | Keys `moonlight:<uuid>` / `moonlight:<uuid>/<percent-encoded app>` (host part lower case); launch `moonlight stream -- <uuid> <app>`. The app list is whatever Moonlight cached last. Kids allow-lists name every app (a host key is no wildcard). Checked in moonlight-qt's source: CLI `stream` never falls back to the full GUI (a failure shows an error dialog, then quits; the session's end quits); if another app already runs on the host it asks whether to quit it, which a kids profile can answer too (open question) |
| D25 | **Guide held closes web apps and streams** (`Launcher::terminate()`: SIGTERM to the process group, SIGKILL to the same pgid after 3 s if anything is left; untracked launches only, never a Steam launch) | They have no exit a pad reaches and no unsaved state. The TV remote may only do it in "full" mode and with **two Home presses within 1.5 s** (the first arms "Press Home again to close"); in "navigation only" it can't close apps. Games and other apps still wait for the M6 overlay. A pgid is only a number: M6's per-app cgroups (systemd scopes) replace it with an exact kill. Verified on the test PC: a group SIGTERM removes `flatpak run`/bwrap, xdg-dbus-proxy and the app |

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
| Favorite | X | Insert, Ctrl+D | Context menu |
| Search | Y | `/`, Ctrl+F | Search tile |
| Options | Start | Tab, Menu, Shift+F10 | Right click |
| Quick panel (volume, Wi-Fi, power) | Select | F12 | Clock area |
| Previous / next category | LB / RB | PgUp / PgDn | Click the category |
| Jump A–Z in grids | LT / RT (letter groups) | Home / End, letter and digit keys (D19) | Wheel |
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

- PIN storage: argon2id (64 MiB, t=3), in a root-only `0600` file. Lockout persists across restarts, with **exponential back-off capped at 1 hour** (not a permanent disable, which an injected-input attacker could use to lock out the owner). There is no master PIN and no reset backdoor.
- Secure entry (PIN pad, admin on-screen keyboard):
  - The layout and the starting cursor position are randomised for each prompt.
  - InputRouter suppresses action logging and the `received` signal while secure entry is open, so gamepad devices shared with other processes and debug logs can't replay the entry.
  - Prefer a physical keyboard for the admin password when one is present.
  - Consider `EVIOCGRAB` on MightyOS's own evdev fds during the prompt.
- Every `Text` showing outside strings (device names, `.desktop` titles, toasts, dialogs) uses `Text.PlainText`. Device names are cleaned of control and bidi characters.
- **Parental lock and kids mode are UI features, not a security boundary**, and will be labelled that way. All profiles run as the same Unix user, so a determined child who reaches a terminal or browser could get around them. The mitigations:
  - kids profiles have no store, settings, general browser or "Close to desktop"; a web app a parent allows *is* a browser, with the limits below
  - leaving a kids profile requires the PIN, checked by the helper
  - the session starts in the last profile, or a chosen default kids profile.
  A stronger later option is a separate Unix user per kids profile; that would be a session switch.
- **Kiosk web apps (M5):** an allowed web app is a Chromium browser in kiosk mode. Standard profiles: the host allowlist governs only the URL a tile starts; links can lead anywhere. Kids profiles get per-launch network limits: `--proxy-server=127.0.0.1:9` (a dead proxy) with `--proxy-bypass-list=<-loopback>;<hosts + kidsHosts>` (only the service and the CDNs it needs are reachable; IP literals, loopback and every other site fail) and `--host-resolver-rules` (`MAP * ~NOTFOUND` except those hosts; YouTube is mapped to `restrict.youtube.com`, Google's DNS-based Restricted Mode; whether youtube.com/tv honours it is **not verified**). The kidsHosts CDN lists are best effort and need hands-on checks. Optional managed policy `data/browser-policies/mightyos-kiosk.json` (DevTools off, file:/chrome:/external schemes blocked, no "always open" for protocols, no downloads, no extensions): an install-time step for the admin, machine-wide for that browser (all profiles), e.g. `/etc/opt/chrome/policies/managed/`, `/etc/chromium/policies/managed/`, `/etc/brave/policies/managed/`, `/etc/opt/edge/policies/managed/`; MightyOS never installs it, and Flatpak browsers' policy locations are not verified. Kids can't add, hide or remove tiles. Deleting a profile always deletes its browser data (logins), saves kept or not; the Flatpak dirs are removed with an `openat`/`O_NOFOLLOW`/`unlinkat` walk.
- **Kids mode as of M4 (not yet enforced by a PIN):** a kids profile sees only what its `allowed.json` lists; Home has no App Store or Settings; Settings screens, All apps, Artwork, Controllers, Power and the profile picker/editor are refused ("Ask a parent"). Leaving a kids profile or changing its kind needs the PIN, which arrives with `mightyos-helper` in M8, so **until M8 it is simply not possible from inside the kids profile**, and a kids profile used last starts without the picker. Changing a kids profile from a standard profile is not PIN-protected yet. RetroArch runs in kiosk mode for kids (no online/core updater, Load Core, Load Content). The Steam client is unrestricted once it runs (store, browser, other games): for kids profiles MightyOS sends `steam -shutdown` after the tracked game ends, but during the game Steam's overlay is reachable.
- **Emulator configuration written by MightyOS:** RetroArch gets a per-launch `--appendconfig` from `$XDG_STATE_HOME/mightyos/profiles/<id>/retroarch/` (written without following symlinks: `JsonFile::writePrivate`; never inside the sandbox-writable `~/.var/app` tree; the Flatpak gets that dir with `--filesystem=<dir>:ro`). It turns network commands, stdin commands, netplay announcing and Discord presence off for every profile.
- **Per-profile saves:** RetroArch (native and Flatpak) and Dolphin (`-u`, a per-profile user folder in its own Flatpak data dir, which also means per-profile Dolphin settings and controller mappings) keep saves per profile. **Gaps:** PCSX2, PPSSPP, melonDS, Azahar, mGBA, Flycast and DuckStation have no reliable save-folder option we use yet, so their saves are shared by all profiles.
- **ROM access for Flatpak emulators:** read-only `--filesystem` overrides for the ROM folders are still to do (M6/M7); until then a Flatpak emulator needs its own access to `~/ROMs`.
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
| **M2** | Data layer & profiles | AppRegistry (`.desktop` files and manifests), **ProfileService + profile picker screen** (create, rename, avatar, choose), per-profile favorites (X to toggle, reorder), async artwork and cache, navstate persistence. **From the M1 review:** a C++ model contract (`indexOfKey` backed by a hash, `get(i)`, `firstIndexForLetter`), replacing the JS `ListModel.get()` loops | general-purpose + gui-expert | code-reviewer |
| **M3** | Gamepad & TV remote | SDL3 thread, key repeat, hotplug, button glyphs (Xbox/PS/Nintendo), rumble. **HDMI-CEC** via libcec or `/dev/cec*`, feeding the same actions. **Needs real hardware** (gamepad, and a CEC adapter or CEC-capable PC). **From the M1 review:** join input threads before the engine is destroyed; coalesce queued navigate repeats | gui-expert | code-reviewer + security-expert (device access) |
| **M4** | Emulators & Steam library | System definitions, RomScanner + SQLite, game grid, RetroArch and standalone emulator launching, per-game core override. **Steam as a system** (`.acf` parsing, launching via `steam://rungameid`, detecting game exit). **SteamGridDB scraper** (API key, rate limits, cache). **From the M1 review:** letter-jump key mapping (F/M/Q clash with A–Z); resolve navstate `params.system` through the registry, never as a path. **From the M2 review:** TileListModel batch insert/remove, incremental key hash, diff-based `setItems`, debounced `Library::changed` (needed for 20k-ROM libraries); profile deletion asks whether to keep saves (default: keep) | general-purpose + gui-expert | code-reviewer + security-expert (launch hygiene, API key) |
| **M5** | Media & streaming | Kodi, Jellyfin, Plex tiles; web-app tiles (kiosk browser); Moonlight host/app tiles | gui-expert | code-reviewer |
| **M6** | Session | cage session files, scope-based launching, dormant mode while an app runs, Guide overlay, crash recovery, VT/SysRq hardening, SIGTERM → clean quit (flushes navstate) | general-purpose | security-expert |
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
- [x] M1 navigation framework, reviewed and fixed. C++ InputRouter, NavStack with persisted focus paths, TileRow/TileGrid/menus/dialogs, mock Emulators/Steam screens, LB/RB system stepping, 5 test targets (52 QML cases)
- [x] M2 data layer & profiles, reviewed and fixed. TileListModel contract, AppRegistry (desktop/Flatpak/manifests, cached startup), ProfileService + picker (startup rule: picker only with 2+ profiles), per-profile favorites/history, ArtworkProvider with bounded caches, 322 test cases
- [x] M3 gamepad & TV remote, code- and security-reviewed and fixed. SDL3 GamepadService with PadHub poll wake-ups (~0% idle CPU), mapping/repeat/coalescing, Guide, rumble, Xbox/PS/Nintendo glyphs, CEC kernel and fake backends with trust levels (D16–D18), plain-text UI, `mightyos-inputtest`. Verified on the test PC with an Xbox Series X pad: detection, build, tests, and a hands-on test by the user on the TV under cage (all buttons, D-pad, stick and diagonal, Guide, hot-plug, one rumble pulse at row end). **Untested on hardware:** kernel CEC, hidraw pads, Steam
- [x] M4 emulators & Steam library, code- and security-reviewed and fixed. systems.d registry (19 default systems, strict validation, layers), RomScanner (contained walk, title cleaning, multi-disc) + library.sqlite cache, RetroArch / Flatpak / native launching with per-profile emulator choice and saves, Steam (VDF/ACF parser, roots, libraries, art, `steam://rungameid`, exit tracker D20), SteamGridDB scraper (opt-in key D21, allowlist, caps, backoff, LRU), A–Z jump (D19), "+N more" systems, kids allow-lists (`allowed.json`), diff-based TileListModel, debounced `Library::changed`, delete-profile keeps saves by default, `--mock-library`, cage `--fullscreen` fix (fullscreen requested after the first frame). **Review follow-ups:** "Forget missing games" option for deliberately deleted ROM folders (M5); per-profile saves for PCSX2/PPSSPP/melonDS/Azahar/mGBA/Flycast/DuckStation (§4). **Pending:** hands-on test with real emulators and Steam (none installed on the test PC); Flatpak ROM-folder read-only overrides (M6/M7)
- [x] M5 media & streaming, code- and security-reviewed and fixed (kids web-app network limits, remote double press, pgid-keyed SIGKILL, fixed browser paths, browser data deleted with the profile, strict IPv4, Moonlight apps named one by one). Media screen (Apps / Streaming / Game streaming rows): well-known media apps via AppRegistry (Kodi `tv.kodi.Kodi`, Jellyfin `org.jellyfin.JellyfinDesktop` / `com.github.iwalton3.jellyfin-media-player`, Plex HTPC `tv.plex.PlexHTPC`, Plexamp `com.plexamp.Plexamp`, Moonlight `com.moonlight_stream.Moonlight`) plus every media-category app, "Get it" tiles for missing ones (no install); `webapps.d` with 6 built-ins (YouTube TV, Netflix, Disney+, Prime Video, Twitch, Spotify) and Settings > Media to add (user-<name>.json, 0600), hide, show and remove tiles; Moonlight hosts and app grids (D22–D25); kids allow-list `allowed.json` `"apps"` for apps, web apps and Moonlight (default deny, also in favorites/history); "Forget missing games" option; favorites/history/Unavailable for the new kinds. **Pending:** hands-on test with a real browser, Moonlight host and media apps (none installed on the test PC); whether YouTube's TV UI accepts the shipped user agent
