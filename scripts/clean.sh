#!/bin/bash
# Remove every build artifact: the compiler caches under build/ and the binary
# in bin/. Nothing else is touched - configs/, html/, ssl/ and db_backup/ are
# live data, never build output.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

source ./scripts/show/divbar

for dir in ./build ./bin; do
    if [ -d "$dir" ]; then
        echo "Removing $dir ($(du -sh "$dir" 2>/dev/null | cut -f1)) ..."
        rm -rf "$dir"
    else
        echo "$dir does not exist - nothing to do."
    fi
done

source ./scripts/show/divbar
echo "Done! The next compile will be a full one."
echo ""
