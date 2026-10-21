#!/bin/bash
# boat_rudder_builder.sh - Boat Rudder main script
#
# Usage: ./boat_rudder_builder.sh <action1> [action2] ...
#
# Actions:
#   compiledebug   Compile with debug symbols and AddressSanitizer
#   compileprod    Compile optimized for production
#   clean          Remove build/ and bin/ (forces a full recompile next time)
#   rundebug       Run locally in debug mode (GDB on Linux, LLDB on macOS)
#   createcert     Generate a self-signed TLS certificate for local development
#   install        Compile and install as a systemd service (Linux only)
#   uninstall      Stop and remove the systemd service (Linux only)
#
#   rundebug@<name>  Run with ./configs/<name>.conf instead of settings.conf
#
# Actions can be combined:
#   ./boat_rudder_builder.sh compiledebug rundebug
#   ./boat_rudder_builder.sh createcert compiledebug rundebug
#   ./boat_rudder_builder.sh compileprod install

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
    echo "    clean          Remove build/ and bin/ (forces a full recompile)"
    echo "    rundebug       Run locally in debug mode"
    echo "    createcert     Generate a self-signed TLS certificate (local dev)"
    echo "    install        Install as a systemd service  (Linux only, requires sudo)"
    echo "    uninstall      Remove the systemd service    (Linux only, requires sudo)"
    echo ""
    echo "    rundebug@<name>  Run with ./configs/<name>.conf"
    echo ""
    echo "  Examples:"
    echo "    $0 compiledebug rundebug"
    echo "    $0 createcert compiledebug rundebug"
    echo "    $0 compileprod install"
    echo ""
    exit 1
fi

for action in "$@"; do
    # "rundebug@<name>" - the config name goes to run_debug.sh.
    instance=""
    case "$action" in
        *@*) instance="${action#*@}"; action="${action%%@*}" ;;
    esac
    if [ -n "$instance" ] && [ "$action" != "rundebug" ]; then
        echo ""; echo "  '$action' takes no @<name>"; echo ""; exit 1
    fi
    case "$action" in
        compiledebug)
            echo "Compiling (debug)..."
            ./scripts/compile_debug.sh
            ;;
        compileprod)
            echo "Compiling (production)..."
            ./scripts/compile_prod.sh
            ;;
        clean)
            echo "Cleaning build artifacts..."
            ./scripts/clean.sh
            ;;
        rundebug)
            echo "Running in debug mode..."
            ./scripts/run_debug.sh ${instance:+"$instance"}
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
            echo "  Valid actions: compiledebug, compileprod, clean, rundebug, createcert, install, uninstall"
            echo "  (rundebug also takes @<name>: ./configs/<name>.conf)"
            echo ""
            exit 1
            ;;
    esac
done
