#ifndef MEMMEM_COMPAT_H
#define MEMMEM_COMPAT_H

#include <stddef.h>
#include <string.h>

#ifndef _GNU_SOURCE
static inline const void *portable_memmem(const void *haystack, size_t haystacklen,
                                          const void *needle, size_t needlelen) {
    if (needlelen == 0)
        return haystack;
    if (haystacklen < needlelen)
        return NULL;

    const unsigned char *h = haystack;
    const unsigned char *n = needle;

    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return h + i;
    }
    return NULL;
}
#define memmem portable_memmem
#endif

#endif
