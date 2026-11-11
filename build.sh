#!/bin/sh
# Builds the Antichamber VR mod (32-bit d3d9.dll proxy) with llvm-mingw.
# Usage: sh build.sh [install]
set -e
cd "$(dirname "$0")"
TC=tools/llvm-mingw-20261104-ucrt-i686/bin
CXX=$TC/i686-w64-mingw32-clang++.exe
CC=$TC/i686-w64-mingw32-clang.exe
OUT=build
mkdir -p $OUT/obj

CFLAGS="-O2 -g0 -msse2 -mfpmath=sse -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Ideps/openvr -Ideps/minhook/include"
CXXFLAGS="$CFLAGS -std=c++17 -fno-exceptions -fno-rtti -Wall -Wno-unused-function"

OBJS=""
for f in deps/minhook/src/buffer.c deps/minhook/src/hook.c deps/minhook/src/trampoline.c deps/minhook/src/hde/hde32.c; do
    o=$OUT/obj/$(basename $f .c).o
    [ -f $o ] || $CC $CFLAGS -c $f -o $o
    OBJS="$OBJS $o"
done
for f in src/*.cpp; do
    o=$OUT/obj/$(basename $f .cpp).o
    $CXX $CXXFLAGS -c $f -o $o
    OBJS="$OBJS $o"
done

$CXX -shared -static -o $OUT/d3d9.dll $OBJS src/d3d9.def \
    -Wl,--kill-at -Wl,--enable-stdcall-fixup -ld3d11 -ldxgi -ldxguid -luuid -luser32 -lgdi32 -lwinmm
echo "built $OUT/d3d9.dll"

# Steam Frame helper: a stand-in for the PhysX installer that hangs under Proton/FEX.
$CC -O2 -s -mwindows steamframe/noop_installer.c -o $OUT/noop_installer.exe

if [ "$1" = "install" ]; then
    DEST=../Binaries/Win32
    cp $OUT/d3d9.dll $DEST/d3d9.dll
    cp deps/openvr/openvr_api.dll $DEST/openvr_api.dll
    [ -f $DEST/antichamber_vr.ini ] || cp config/antichamber_vr.ini $DEST/antichamber_vr.ini
    echo "installed into $DEST"
fi
