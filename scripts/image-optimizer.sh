#!/bin/bash

# Usage: image-optimizer.sh input_folder output_folder [input_file]
#
# Generates these variants from each image:
#   _full  — original resolution, optimized
#   _half  — max 1024px,  JPEG or GIF (same format as source)
#   _small — max  300px,  JPEG or GIF (same format as source)
#   _medium— max  600px,  always GIF  (epoch1/2 browser compatibility)
#   _micro — max  180px,  always GIF  (epoch1 Mosaic 1.0 compatibility)

INPUT_DIR="$1"
OUTPUT_DIR="$2"
INPUT_FILE="$3"

if [ -z "$INPUT_DIR" ] || [ -z "$OUTPUT_DIR" ]; then
    echo "Usage: $0 input_folder output_folder [input_file]"
    exit 1
fi

if [ ! -d "$INPUT_DIR" ]; then
    echo "Input folder '$INPUT_DIR' does not exist."
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Returns true if the image has 256 colors or fewer (GIF-safe)
is_suitable_for_gif() {
    NUM_COLORS=$(magick "$1" -auto-orient -format "%k" info:)
    [ "$NUM_COLORS" -le 256 ]
}

# Resize keeping aspect ratio — only shrinks, never enlarges
resize_image() {
    local src="$1" dst="$2" max="$3"
    magick "$src" -auto-orient -resize "${max}x${max}>" "$dst"
}

# Resize and convert to GIF with dithering (for old-browser compatibility)
resize_to_gif() {
    local src="$1" dst="$2" max="$3"
    magick "$src" -auto-orient -resize "${max}x${max}>" -colors 256 -dither Riemersma "$dst"
    gifsicle -O3 "$dst" -o "$dst"
}

process_image() {
    local INPUT_FILE="$1"
    local FILENAME BASENAME

    FILENAME=$(basename "$INPUT_FILE")
    BASENAME="${FILENAME%.*}"

    MIME_TYPE=$(file --mime-type -b "$INPUT_FILE")
    case "$MIME_TYPE" in
        image/*) ;;
        *) echo "Skipping '$FILENAME' (not an image)"; return ;;
    esac

    local FULL_FILE="$OUTPUT_DIR/${BASENAME}_full"
    local HALF_FILE="$OUTPUT_DIR/${BASENAME}_half"
    local SMALL_FILE="$OUTPUT_DIR/${BASENAME}_small"
    local MEDIUM_FILE="$OUTPUT_DIR/${BASENAME}_medium"
    local MICRO_FILE="$OUTPUT_DIR/${BASENAME}_micro"

    if is_suitable_for_gif "$INPUT_FILE"; then
        # Source has <= 256 colors: generate all variants as GIF
        magick "$INPUT_FILE" -auto-orient -colors 256 "$FULL_FILE.gif"
        gifsicle -O3 "$FULL_FILE.gif" -o "$FULL_FILE.gif"

        resize_image "$FULL_FILE.gif" "$HALF_FILE.gif" 1024
        gifsicle -O3 "$HALF_FILE.gif" -o "$HALF_FILE.gif"

        resize_image "$FULL_FILE.gif" "$SMALL_FILE.gif" 300
        gifsicle -O3 "$SMALL_FILE.gif" -o "$SMALL_FILE.gif"

        resize_image "$FULL_FILE.gif" "$MEDIUM_FILE.gif" 600
        gifsicle -O3 "$MEDIUM_FILE.gif" -o "$MEDIUM_FILE.gif"

        resize_image "$FULL_FILE.gif" "$MICRO_FILE.gif" 180
        gifsicle -O3 "$MICRO_FILE.gif" -o "$MICRO_FILE.gif"

        FINAL_FILE="${FULL_FILE}.gif"
    else
        # Source is a photo: _full/_half/_small as JPEG, _medium/_micro as GIF
        magick "$INPUT_FILE" -auto-orient -quality 80 "$FULL_FILE.jpg"
        jpegoptim --strip-all --max=80 "$FULL_FILE.jpg" > /dev/null

        resize_image "$FULL_FILE.jpg" "$HALF_FILE.jpg" 1024
        jpegoptim --strip-all --max=80 "$HALF_FILE.jpg" > /dev/null

        resize_image "$FULL_FILE.jpg" "$SMALL_FILE.jpg" 300
        jpegoptim --strip-all --max=80 "$SMALL_FILE.jpg" > /dev/null

        resize_to_gif "$INPUT_FILE" "$MEDIUM_FILE.gif" 600
        resize_to_gif "$INPUT_FILE" "$MICRO_FILE.gif" 180

        FINAL_FILE="${FULL_FILE}.jpg"
    fi

    rm -f "$INPUT_FILE"

    # Create a base-name symlink pointing to _half for direct URL access
    local BASE_LINK="$OUTPUT_DIR/${FILENAME}"
    local HALF_BASENAME
    if [ -f "$HALF_FILE.jpg" ]; then
        HALF_BASENAME=$(basename "$HALF_FILE.jpg")
    elif [ -f "$HALF_FILE.gif" ]; then
        HALF_BASENAME=$(basename "$HALF_FILE.gif")
    fi
    if [ -n "$HALF_BASENAME" ]; then
        ln -sf "$HALF_BASENAME" "$BASE_LINK"
    fi

    echo "$FINAL_FILE"
}

if [ -n "$INPUT_FILE" ]; then
    process_image "$INPUT_FILE"
else
    for FILE in "$INPUT_DIR"/*; do
        [ -f "$FILE" ] && process_image "$FILE"
    done
fi
