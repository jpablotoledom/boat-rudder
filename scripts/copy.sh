#!/bin/bash

# delete
rm -rf ./bin/html

# Copy folders
# cp -r ./ssl ./bin/ssl
# cp -r ./configs ./bin/configs
cp -r ./src/html ./bin/html

# Run server, with or not SSL

# cd ./bin
# ./web-server ./html
# cd ..