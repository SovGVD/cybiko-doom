#!/bin/sh
# Builds Cybiko Doom: ./build.sh WAD [MAPS]
#
#   WAD   your Doom IWAD: doom1.wad (shareware), doom.wad, or Freedoom's
#         freedoom1.wad. It is converted into the game; it is not in this
#         repository, and the build output should not be shared unless the
#         WAD's licence allows it (Freedoom's does; the shareware one only
#         allows passing on the unmodified shareware package).
#   MAPS  comma-separated maps to include (default: E1M1-E1M3, or MAP01-03).
#         The app must fit the Cybiko's file space and its RAM: three maps
#         make a 160 KB app that needs about 550 KB of RAM. Each map adds
#         20-60 KB to the app and 40-120 KB to the RAM it needs.
#
# Needs Python 3 and the GNU H8 tools (Debian/Ubuntu: binutils-h8300-hms
# gcc-h8300-hms). APPTYPE=root puts the icon on the main desktop instead of
# Games. The result is build/doom.app: copy it to the Cybiko with
# the Cybiko console, or into /Cybiko/apps on the Dragonfruit SD card.
set -e
cd "$(dirname "$0")"
if [ -z "$1" ]; then
    echo "usage: $0 WAD [MAPS]" >&2
    exit 1
fi
mkdir -p build
CC=h8300-hitachi-coff-gcc
CFLAGS="-ms -O2 -fomit-frame-pointer -Wall -ffreestanding -fno-builtin"
if [ -n "$2" ]; then MAPS="--maps $2"; else MAPS=""; fi
python3 tools/wad2cy.py "$1" build/levels.s $MAPS
objs=""
for f in src/*.c cybiko/hw_cybiko.c; do
    o=build/$(basename "$f" .c).o
    $CC $CFLAGS -c "$f" -o "$o"
    objs="$objs $o"
done
h8300-hitachi-coff-as cybiko/rt.s -o build/rt.o
h8300-hitachi-coff-as build/levels.s -o build/levels.o
for base in 0 0x10000; do
    h8300-hitachi-coff-ld -mh8300s -T cybiko/link.ld -Ttext $base -o build/at$base.coff \
        $objs build/rt.o build/levels.o
done
python3 tools/cybiko_mkapp.py --at0 build/at0.coff --at1 build/at0x10000.coff \
    --name Doom --type "${APPTYPE:-game}" --icon cybiko/icon.txt --help-file cybiko/help.txt build/doom.app
