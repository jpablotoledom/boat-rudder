#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

DEFAULT_DIR="/usr/local/bin/boat-rudder"

usage() {
    echo "Usage: install.sh                 build and install as boat-rudder.service in $DEFAULT_DIR"
    echo "       install.sh --dir <path>    build and install the files only into <path>: no systemd"
    echo "                                  unit, logrotate rule or /usr/local/sbin/boat-rudder-update"
    echo "                                  link - for a tool that runs and updates the install itself"
    echo "                                  (several sites on one machine). The updater works on such"
    echo "                                  an install with BR_INSTALL_DIR and BR_SERVICE set."
    exit 2
}

# The directory is a command-line argument, never an environment variable:
# `sudo` drops the caller's environment, and a variable lost that way would
# silently send the install to the default directory - over a live site.
INSTALL_DIR="$DEFAULT_DIR"
FILES_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dir)
            [ -n "${2:-}" ] || usage
            INSTALL_DIR="${2%/}"
            FILES_ONLY=1
            shift 2 ;;
        *) usage ;;
    esac
done
if [ -n "${BR_INSTALL_DIR:-}${BR_FILES_ONLY:-}" ]; then
    echo "Warning: BR_INSTALL_DIR / BR_FILES_ONLY are ignored - use --dir <path>."
fi

# --dir: a directory of its own. The default directory belongs to
# boat-rudder.service (installing only its files would leave the service
# running the old binary); a system directory or one holding anything but a
# Boat Rudder install would get files mixed into it.
if [ "$FILES_ONLY" = "1" ]; then
    if ! [[ "$INSTALL_DIR" =~ ^(/[A-Za-z0-9._-]+)+$ ]]; then
        echo "Error: --dir '$INSTALL_DIR' must be an absolute path of a-z A-Z 0-9 . _ -"
        exit 1
    fi
    case "$INSTALL_DIR" in
        "$DEFAULT_DIR")
            echo "Error: $DEFAULT_DIR is boat-rudder.service's install - run install.sh without --dir."
            exit 1 ;;
        /bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/usr/bin|/usr/sbin|/usr/lib|/usr/share|/usr/local|/usr/local/bin|/usr/local/sbin|/usr/local/lib|/usr/local/src|/var|/var/lib|/var/log)
            echo "Error: --dir '$INSTALL_DIR' is a system directory - give the install a directory of its own."
            exit 1 ;;
    esac
    if [ -d "$INSTALL_DIR" ] && [ -n "$(ls -A "$INSTALL_DIR")" ] && [ ! -d "$INSTALL_DIR/.boat-rudder" ]; then
        echo "Error: $INSTALL_DIR is not empty and holds no Boat Rudder install - refusing to install into it."
        exit 1
    fi
fi
SERVICE_NAME="boat-rudder"
SERVICE_FILE="$SCRIPT_DIR/boat-rudder.service"
SYSTEMD_DIR="/etc/systemd/system"

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

# The server runs scripts/image-optimizer.sh (relative to its
# WorkingDirectory) on every media upload; without it every upload is
# rejected with "415 - could not be processed as an image". Its tools are
# checked too, since a missing one fails the same way.
mkdir -p "$INSTALL_DIR/scripts"
install -m 755 ./scripts/image-optimizer.sh "$INSTALL_DIR/scripts/image-optimizer.sh"
# manifest.sh checks an install against its manifest ("verify") on the host;
# boat-rudder-update updates it from signed releases, and is linked into
# /usr/local/sbin so `sudo boat-rudder-update ...` works from anywhere.
install -m 755 ./scripts/manifest.sh "$INSTALL_DIR/scripts/manifest.sh"
install -m 755 ./scripts/boat-rudder-update "$INSTALL_DIR/scripts/boat-rudder-update"
[ "$FILES_ONLY" = "1" ] || ln -sfn "$INSTALL_DIR/scripts/boat-rudder-update" /usr/local/sbin/boat-rudder-update
for tool in magick jpegoptim gifsicle; do
    command -v "$tool" > /dev/null 2>&1 ||
        echo "Warning: '$tool' not found - media uploads will fail until it is installed."
done

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

# html/ is required at runtime. Everything but html/content/ is merged over
# the installed copy: templates, themes and assets are updated, and files that
# only exist there (uploaded fonts...) are left alone. html/content/ is the
# site's own - posts, uploads, generated QR codes - and the repository's copy
# only sample content, so it is seeded on the first install only: copied over
# a live site it would replace same-named files of the site with the samples.
if [ -d ./html ]; then
    mkdir -p "$INSTALL_DIR/html"
    for entry in ./html/* ./html/.[!.]*; do
        [ -e "$entry" ] || continue
        [ "$(basename "$entry")" = "content" ] && continue
        if [ -d "$entry" ]; then
            cp -rT "$entry" "$INSTALL_DIR/html/$(basename "$entry")"
        else
            cp "$entry" "$INSTALL_DIR/html/"
        fi
    done
    if [ -d ./html/content ] && [ ! -d "$INSTALL_DIR/html/content" ]; then
        cp -rT ./html/content "$INSTALL_DIR/html/content"
        echo "Seeded html/content/ with the sample content."
    fi
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

# .boat-rudder/ - what this install is (develop_docs/plans/update-system-plan.md):
# its version, and the manifest of the system files just installed - the list
# the updater compares against, and the only files it will ever replace or
# delete. Written once every file is in place.
mkdir -p "$INSTALL_DIR/.boat-rudder"
install -m 644 ./VERSION "$INSTALL_DIR/.boat-rudder/VERSION"
./scripts/manifest.sh create . > "$INSTALL_DIR/.boat-rudder/manifest.sha256.tmp"
chmod 644 "$INSTALL_DIR/.boat-rudder/manifest.sha256.tmp"
mv "$INSTALL_DIR/.boat-rudder/manifest.sha256.tmp" "$INSTALL_DIR/.boat-rudder/manifest.sha256"
if [ -f ./scripts/minisign.pub ]; then
    install -m 644 ./scripts/minisign.pub "$INSTALL_DIR/.boat-rudder/minisign.pub"
fi
echo "Installed version $(head -n1 ./VERSION) ($(wc -l < "$INSTALL_DIR/.boat-rudder/manifest.sha256") system files)."

if [ "$FILES_ONLY" = "1" ]; then
    source ./scripts/show/divbar
    echo "Files installed in $INSTALL_DIR (--dir: no service registered)."
    echo ""
    exit 0
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
