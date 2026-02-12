#!/bin/bash
sudo killall boat-rudder
sudo ./scripts/mongodb_start.sh

# Requires 'inotify-tools' to be installed.

# Directories
SRC_DIR="./src/html"
BUILD_DIR="./bin/html"

# Cache to track recently processed files
declare -A file_cache

# Expiry time for cache entries (in seconds)
CACHE_EXPIRY=2

# Check if inotifywait is installed
if ! command -v inotifywait &> /dev/null; then
    echo "inotifywait is not installed. Please install inotify-tools first."
    exit 1
fi

# Function to copy modified files
copy_modified_file() {
    local file_path="$1"
    local relative_path="${file_path#$SRC_DIR/}"
    local destination="$BUILD_DIR/$relative_path"
    local destination_dir="$(dirname "$destination")"

    # Create the destination directory if it doesn't exist
    mkdir -p "$destination_dir"
    
    # Copy the modified file
    cp "$file_path" "$destination"
    echo "File copied: $file_path -> $destination"
}

# Function to check if a file is recently processed
is_recently_processed() {
    local file_path="$1"
    local current_time=$(date +%s)

    # Check if the file is in the cache and if it was processed recently
    if [[ -n "${file_cache[$file_path]}" ]]; then
        local last_processed_time=${file_cache[$file_path]}
        if (( current_time - last_processed_time < CACHE_EXPIRY )); then
            return 0 # Recently processed
        fi
    fi

    # Update the cache with the current time
    file_cache[$file_path]=$current_time
    return 1 # Not recently processed
}

# Main function to run the server and monitor changes
main() {
    # Start the server in the background
    (sudo gdb -q -ex run --args ./bin/boat-rudder ./html)
    echo "Server started."

    # Monitor changes in the SRC_DIR directory
    # inotifywait -m -r -e modify,create,delete,move "$SRC_DIR" --format '%w%f' | while read file; do
    #     # Avoid duplicate events by checking the cache
    #     if ! is_recently_processed "$file"; then
    #         copy_modified_file "$file"
    #     fi
    # done
}

main
