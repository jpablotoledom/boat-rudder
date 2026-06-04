#!/bin/bash
# bhs.sh - base-http-server main script
#
# Usage: ./bhs.sh <action1> [action2] ...
#
# Actions:
#   compiledebug   Compile with debug symbols and AddressSanitizer
#   compileprod    Compile optimized for production
#   rundebug       Run locally in debug mode (GDB on Linux, LLDB on macOS)
#   createcert     Generate a self-signed TLS certificate for local development
#   install        Compile and install as a systemd service (Linux only)
#   uninstall      Stop and remove the systemd service (Linux only)
#
# Actions can be combined:
#   ./bhs.sh compiledebug rundebug
#   ./bhs.sh createcert compiledebug rundebug
#   ./bhs.sh compileprod install

# Resolve script location so it works from any directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -lt 1 ]; then
    echo ""
    echo "  Usage: $0 <action1> [action2] ..."
    echo ""
    echo "  Actions:"
    echo "    compiledebug   Compile with debug symbols and AddressSanitizer"
    echo "    compileprod    Compile optimized for production"
    echo "    rundebug       Run locally in debug mode"
    echo "    createcert     Generate a self-signed TLS certificate (local dev)"
    echo "    install        Install as a systemd service  (Linux only, requires sudo)"
    echo "    uninstall      Remove the systemd service    (Linux only, requires sudo)"
    echo ""
    echo "  Examples:"
    echo "    $0 compiledebug rundebug"
    echo "    $0 createcert compiledebug rundebug"
    echo "    $0 compileprod install"
    echo ""
    exit 1
fi

for action in "$@"; do
    case "$action" in
        compiledebug)
            echo "Compiling (debug)..."
            ./scripts/compile_debug.sh
            ;;
        compileprod)
            echo "Compiling (production)..."
            ./scripts/compile_prod.sh
            ;;
        rundebug)
            echo "Running in debug mode..."
            ./scripts/run_debug.sh
            ;;
        createcert)
            echo "Generating local TLS certificate..."
            ./scripts/create_local_cert.sh
            ;;
        install)
            echo "Installing..."
            sudo ./scripts/install.sh
            ;;
        uninstall)
            echo "Uninstalling..."
            sudo ./scripts/uninstall.sh
            ;;
        *)
            echo ""
            echo "  Unknown action: '$action'"
            echo "  Valid actions: compiledebug, compileprod, rundebug, createcert, install, uninstall"
            echo ""
            exit 1
            ;;
    esac
done
