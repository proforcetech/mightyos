# MightyOS UI mockups and focus maps (M1)

ASCII sketches of the M1 screens as built, and how focus moves between
them. Sizes are authored for 1920x1080 and scale with the window; the outer
frame is the 5% TV safe area. Content is mock data until M2/M4.

## Home (depth 1)

```
+--------------------------------------------------------------------------+
| MightyOS                                                          10:42  |
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
                                        saved across restarts)
LB/RB (PgUp/PgDn): previous/next category, from either row.
Accept on a category -> its screen. Accept on a tile -> open/launch it.
```

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

## CategoryScreen (Media / App Store / Settings placeholders)

One TileRow of the category's items; the real screens arrive in M5, M7 and
M8. Settings -> Power opens the Dialog below.

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
