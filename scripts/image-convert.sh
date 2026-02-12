#!/bin/bash

# Color detection:

#     Uses ImageMagick to determine the number of colors in the input image.
#     If the image has 256 colors or less, it's suitable to be converted to GIF without significant quality loss.

# Optimization for GIF:

#     Converts the image to GIF with a maximum of 256 colors.
#     Uses gifsicle with the -O3 option to apply the maximum level of optimization.

# Optimization for JPEG:

#     If the image has more than 256 colors, it is converted to JPEG.
#     Uses ImageMagick to convert the image, setting the quality to 80%.
#     Then, jpegoptim optimizes the resulting file, stripping metadata and ensuring a maximum quality of 80%.

# Check that two arguments are provided
if [ $# -ne 2 ]; then
    echo "Usage: $0 input_folder output_folder"
    exit 1
fi

INPUT_DIR="$1"
OUTPUT_DIR="$2"

# Verify if the input folder exists
if [ ! -d "$INPUT_DIR" ]; then
    echo "The input folder '$INPUT_DIR' does not exist."
    exit 1
fi

# Create the output folder if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Function to determine if the image is suitable for GIF (less than 256 colors)
is_suitable_for_gif() {
    NUM_COLORS=$(convert "$1" -format "%k" info:)
    if [ "$NUM_COLORS" -le 256 ]; then
        return 0  # Suitable for GIF
    else
        return 1  # Not suitable for GIF
    fi
}

# Process each image file in the input folder
for INPUT_FILE in "$INPUT_DIR"/*; do
    if [ -f "$INPUT_FILE" ]; then
        FILENAME=$(basename "$INPUT_FILE")
        EXTENSION="${FILENAME##*.}"
        BASENAME="${FILENAME%.*}"

        # Determine if the file is a supported image
        MIME_TYPE=$(file --mime-type -b "$INPUT_FILE")
        case "$MIME_TYPE" in
            image/*)
                # It's an image, continue
                ;;
            *)
                # Not an image, skip
                echo "Skipping '$FILENAME' (not a supported image)"
                continue
                ;;
        esac

        # Path to the output file
        OUTPUT_FILE="$OUTPUT_DIR/$FILENAME"

        # If the image is suitable for GIF, convert to optimized GIF
        if is_suitable_for_gif "$INPUT_FILE"; then
            OUTPUT_FILE="$OUTPUT_DIR/$BASENAME.gif"
            echo "Converting '$FILENAME' to optimized GIF..."
            convert "$INPUT_FILE" -colors 256 "$OUTPUT_FILE"
            gifsicle -O3 "$OUTPUT_FILE" -o "$OUTPUT_FILE"
        else
            # Otherwise, convert to optimized JPEG
            OUTPUT_FILE="$OUTPUT_DIR/$BASENAME.jpg"
            echo "Converting '$FILENAME' to optimized JPEG..."
            convert "$INPUT_FILE" -quality 80 "$OUTPUT_FILE"
            jpegoptim --strip-all --max=80 "$OUTPUT_FILE" > /dev/null
        fi

        echo "Optimized image saved as '$(basename "$OUTPUT_FILE")'."
    fi
done

echo "Process completed. Optimized images are located in '$OUTPUT_DIR'."

exit 0
