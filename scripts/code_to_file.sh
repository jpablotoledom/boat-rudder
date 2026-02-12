#!/bin/bash
find ./src \( -path "./src/html" -o -path "./src/lib" \) -prune -o -type f -exec sh -c 'echo "=== {} ==="; cat "{}"; echo ""' \; > source_code_condensed.txt