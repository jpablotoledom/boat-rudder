#!/bin/bash
# Restore this site's MongoDB database from ./db_backup/<db>/, dropping the
# existing collections first. The database name is read from
# configs/settings.conf (mongodb_db), so no site name is hardcoded here.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

DB="$(sed -n 's/^[[:space:]]*mongodb_db[[:space:]]*=//p' configs/settings.conf | tail -1 | tr -d ' \r')"
DB="${DB:-boat_rudder}"

if [ ! -d "./db_backup/$DB" ]; then
    echo "Error: ./db_backup/$DB not found. Run scripts/mongodb_dump.sh first."
    exit 1
fi

echo "Restoring database '$DB' from ./db_backup/$DB (existing collections are dropped) ..."
mongorestore --drop --db "$DB" "./db_backup/$DB"
