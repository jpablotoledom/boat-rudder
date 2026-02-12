#!/bin/bash
source ./scripts/show/welcome

# Get user in session and say hello
user="$(whoami)" 
echo "Hi ${user}!!"

source ./scripts/show/divbar
echo '1- Cleaning directories...'
sleep .5

# Delete build and bin directories if exists
rm -rf ./build
rm -rf ./bin

source ./scripts/show/divbar
echo '2- Compiling...'
sleep .5

# Create a directory named 'build' in your project root
mkdir build

# Navigate to the 'build' directory
cd build

# Generate the Makefile using CMake with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Build the executable using make
cmake --build .

# Change directory
cd ..

# Create a directory named 'bin' for executables files
mkdir bin

source ./scripts/show/divbar
echo '3- Copying binaries, HTML content and configs...'
sleep .5

# Copy files
cp ./build/boat-rudder ./bin/boat-rudder

# Copy folders
cp -r ./ssl ./bin/ssl
cp -r ./configs ./bin/configs

source ./scripts/show/divbar
echo '4- Cleaning directories...'
sleep .5

# Delete build directory if exist
rm -rf ./build

source ./scripts/show/divbar
echo 'Done!!'
sleep .5
