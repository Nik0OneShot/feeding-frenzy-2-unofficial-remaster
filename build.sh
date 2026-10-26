#!/bin/sh
# Builds everything into out/:
#   version.dll    the patch, loaded by the game (src/)
#   ff2online.dll  online play's host, loaded by the patch with online=1 (online/)
#   ff2guest.exe   the viewer friends run to join online (guest/)
#   FF2 Remaster Setup.exe   the installer, with all of the above inside it (installer/)
# Needs a 32-bit MinGW-w64 (i686): set MINGW to its bin folder, or have
# i686-w64-mingw32-gcc and i686-w64-mingw32-windres on PATH.
set -e
cd "$(dirname "$0")"
if [ -n "$MINGW" ]; then
    GCC="$MINGW/gcc"
    WINDRES="$MINGW/windres"
else
    GCC=i686-w64-mingw32-gcc
    WINDRES=i686-w64-mingw32-windres
fi
CFLAGS="-O2 -Wall -Wextra -static -s"
mkdir -p out

"$GCC" $CFLAGS -shared -o out/version.dll src/*.c shared/label.c src/version.def -lgdi32
echo "built out/version.dll"
MF_LIBS="-lole32 -loleaut32 -lmfuuid -lstrmiids -lwmcodecdspuuid -luuid"
"$GCC" $CFLAGS -shared -o out/ff2online.dll online/*.c shared/*.c -lws2_32 -lwinmm -lgdi32 $MF_LIBS
echo "built out/ff2online.dll"
"$GCC" $CFLAGS -mwindows -o out/ff2guest.exe guest/*.c shared/*.c -lws2_32 -lopengl32 -lgdi32 -lwinmm $MF_LIBS
echo "built out/ff2guest.exe"

"$WINDRES" -I installer installer/setup.rc -O coff -o out/setup.res
# Setup has a manifest of its own (modern controls, DPI aware): an empty stand-in for
# MinGW's default-manifest.o, found first through -B, keeps the two from clashing.
mkdir -p out/no-default-manifest
echo "" | "$GCC" -c -x c - -o out/no-default-manifest/default-manifest.o
"$GCC" $CFLAGS -Bout/no-default-manifest/ -municode -mwindows -o "out/FF2 Remaster Setup.exe" installer/setup.c \
    installer/mods.c installer/saf.c out/setup.res -lcomctl32 -lshell32 -lole32 -lwindowscodecs -luuid
echo "built out/FF2 Remaster Setup.exe"
