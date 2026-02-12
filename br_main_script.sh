#!/bin/bash

# Checks if at least one parameter was passed
if [ $# -lt 1 ]; then
    echo "Use: $0 <action1> [action2] [action3] ..."
    exit 1
fi

# Run all programs
for action in "$@"; do
    case "$action" in
        compiledebug)
            echo "Compile for local..."
            sudo ./scripts/compile_debug.sh
            ;;
        compileprod)
            echo "Compile for production..."
            sudo ./scripts/compile.sh
            ;;
        rundebug)
            echo "Run for local..."
            sudo ./scripts/run_development.sh
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
            echo "Unknown action: $action"
            echo "Valid actions: compiledebug, compileprod, rundebug, install, uninstall You can add more than one action, for example: ./br_main_script.sh compiledebug rundebug"
            ;;
    esac
done
