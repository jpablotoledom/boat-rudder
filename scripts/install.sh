#!/bin/bash
sudo ./scripts/compile.sh

mkdir -p /usr/local/bin/boat-rudder
mkdir -p /usr/local/bin/boat-rudder/html
sudo cp -r ./bin/* /usr/local/bin/boat-rudder/
sudo cp -r ./html/* /usr/local/bin/boat-rudder/html
sudo cp -r ./scripts/boat-rudder.service /etc/systemd/system/

sudo systemctl daemon-reload
sudo systemctl enable boat-rudder.service
sudo systemctl start boat-rudder.service
