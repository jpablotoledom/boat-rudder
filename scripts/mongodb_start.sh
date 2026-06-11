#!/bin/bash

start_service() {
    if command -v systemctl >/dev/null 2>&1; then
        if systemctl list-unit-files | grep -q "^mongod.service"; then
            sudo systemctl start mongod
            return
        elif systemctl list-unit-files | grep -q "^mongodb.service"; then
            sudo systemctl start mongodb
            return
        fi
    fi

    # SysV init fallback
    if service mongod status >/dev/null 2>&1; then
        sudo service mongod start
    elif service mongodb status >/dev/null 2>&1; then
        sudo service mongodb start
    else
        echo "No se encontró servicio MongoDB"
        exit 1
    fi
}

start_service
