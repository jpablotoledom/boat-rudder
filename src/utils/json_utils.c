#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *json_escape_alloc(const char *src) {
    size_t len = strlen(src);

    // Worst case: every byte becomes a 6-char "\u00XX" escape.
    char *out = malloc(len * 6 + 1);
    if (!out) return NULL;

    char *dst = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  memcpy(dst, "\\\"", 2); dst += 2; break;
            case '\\': memcpy(dst, "\\\\", 2); dst += 2; break;
            case '\n': memcpy(dst, "\\n", 2);  dst += 2; break;
            case '\r': memcpy(dst, "\\r", 2);  dst += 2; break;
            case '\t': memcpy(dst, "\\t", 2);  dst += 2; break;
            default:
                if (c < 0x20) {
                    dst += sprintf(dst, "\\u%04x", c);
                } else {
                    *dst++ = (char)c;
                }
                break;
        }
    }
    *dst = '\0';

    return out;
}
