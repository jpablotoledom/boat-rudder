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
cp ./bin/boat-rudder "$INSTALL_DIR/boat-rudder"
chmod +x "$INSTALL_DIR/boat-rudder"

# configs/, html/ and ssl/ are taken straight from the project root - bin/ holds
# the binary only.
cp -r ./configs "$INSTALL_DIR/configs"

# html/ is required at runtime
if [ -d ./html ]; then
    cp -r ./html "$INSTALL_DIR/html"
else
    mkdir -p "$INSTALL_DIR/html"
    echo "Warning: html/ not found. Add your retro-compatible CMS files to $INSTALL_DIR/html/"
fi

# ssl/ is optional
if [ -d ./ssl ] && [ -n "$(ls -A ./ssl 2>/dev/null)" ]; then
    cp -r ./ssl "$INSTALL_DIR/ssl"
fi

source ./scripts/show/divbar
echo "3 - Installing systemd service ..."
sleep .3

cp "$SERVICE_FILE" "$SYSTEMD_DIR/$SERVICE_NAME.service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME.service"
systemctl start  "$SERVICE_NAME.service"

source ./scripts/show/divbar
echo "Done! Service status:"
systemctl status "$SERVICE_NAME.service" --no-pager || true
echo ""
echo "Logs: journalctl -u $SERVICE_NAME -f"
echo ""
