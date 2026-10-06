#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

INSTALL_DIR="/usr/local/bin/boat-rudder"
SERVICE_NAME="boat-rudder"
SERVICE_FILE="$SCRIPT_DIR/boat-rudder.service"
SYSTEMD_DIR="/etc/systemd/system"

source ./scripts/show/banner

# Require Linux with systemd
if [ "$(uname -s)" != "Linux" ]; then
    echo "Error: install is only supported on Linux (systemd)."
    echo "On macOS, run the server directly with: ./boat_rudder_builder.sh rundebug"
    exit 1
fi

if ! command -v systemctl &>/dev/null; then
    echo "Error: systemd not found. Install requires a systemd-based Linux distribution."
    exit 1
fi

source ./scripts/show/divbar
echo "1 - Compiling for production ..."
sleep .3

# Always compile fresh for install
./scripts/compile_prod.sh

source ./scripts/show/divbar
echo "2 - Installing to $INSTALL_DIR ..."
sleep .3

mkdir -p "$INSTALL_DIR"
# install(1) replaces the file instead of writing into it, so this also works
# while the service is running the old binary ("Text file busy" with cp).
install -m 755 ./bin/boat-rudder "$INSTALL_DIR/boat-rudder"

# Every "cp -r ./dir $INSTALL_DIR/dir" below would copy INTO the existing
# directory on a reinstall (configs/configs, html/html, ...) and update
# nothing - hence cp -rT / per-file copies.

# configs/settings.conf is per-host (ports, WAP gateway IPs/interface, log
# level...): installed once, never overwritten. A reinstall drops the repo's
# version next to it as settings.conf.dist and lists any setting the
# installed file doesn't have yet.
mkdir -p "$INSTALL_DIR/configs"
if [ ! -f "$INSTALL_DIR/configs/settings.conf" ]; then
    cp ./configs/settings.conf "$INSTALL_DIR/configs/settings.conf"
    echo "Installed configs/settings.conf - review it for this host."
else
    cp ./configs/settings.conf "$INSTALL_DIR/configs/settings.conf.dist"
    missing="$(grep -oE '^[a-z_]+=' ./configs/settings.conf | while read -r key; do
        grep -q "^$key" "$INSTALL_DIR/configs/settings.conf" || echo "  ${key%=}"
    done)"
    echo "Kept the existing configs/settings.conf (repo version: settings.conf.dist)."
    if [ -n "$missing" ]; then
        echo "Settings not in the installed file (their built-in defaults apply):"
        echo "$missing"
    fi
fi

# html/ is required at runtime. Merged over the installed copy: templates
# and themes are updated, and files that only exist there - media uploaded
# from the dashboard into html/content - are left alone.
if [ -d ./html ]; then
    mkdir -p "$INSTALL_DIR/html"
    cp -rT ./html "$INSTALL_DIR/html"
else
    mkdir -p "$INSTALL_DIR/html"
    echo "Warning: html/ not found. Add your retro-compatible CMS files to $INSTALL_DIR/html/"
fi

# data/ holds the GeoLite2 database analytics resolves countries with
# (GEOIP_DEFAULT_DB_PATH is relative to WorkingDirectory) - without it every
# visit is recorded as country "Unknown".
if [ -f ./data/GeoLite2-Country.mmdb ]; then
    mkdir -p "$INSTALL_DIR/data"
    cp ./data/GeoLite2-Country.mmdb "$INSTALL_DIR/data/GeoLite2-Country.mmdb"
else
    echo "Warning: data/GeoLite2-Country.mmdb not found - visitor countries will be \"Unknown\""
fi

# ssl/ is optional and only seeded on first install: the installed
# certificates are renewed in place, and a stale copy in the checkout must
# not overwrite them.
if [ -d ./ssl ] && [ -n "$(ls -A ./ssl 2>/dev/null)" ]; then
    if [ -z "$(ls -A "$INSTALL_DIR/ssl" 2>/dev/null)" ]; then
        mkdir -p "$INSTALL_DIR/ssl"
        cp -rT ./ssl "$INSTALL_DIR/ssl"
    else
        echo "Kept the existing ssl/ certificates."
    fi
fi

# The service appends to /var/log/boat-rudder.log forever otherwise.
if [ -d /etc/logrotate.d ]; then
    install -m 644 ./scripts/boat-rudder.logrotate /etc/logrotate.d/boat-rudder
fi

source ./scripts/show/divbar
echo "3 - Installing systemd service ..."
sleep .3

cp "$SERVICE_FILE" "$SYSTEMD_DIR/$SERVICE_NAME.service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME.service"
# restart, not start: on a reinstall the old binary is still running
systemctl restart "$SERVICE_NAME.service"

source ./scripts/show/divbar
echo "Done! Service status:"
systemctl status "$SERVICE_NAME.service" --no-pager || true
echo ""
echo "Logs: tail -f /var/log/boat-rudder.log"
echo ""
