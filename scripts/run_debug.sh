#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

BINARY="./bin/base-http-server"
HTML_DIR="./html"
CONFIG="./configs/settings.conf"

source ./scripts/show/divbar
echo "Starting base-http-server in debug mode..."
source ./scripts/show/divbar

# Verify binary exists
if [ ! -f "$BINARY" ]; then
    echo "Error: binary not found at $BINARY"
    echo "Run './bhs.sh compiledebug' first."
    exit 1
fi

# Verify html/ exists
if [ ! -d "$HTML_DIR" ]; then
    echo "Warning: $HTML_DIR not found - creating an empty one."
    mkdir -p "$HTML_DIR"
fi

# Sync configs and ssl from source to bin/ so changes take effect without recompiling
cp -f ./configs/settings.conf ./bin/configs/settings.conf 2>/dev/null || true
if [ -d ./ssl ] && [ -n "$(ls -A ./ssl 2>/dev/null)" ]; then
    mkdir -p ./bin/ssl
    cp -f ./ssl/*.pem ./bin/ssl/ 2>/dev/null || true
fi

# Kill any existing instance
if pkill -0 -f "bin/base-http-server" 2>/dev/null; then
    echo "Stopping existing instance..."
    pkill -TERM -f "bin/base-http-server" 2>/dev/null || true
    sleep 1
fi

# Detect if privileged ports are in use (< 1024 requires root on Linux)
HTTP_PORT=$(grep -E "^http_port=" "$CONFIG" 2>/dev/null | cut -d= -f2 | tr -d ' \r')
HTTPS_PORT=$(grep -E "^https_port=" "$CONFIG" 2>/dev/null | cut -d= -f2 | tr -d ' \r')
SSL_ENABLED=$(grep -E "^ssl_enabled=" "$CONFIG" 2>/dev/null | cut -d= -f2 | tr -d ' \r')

NEEDS_ROOT=0
if [ "$HTTP_PORT" -lt 1024 ] 2>/dev/null; then NEEDS_ROOT=1; fi
if [ "$SSL_ENABLED" = "1" ] && [ "$HTTPS_PORT" -lt 1024 ] 2>/dev/null; then NEEDS_ROOT=1; fi

if [ "$NEEDS_ROOT" = "1" ] && [ "$(id -u)" -ne 0 ] && [ "$(uname -s)" = "Linux" ]; then
    echo "Note: ports $HTTP_PORT/$HTTPS_PORT require root on Linux."
    echo "Restarting with sudo..."
    source ./scripts/show/divbar
    exec sudo "$0" "$@"
fi

echo "HTTP port : ${HTTP_PORT:-?}"
if [ "$SSL_ENABLED" = "1" ]; then
    echo "HTTPS port: ${HTTPS_PORT:-?}"
fi
echo "Config    : $CONFIG"
echo "Serving   : $HTML_DIR"
source ./scripts/show/divbar

OS="$(uname -s)"

if [ "$OS" = "Linux" ] && command -v gdb &>/dev/null; then
    echo "Debugger: GDB"
    ASAN_OPTIONS=check_printf=0 \
        gdb -q \
            -ex "handle SIGPIPE nostop noprint pass" \
            -ex run \
            --args "$BINARY" -c "$CONFIG" "$HTML_DIR"

elif [ "$OS" = "Darwin" ] && command -v lldb &>/dev/null; then
    echo "Debugger: LLDB"
    ASAN_OPTIONS=check_printf=0 \
        lldb -- "$BINARY" -c "$CONFIG" "$HTML_DIR"

else
    echo "No debugger found - running directly (ASan still active)."
    ASAN_OPTIONS=check_printf=0 "$BINARY" -c "$CONFIG" "$HTML_DIR"
fi
