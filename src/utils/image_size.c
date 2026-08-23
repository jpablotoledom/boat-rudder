#include "image_size.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int image_intrinsic_width(const char *url_path) {
    if (!url_path || url_path[0] != '/') return 0;
    // The path reaches here from the database. It is only ever opened for
    // reading, but a traversal would still read outside the site.
    if (strstr(url_path, "..")) return 0;

    char path[1024];
    if (snprintf(path, sizeof(path), "./html%s", url_path) >= (int)sizeof(path))
        return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    // GIF header: 6 bytes of magic, then the logical screen width as a
    // little-endian 16-bit value.
    unsigned char head[10];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);

    if (n < sizeof(head)) return 0;
    if (memcmp(head, "GIF87a", 6) != 0 && memcmp(head, "GIF89a", 6) != 0) return 0;

    return head[6] | (head[7] << 8);
}
