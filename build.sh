#!/bin/sh
# Builds Cybiko Doom: ./build.sh WAD [MAPS]
#
#   WAD   your Doom IWAD: doom1.wad (shareware), doom.wad, or Freedoom's
#         freedoom1.wad. Its maps and sprites are converted into the level
#         pack, build/pack/ (doom.dat and a file per map); the WAD is not in
#         this repository, and the pack should not be shared unless the
#         WAD's licence allows it (Freedoom's does; the shareware one only
#         allows passing on the unmodified shareware package).
#   MAPS  comma-separated maps to put in the pack (default: all of them).
#         The shareware episode's 9 maps make 343 KB; the game plays the
#         maps whose files it finds, so you can also copy over only some.
#
# Needs Python 3 and the GNU H8 tools (Debian/Ubuntu: binutils-h8300-hms
# gcc-h8300-hms). APPTYPE=root puts the icon on the main desktop instead of
# Games. TEXELS=8 or 16 sets the most texels along a wall texture's longer
# side (default 32); TEXELS=0 leaves the textures out (flat walls only).
# The results are build/doom.app, the game, build/pack/, the level pack
# (see README.md for where they go), and build/doomtest.app, a test of the
# files, memory and keys on a real Cybiko (README.md, "Doom Test").
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
rm -rf build/pack
python3 tools/wad2cy.py "$1" build/pack $MAPS --texels "${TEXELS:-32}"
objs=""
for f in src/*.c cybiko/hw_cybiko.c; do
    o=build/$(basename "$f" .c).o
    $CC $CFLAGS -c "$f" -o "$o"
    objs="$objs $o"
done
h8300-hitachi-coff-as cybiko/rt.s -o build/rt.o
for base in 0 0x10000; do
    h8300-hitachi-coff-ld -mh8300s -T cybiko/link.ld -Ttext $base -o build/at$base.coff \
        $objs build/rt.o
done
python3 tools/cybiko_mkapp.py --at0 build/at0.coff --at1 build/at0x10000.coff \
    --name Doom --type "${APPTYPE:-game}" --icon cybiko/icon.txt --help-file cybiko/help.txt build/doom.app

# Doom Test (cybiko/doomtest.c): the same code with a test screen in place
# of the game, for checking files, memory and keys on a real Cybiko
$CC $CFLAGS -DDOOM_TEST -c cybiko/hw_cybiko.c -o build/hw_test.o
$CC $CFLAGS -c cybiko/doomtest.c -o build/doomtest.o
tobjs=$(echo "$objs" | sed 's#build/hw_cybiko.o#build/hw_test.o build/doomtest.o#')
for base in 0 0x10000; do
    h8300-hitachi-coff-ld -mh8300s -T cybiko/link.ld -Ttext $base -o build/test$base.coff \
        $tobjs build/rt.o
done
python3 tools/cybiko_mkapp.py --at0 build/test0.coff --at1 build/test0x10000.coff \
    --name "Doom Test" --type "${APPTYPE:-game}" --icon cybiko/icon.txt \
    --help-file cybiko/doomtest-help.txt build/doomtest.app
