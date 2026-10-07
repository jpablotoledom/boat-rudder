#include "image_convert.h"
#include "../wbmp_writer.h"
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../../third_party/stb_image.h"

// Nearest-neighbor resample from src (sw x sh, 1 byte/pixel grayscale) into
// a freshly malloc'd dw x dh buffer. No new header vendored just for a
// resize - logos are small and headed for a 1-bit format anyway, so a
// higher-fidelity resampler wouldn't be visible in the output.
static unsigned char *resample_gray(const unsigned char *src, int sw, int sh, int dw, int dh) {
    unsigned char *dst = malloc((size_t)dw * dh);
    if (!dst) return NULL;

    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
    return dst;
}

int image_convert_to_wbmp(const char *in_path, const char *out_path, int max_dim) {
    int w, h, channels;
    // desired_channels = 1: stb_image itself does the RGB/RGBA -> grayscale
    // luma conversion (0.299r+0.587g+0.114b, the same weights used
    // everywhere else in this codebase's image handling).
    unsigned char *gray = stbi_load(in_path, &w, &h, &channels, 1);
    if (!gray) return -1;

    int dw = w, dh = h;
    if (max_dim > 0 && (w > max_dim || h > max_dim)) {
        if (w >= h) {
            dw = max_dim;
            dh = (h * max_dim) / w;
        } else {
            dh = max_dim;
            dw = (w * max_dim) / h;
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    unsigned char *scaled = gray;
    int owns_scaled = 0;
    if (dw != w || dh != h) {
        scaled = resample_gray(gray, w, h, dw, dh);
        owns_scaled = 1;
        if (!scaled) {
            stbi_image_free(gray);
            return -1;
        }
    }

    // write_wbmp()'s convention: pixels[y*w+x] & 1, nonzero = dark. A flat
    // 50%-brightness threshold - below 128 renders as a dark (black) pixel.
    unsigned char *bits = malloc((size_t)dw * dh);
    int ret = -1;
    if (bits) {
        for (int i = 0; i < dw * dh; i++)
            bits[i] = (scaled[i] < 128) ? 1 : 0;
        ret = write_wbmp(out_path, bits, dw, dh);
        free(bits);
    }

    if (owns_scaled) free(scaled);
    stbi_image_free(gray);
    return ret;
}
