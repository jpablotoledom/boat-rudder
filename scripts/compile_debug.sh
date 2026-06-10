#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

source ./scripts/show/banner

user="$(whoami)"
echo "Hi ${user}!"

source ./scripts/show/divbar
echo "1 - Cleaning build/ and bin/ ..."
sleep .3
rm -rf ./build ./bin

source ./scripts/show/divbar
echo "2 - Compiling (Debug + AddressSanitizer) ..."
sleep .3

mkdir -p build
cmake -B build -DCMAKE_BUILD_TYPE=Debug .
cmake --build build

source ./scripts/show/divbar
echo "3 - Assembling bin/ ..."
sleep .3

mkdir -p bin
cp ./build/base-http-server ./bin/base-http-server
cp -r ./configs ./bin/configs

# Copy html/ if it exists
[ -d ./html ] && cp -r ./html ./bin/html

# Copy ssl/ only if it contains files
if [ -d ./ssl ] && [ -n "$(ls -A ./ssl 2>/dev/null)" ]; then
    cp -r ./ssl ./bin/ssl
fi

source ./scripts/show/divbar
echo "4 - Removing build/ ..."
sleep .3
rm -rf ./build

source ./scripts/show/divbar
echo "Done! Debug binary ready in bin/"
echo "Run with: ./bhs.sh rundebug"
echo ""
