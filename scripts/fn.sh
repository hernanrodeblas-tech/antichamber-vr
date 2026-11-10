#!/bin/sh
# fn.sh <hex-address> [context-bytes-after]
# Finds the start of the function containing the address (previous int3
# padding run) and disassembles from there to a bit past the address.
cd "$(dirname "$0")/.."
B=tools/llvm-mingw-20261104-ucrt-i686/bin
EXE=../Binaries/Win32/UDK.exe
ADDR=$((0x$1))
AFTER=${2:-48}
START=$((ADDR - 0x1800))
$B/llvm-objdump.exe -d --no-show-raw-insn --x86-asm-syntax=intel --start-address=$START --stop-address=$((ADDR + AFTER)) $EXE \
  | sed -E 's/<[^>]*>//' | awk -v addr="$1" '
    /int3/ { if (!inpad) { inpad=1 } next }
    /^ *[0-9a-f]+:/ { if (inpad) { n=0; inpad=0 } lines[n++]=$0 }
    END { for (i=0;i<n;i++) print lines[i] }'
