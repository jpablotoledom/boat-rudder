#include "wbmp_writer.h"
#include <stdio.h>

// WBMP type 0: two header bytes, then width/height as multi-byte integers
// (7 bits per byte, high bit = "more follows"), then a 1bpp bitmap padded to
// whole bytes per row, where 1 = white and 0 = black.
int write_wbmp(const char *path, const unsigned char *pixels, int width, int height) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    fputc(0x00, fp);  /* type: B&W, no compression */
    fputc(0x00, fp);  /* fixed header */

    for (int dim = 0; dim < 2; dim++) {
        int v = dim ? height : width;
        unsigned char mb[5];
        int n = 0;
        do { mb[n++] = v & 0x7F; v >>= 7; } while (v > 0);
        while (n > 0) {
            n--;
            fputc(mb[n] | (n > 0 ? 0x80 : 0x00), fp);
        }
    }

    int row_bytes = (width + 7) / 8;
    for (int y = 0; y < height; y++) {
        for (int b = 0; b < row_bytes; b++) {
            unsigned char byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int x = b * 8 + bit;
                /* pad beyond the row with white so the quiet zone stays clean */
                int dark = (x < width) ? (pixels[y * width + x] & 1) : 0;
                if (!dark) byte |= (unsigned char)(0x80 >> bit);
            }
            fputc(byte, fp);
        }
    }

    int ok = (ferror(fp) == 0);
    fclose(fp);
    return ok ? 0 : -1;
}
