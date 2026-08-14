#!/bin/bash
# Dump this site's MongoDB database into ./db_backup/<db>/
# The database name is read from configs/settings.conf (mongodb_db), so no site
# name is hardcoded here.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

DB="$(sed -n 's/^[[:space:]]*mongodb_db[[:space:]]*=//p' configs/settings.conf | tail -1 | tr -d ' \r')"
DB="${DB:-boat_rudder}"

echo "Dumping database '$DB' into ./db_backup/ ..."
mongodump --db "$DB" --out ./db_backup
