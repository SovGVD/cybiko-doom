# Doom for Cybiko

A Doom port for the Cybiko Xtreme: a native CyOS application in H8S code
that plays the original maps on the 160x100, 4-grey screen. It runs on a
real Cybiko Xtreme and in the [Dragonfruit](https://github.com/SovGVD/Dragonfruit-JC4827W543-esp32)
Cybiko emulator. It uses no emulator hooks: the game talks to the Xtreme's
hardware (LCD controller, key matrix, timer) directly.

The 18 MHz H8S is far too slow for real Doom, so the picture is simplified:

- walls have their textures shrunk to a quarter (one texel for 4x4
  pixels) in 16 tones, dithered to the 4 greys; floors and ceilings are flat
  dithered shades (from each flat's average brightness, the sector light and
  the distance); edges are drawn black. Flat walls are a title menu choice
  and run faster;
- monsters, items, decorations and the weapon are their sprites shrunk to
  at most 32x32 cells in 16 tones, drawn in the 4 greys with a black
  outline;
- the view is 80 columns of 2 pixels (Doom's low detail) and 84 rows, above
  a 16-row status bar.

The game logic follows Doom: BSP rendering, blockmap collision, monster AI
with sight checks, all weapons, pickups, doors, lifts, stairs, crushers,
teleports, secrets and exits, and the five skill levels. In the emulator
E1M1 runs at 12-31 frames a second with textured walls and 15-35 with flat
ones (35 is the game's tic rate), and at 10-14 in its widest view with
monsters. Hiding the weapon in the title menu makes it a little faster.

## Building

Needs Python 3 (with nothing extra) and the GNU H8 tools (Debian/Ubuntu:
`binutils-h8300-hms gcc-h8300-hms`).

    ./build.sh doom1.wad                    # all the WAD's maps
    ./build.sh doom1.wad E1M1,E1M2          # pick the maps

This makes two files:

- `build/doom.app`, the game (about 55 KB). Put it on the Cybiko with the
  Cybiko console software, or into `/Cybiko/apps` on the Dragonfruit SD
  card. It shows up in Games (`APPTYPE=root ./build.sh ...` puts it on the
  main desktop instead).
- `build/doom.dat`, the level pack: the maps with their shrunk wall
  textures, and the shrunk sprites. The
  game looks for it on the MP3 cartridge's card first (`/MP3/doom.dat`, the
  card's top folder; on Dragonfruit that is `/Cybiko/card/doom.dat` on the
  SD card) and then in the Cybiko's own storage (next to the app). The
  shareware episode makes a 940 KB pack, which wants the card; the Cybiko's
  own storage holds only a few maps (60-150 KB each), so pick them with the
  second argument there.

The game reads the pack's sprites when it starts and each map when it is
played. It needs about 350 KB of RAM, plus the sprites (about 40 KB) and
room for the biggest map in the pack (up to 150 KB in the shareware
episode).

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
- `cybiko/` the Cybiko side: hardware access and file reading
  (`hw_cybiko.c`), runtime and division helpers (`rt.s`), the CyOS
  functions it calls (`cyos.ld`), the link script, icon and help page.
- `tools/wad2cy.py` converts a WAD into the level pack;
  `tools/cybiko_mkapp.py` and `tools/cybiko_cap.py` make the CyOS `.app`.
- `pc/` a PC build of the game code for testing: it writes frames as PGM
  images (`make -C pc`, then `pc/doom_pc doom.dat --frames N --shots DIR`).

## Notes on the Cybiko

- The game turns interrupts off while it runs and so has the whole CPU;
  it resets the watchdog itself and gives CyOS its interrupts back on exit.
- The H8S's 8 KB of on-chip RAM takes one state an access against the
  DRAM's three, so while the game runs it holds the screen buffer, the stack,
  the renderer's hot tables and its fill loops (`cybiko/link.ld`); CyOS's
  contents are kept aside and put back on exit, and the DTC is off meanwhile.
- The pack is read with CyOS's own file functions (`FileInput`,
  `Input_read`; `cybiko/cyos.ld` has their export numbers from the Cybiko
  SDK). The app imports them like CyOS's own applications do:
  `cybiko_mkapp.py` turns calls to `0xf00000 + N` into imports of kernel
  export N. To read a map while the game runs, the game hands the machine
  back to CyOS for that time (its on-chip RAM, the DTC and the interrupts,
  on CyOS's stack), so CyOS's own screen can show for a moment while a map
  loads.
- The MP3 cartridge's card is CyOS's `/MP3/` only with the cartridge
  software installed. In the emulator the newer set (`sdflash.dl` 21.1.4,
  in the "MP3Soft" pack) mounts the card; an older set (`sdflash.dl`
  21.1.3, "AddPack") did not: its card start stopped after reading the
  card's CSD.
- CyOS loads only a small zeroed data area, so the game's tables are stored
  as zeros in the module (the `.app` compression removes them).
- The CyOS decompressor has an 8 KB window: `cybiko_cap.py` makes no back
  reference further than 8191 bytes, or large apps load corrupted.

## Licence

MIT for the code here. Doom is a trademark of id Software; this is an
unofficial fan port; its code was written for the Cybiko and it contains no id Software data.
