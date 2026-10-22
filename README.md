# Doom for Cybiko

A Doom port for the Cybiko Xtreme: a native CyOS application in H8S code
that plays the original maps on the 160x100, 4-grey screen. It runs on a
real Cybiko Xtreme and in the [Dragonfruit](https://github.com/SovGVD/Dragonfruit-JC4827W543-esp32)
Cybiko emulator. It uses no emulator hooks: the game talks to the Xtreme's
hardware (LCD controller, key matrix, timer) directly.

The 18 MHz H8S is far too slow for real Doom, so the picture is simplified:

- walls, floors and ceilings are flat dithered shades (taken from each
  texture's average brightness, the sector light and the distance) with black
  edges instead of textures;
- monsters, items, decorations and the weapon are their sprites shrunk to
  at most 32x32 cells in 16 tones, drawn in the 4 greys with a black
  outline;
- the view is 80 columns of 2 pixels (Doom's low detail) and 84 rows, above
  a 16-row status bar.

The game logic follows Doom: BSP rendering, blockmap collision, monster AI
with sight checks, all weapons, pickups, doors, lifts, stairs, crushers,
teleports, secrets and exits, and the five skill levels. In the emulator
E1M1 runs at 15-35 frames a second (35 is the game's tic rate), and at about
14 with three monsters in its widest view. Hiding the weapon in the title
menu makes it a little faster.

## Building

Needs Python 3 (with nothing extra) and the GNU H8 tools (Debian/Ubuntu:
`binutils-h8300-hms gcc-h8300-hms`).

    ./build.sh doom1.wad                    # E1M1-E1M3
    ./build.sh doom1.wad E1M1,E1M2          # pick the maps

The result is `build/doom.app`. Put it on the Cybiko with the Cybiko console
software, or into `/Cybiko/apps` on the Dragonfruit SD card. It shows up in
Games (`APPTYPE=root ./build.sh ...` puts it on the main desktop instead).

The app holds the maps, so its size depends on them: three maps make a
160 KB app that needs about 550 KB of RAM when it runs. A real Cybiko has
little room for files, so keep to a few maps there.

## The WAD and its licence

No game data is in this repository. `build.sh` converts maps and shrunk
sprites from a Doom IWAD you supply into the app:

- `doom1.wad`, the shareware Doom 1.9 episode. Its licence allows passing on
  only the unmodified shareware package, so do not share a `doom.app` built
  from it.
- `doom.wad` / `doom2.wad`, the commercial games: for your own use only.
- `freedoom1.wad` / `freedoom2.wad` from [Freedoom](https://freedoom.github.io/),
  BSD-licensed: an app built from it can be shared.

## Keys

| Key | Action |
| --- | --- |
| Up/Down or W/S | walk |
| Left/Right | turn |
| Fn + Left/Right, A/D or `,` `.` | strafe |
| Shift | run (Select: always run) |
| Space or K | fire |
| Enter or E | open doors, press switches |
| F1-F7, Tab | weapons, next weapon |
| Menu or M | map |
| Help | frames per second on/off |
| Esc | pause; Esc again quits |

## Layout

- `src/` the game: renderer (`render.c`), map loading (`level.c`), things
  and physics (`mobj.c`), monsters (`ai.c`), the player (`player.c`), doors,
  lifts and other specials (`spec.c`), menus and the main loop (`main.c`).
- `cybiko/` the Cybiko side: hardware access (`hw_cybiko.c`), runtime and
  division helpers (`rt.s`), the link script, icon and help page.
- `tools/wad2cy.py` converts a WAD into the level pack;
  `tools/cybiko_mkapp.py` and `tools/cybiko_cap.py` make the CyOS `.app`.
- `pc/` a PC build of the game code for testing: it writes frames as PGM
  images (`make -C pc`, then `pc/doom_pc PACK.bin --frames N --shots DIR`
  with a pack from `tools/wad2cy.py WAD PACK.bin --bin`).

## Notes on the Cybiko

- The game turns interrupts off while it runs and so has the whole CPU;
  it resets the watchdog itself and gives CyOS its interrupts back on exit.
- The H8S's 8 KB of on-chip RAM takes one state an access against the
  DRAM's three, so while the game runs it holds the screen buffer, the stack,
  the renderer's hot tables and its fill loops (`cybiko/link.ld`); CyOS's
  contents are kept aside and put back on exit, and the DTC is off meanwhile.
- CyOS loads only a small zeroed data area, so the game's tables are stored
  as zeros in the module (the `.app` compression removes them).
- The CyOS decompressor has an 8 KB window: `cybiko_cap.py` makes no back
  reference further than 8191 bytes, or large apps load corrupted.

## Licence

MIT for the code here. Doom is a trademark of id Software; this is an
unofficial fan port; its code was written for the Cybiko and it contains no id Software data.
