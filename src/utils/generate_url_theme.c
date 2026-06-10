#include "generate_url_theme.h"
#include "config_loader.h"
#include <stdio.h>
#include <stdlib.h>

char *generate_url_theme(const char *subpath_fmt, int epoch) {
    char subpath[256];
    int n = snprintf(subpath, sizeof(subpath), subpath_fmt, epoch);
    if (n < 0 || (size_t)n >= sizeof(subpath)) return NULL;

    int total = snprintf(NULL, 0, "./html/themes/%s/%s", theme, subpath);
    if (total < 0) return NULL;

    char *result = malloc((size_t)total + 1);
    if (!result) return NULL;

    snprintf(result, (size_t)total + 1, "./html/themes/%s/%s", theme, subpath);
    return result;
}
