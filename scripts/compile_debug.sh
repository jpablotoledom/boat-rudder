#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

source ./scripts/show/banner

user="$(whoami)"
echo "Hi ${user}!"

# One build directory per build type, kept between runs: CMake only recompiles
# the .c files whose sources or included headers actually changed. Separate
# directories also mean switching debug <-> prod does not invalidate the other
# one's objects. Use "./boat_rudder_builder.sh clean" to wipe them.
BUILD_DIR=./build/debug

source ./scripts/show/divbar
echo "1 - Compiling (Debug + AddressSanitizer) ..."
sleep .3

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug .
cmake --build "$BUILD_DIR"

source ./scripts/show/divbar
echo "2 - Installing binary into bin/ ..."
sleep .3

# bin/ holds the binary and nothing else: the server runs from the project root
# and reads ./configs, ./html and ./ssl directly, so copying them here would
# only create a second, immediately-stale copy of the whole content tree.
mkdir -p bin
cp "$BUILD_DIR/boat-rudder" ./bin/boat-rudder

source ./scripts/show/divbar
echo "Done! Debug binary ready at bin/boat-rudder"
echo "It runs against ./configs, ./html and ./ssl in the project root."
echo "Run with: ./boat_rudder_builder.sh rundebug"
echo ""
