# MightyOS UI mockups and focus maps (M2)

ASCII sketches of the screens as built, and how focus moves between them.
Sizes are authored for 1920x1080 and scale with the window; the outer frame
is the 5% TV safe area. Apps come from AppRegistry; systems and games are
mock data until M4.

## Home (depth 1)

```
+--------------------------------------------------------------------------+
| MightyOS                                         (bot) Player 1  10:42  |
|                                                                          |
| (*Favorites) ( Emulators ) ( Games ) ( Media ) ( App Store ) ( Settings ) |  row 0: CategoryBar
|                                                                          |
| Emulators                                                                |  info: focused item
| Retro and modern consoles                                                |
|                                                                          |
| +------+ +------+ +------+ +------+ +------+ +------+ +-----             |  row 1: TileRow
| | NES  | | SNES | | N64  | | PS1  | | PS2  | | PSP  | | Game...  (runs   |
| +------+ +------+ +------+ +------+ +------+ +------+ +-----   off-edge) |
|                                                                          |
| [Enter] Open   [M] Options   [PgUp PgDn] Category                        |  PromptBar
+--------------------------------------------------------------------------+
```

Focus map:

```
          Left/Right (bump at ends, no wrap)
row 0   [Fav] <-> [Emu] <-> [Games] <-> [Media] <-> [Store] <-> [Settings]
          |  ^
     Down |  | Up, or Back
          v  |
row 1   [tile] <-> [tile] <-> ...      (each category remembers its position,
                                        saved across restarts, per profile)
LB/RB (PgUp/PgDn): previous/next category, from any row. Passing a category
with no content drops focus to the bar; the next one with content brings
it back down.
Accept on a category -> its screen. Accept on a tile -> open/launch it.
X/F on a game, app or system tile -> toggle favorite (star badge, toast).
Options adds "Switch profile". The profile chip (top right) is mouse-only.
```

## Home > Favorites (row 1 = favorites in saved order, row 2 = Recent)

```
+--------------------------------------------------------------------------+
| MightyOS                                         (bot) Player 1  10:42  |
| (*Favorites) ( Emulators ) ( Games ) ( Media ) ( App Store ) ( Settings ) |  row 0
| Demo Player                                                              |
| Media player                                                             |
| #========#* +--------+*  +--------+                                      |  row 1: favorites
| # [Vim]  #  | [cart] |   |  ...   |    (* = star badge; a gone target   |
| # Demo   #  | Astro  |   | Kodi   |     is a greyed "Unavailable" tile)  |
| #========#  +--------+   +Unavail.+                                      |
| Recent                                                                   |
| +----+ +----+                                                            |  row 2: Recent (small,
| |Demo| |Astr|                                                            |  only if non-empty)
| +----+ +----+                                                            |
| [A] Launch [B] Categories [X] Favorite [Start] Options [LB RB] Category  |
+--------------------------------------------------------------------------+
```

Up/Down: bar <-> favorites <-> Recent (empty rows are skipped; with no
favorites, Down goes straight to Recent). Options on a favorite: Launch,
Move left / Move right (only where possible), Remove from Favorites,
Details. The moved tile keeps the focus. Accept on an "Unavailable" tile
opens a Keep (default) / Remove dialog. With no favorites the info line
reads "Press X (F) on any game, app or system to pin it here".

Accept on the Favorites category opens the Favorites grid (GameGrid with
`{ collection: "favorites" }`): the same order and options as a poster
grid; LB/RB keep their global meaning there.

## SystemList (depth 2): Emulators -> systems, Games -> [Steam]

```
+--------------------------------------------------------------------------+
| MightyOS > Emulators                                              10:42  |
| SNES                                                                     |
| Super Nintendo                                                           |
| +------+ #========# +------+ +------+ +------+ +------+ +-----           |
| | NES  | # SNES   # | N64  | | PS1  | | PS2  | | PSP  | | Game...        |
| +------+ #========# +------+ +------+ +------+ +------+ +-----           |
| [Enter] Open   [Esc] Back   [M] Options                                  |
+--------------------------------------------------------------------------+
```

Left/Right move, Accept opens GameGrid, Back returns to Home (exact tile),
LB/RB return to Home and switch category.

## GameGrid (depth 3)

```
+--------------------------------------------------------------------------+
| MightyOS > Emulators > SNES                                       10:42  |
| Haunted Hallways                                                         |
| 2004  .  17 of 40                                                        |
|   ...1989.. ...2004.. ...1990.. ...1999.. ...1986.. ...1987.. ...1995..  |  (row above peeks)
|   +-------+ +-------+ #=======# +-------+ +-------+ +-------+ +-------+  |
|   | Galaxy| | Gear  | #Haunted# | Hyper | | Iron  | | Jungle| |Kingdom|  |
|   +-------+ +-------+ #=======# +-------+ +-------+ +-------+ +-------+  |
|   +-------+ +-------+ +-------+ +-------+ +-------+ +-------+ +-------+  |
| [Enter] Play [Esc] Back [M] Options [Home End] Page [PgUp PgDn] System   |
+--------------------------------------------------------------------------+
```

Focus map:

```
Arrows: move; Left/Right bump at row ends (no wrap); Up from the first row
bumps; Down into a partial last row lands on its last tile.
Home/End (LT/RT): one page of visible rows up/down, same column.
PgUp/PgDn (LB/RB): previous/next system of the same category, in place:

   SystemList [NES][SNES][N64]...           Back from any of these grids
                      |                     returns to SystemList, focused
   GameGrid  SNES  <--LB/RB-->  N64  <-->  PS1 ...   on the system shown last.
             (replace, not push; bump at the first/last system;
              the Favorites grid keeps LB/RB = back to Home + switch)
```

## CategoryScreen (Media / App Store / Settings)

One TileRow. Media lists the registry's media apps (Categories=AudioVideo,
Audio, Video, TV, or manifest `category: "media"`), with their icon theme
artwork and the vector icon as fallback. App Store and Settings are
placeholders until M7 / M8, except:

```
Settings:  [Profiles] [All apps] [Display] [Audio] [Network] [Controllers] [Storage] [Power]
              |           |                                                          |
        profile picker  All apps grid                                         Power dialog
```

## All apps (Settings > All apps, depth 2)

A poster grid (same focus rules as GameGrid) of every registry app, sorted
by name: Accept launches with the parsed argv, X/F toggles a favorite,
LT/RT page. Empty: "No apps found" ("Looking for apps..." until the first
scan finishes).

## Profile picker ("Who's playing?")

Shown at startup only with 2+ profiles (last used preselected); also from
Home > Options > Switch profile, Settings > Profiles and the top-bar chip.
It covers the NavStack area and is modal (routed after Dialog/ContextMenu).

```
+--------------------------------------------------------------------------+
| MightyOS > Profiles                                               10:42  |
|                                                                          |
|                          Who's playing?                                  |
|                                                                          |
|            #==========#  +[Kids]---+  +----------+                       |
|            #  (bot)   #  | (rocket)|  |    +     |                       |
|            # Player 1 #  |  Pixel  |  |Add profile|                      |
|            #==========#  +---------+  +----------+                       |
|                                                                          |
| [A] Choose   [Start] Options   [B] Back (not at startup)                 |
+--------------------------------------------------------------------------+
```

Focus map:

```
[profile] <-> [profile] <-> ... <-> [Add profile]    (bump at the ends)
Accept on a profile -> choose it: NavStack.reset() + that profile's saved
                       path (same profile: just close).
Accept on Add       -> profile editor (create).
Options on a profile -> Choose / Rename / change avatar / Delete
                       Delete -> Dialog "Delete <name>?" (Keep is default
                       and the Back answer); the last profile: error toast.
Back                -> close (at startup: bump; a profile must be chosen).
                       If the current profile was deleted, the most
                       recently used remaining one is entered.
```

## Profile editor (create / edit, no on-screen keyboard yet)

```
   +------------------------------------------------------------+
   | (rocket)  New profile                                      |
   |           Pixel · Kids                                     |
   | Name     ( Comet ) (#Pixel#) ( Nova ) ( Blaze ) ( Echo ) .. |  row 0
   | Avatar   [bot] [cat] [ghost] [#rocket#] [alien] [bolt] ..   |  row 1
   | Colour   [ ] [ ] [ ] [*] [ ] [ ] [ ] [ ]                    |  row 2
   | Type     ( Standard ) (#Kids#)                              |  row 3
   | Kids profiles are marked with a badge. Restrictions ... M8. |
   |                               ( Cancel )  (#Create#)       |  row 4
   +------------------------------------------------------------+
[A] Next   [B] Cancel
```

Up/Down move between rows (bump at the ends); Left/Right change the
selection in rows 0-3 (focus = selection, bump at the ends) and pick a
button in row 4. Accept moves down a row, and in row 4 presses the button.
Back cancels. Names are generated suggestions not used by another profile
(edit mode lists the current name first). Mouse: click any chip or button.

## ContextMenu (Start / M / right click)

```
            +--------------------------------+
            | Haunted Hallways               |
            | #============================# |
            | # Play                       # |
            | #============================# |
            |   Add to Favorites             |
            |   Details                      |
            +--------------------------------+
[Enter] Select   [Esc] Close
```

Focus-trapped: Up/Down move (bump at ends), Accept picks, Back/Start or a
click outside closes. Nothing behind it moves.

## Dialog

```
   +-----------------------------------------------------+
   | Power off?                                          |
   | Power controls arrive in M8. ...                    |
   |                        (#Cancel#)  ( Power off )     |
   +-----------------------------------------------------+
[Enter] Select   [Esc] Cancel
```

Focus starts on the first non-destructive button; Back answers with it.
Left/Right move between buttons; clicks outside do nothing.

## Launch overlay

While an app runs: full-screen scrim, "<title> is running", all input
blocked. On exit focus is restored to the launched tile and input is muted
for 300 ms; a crash or non-zero exit shows a toast.
