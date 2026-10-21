#!/bin/bash
# Dump this site's MongoDB database into ./db_backup/<db>/
# The database name is read from configs/settings.conf (mongodb_db), so no site
# name is hardcoded here.
set -e

# An optional argument names another settings.conf - another install's:
# mongodb_dump.sh /path/to/install/configs/settings.conf
# A path that doesn't exist is an error, never a fallback to this checkout's
# database.
CONFIG=""
if [ -n "${1:-}" ]; then
    [ -f "$1" ] || { echo "Error: $1 not found."; exit 1; }
    CONFIG="$(realpath "$1")"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"
CONFIG="${CONFIG:-configs/settings.conf}"

DB="$(sed -n 's/^[[:space:]]*mongodb_db[[:space:]]*=//p' "$CONFIG" | tail -1 | tr -d ' \r')"
DB="${DB:-boat_rudder}"

echo "Dumping database '$DB' into ./db_backup/ ..."
mongodump --db "$DB" --out ./db_backup
