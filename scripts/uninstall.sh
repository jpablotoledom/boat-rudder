#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

INSTALL_DIR="/usr/local/bin/boat-rudder"
SERVICE_NAME="boat-rudder"
SYSTEMD_DIR="/etc/systemd/system"

source ./scripts/show/divbar
echo "Uninstalling Boat Rudder ..."
source ./scripts/show/divbar

if [ "$(uname -s)" != "Linux" ]; then
    echo "Error: uninstall is only supported on Linux (systemd)."
    exit 1
fi

# Stop and disable service if it exists
if systemctl list-unit-files | grep -q "$SERVICE_NAME.service"; then
    echo "1 - Stopping and disabling service ..."
    systemctl stop    "$SERVICE_NAME.service" 2>/dev/null || true
    systemctl disable "$SERVICE_NAME.service" 2>/dev/null || true
    rm -f "$SYSTEMD_DIR/$SERVICE_NAME.service"
    systemctl daemon-reload
    echo "    Service removed."
else
    echo "1 - Service not found, skipping."
fi

# Remove installed files
if [ -d "$INSTALL_DIR" ]; then
    echo "2 - Removing $INSTALL_DIR ..."
    rm -rf "$INSTALL_DIR"
    echo "    Removed."
else
    echo "2 - $INSTALL_DIR not found, skipping."
fi

source ./scripts/show/divbar
echo "Uninstall complete."
# Show remaining units matching the service name, if any
systemctl list-unit-files 2>/dev/null | grep "$SERVICE_NAME" || true
echo ""
