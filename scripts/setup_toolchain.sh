#!/bin/sh
# Downloads the llvm-mingw toolchain (32-bit, UCRT) used by build.sh into tools/.
set -e
cd "$(dirname "$0")/.."
VER=20261104
NAME=llvm-mingw-$VER-ucrt-i686
if [ -d tools/$NAME ]; then
    echo "tools/$NAME already present"
    exit 0
fi
mkdir -p tools
curl -L --fail -o tools/$NAME.zip https://github.com/mstorsjo/llvm-mingw/releases/download/$VER/$NAME.zip
unzip -q tools/$NAME.zip -d tools
rm tools/$NAME.zip
echo "toolchain ready: tools/$NAME"
