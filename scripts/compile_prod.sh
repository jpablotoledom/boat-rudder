#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

source ./scripts/show/banner

user="$(whoami)"
echo "Hi ${user}!"

# Kept between runs and separate from the debug objects - see compile_debug.sh.
BUILD_DIR=./build/release

source ./scripts/show/divbar
echo "1 - Compiling (Release) ..."
sleep .3

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release .
cmake --build "$BUILD_DIR"

source ./scripts/show/divbar
echo "2 - Installing binary into bin/ ..."
sleep .3

# bin/ holds the binary only - see compile_debug.sh. Deployment copies of
# configs/, html/ and ssl/ are made by scripts/install.sh, straight from the
# project root.
mkdir -p bin
cp "$BUILD_DIR/boat-rudder" ./bin/boat-rudder

# Strip the copy, never the one in $BUILD_DIR: stripping the build output would
# make the next incremental build ship an already-stripped binary silently.
# strip is available on both Linux and macOS.
if command -v strip &>/dev/null; then
    strip ./bin/boat-rudder
fi

source ./scripts/show/divbar
echo "Done! Production binary ready at bin/boat-rudder"
echo ""
