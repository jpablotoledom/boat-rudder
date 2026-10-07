#include "generate_url_theme.h"
#include "request_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// snprintf(fmt, dir_arg, subpath) into a malloc'd, exactly-sized buffer.
// `fmt` takes exactly two "%s" - a directory piece, then subpath.
static char *build_path(const char *fmt, const char *dir_arg, const char *subpath) {
    int total = snprintf(NULL, 0, fmt, dir_arg, subpath);
    if (total < 0) return NULL;

    char *result = malloc((size_t)total + 1);
    if (!result) return NULL;

    snprintf(result, (size_t)total + 1, fmt, dir_arg, subpath);
    return result;
}

char *generate_url_theme(const char *subpath_fmt, int epoch) {
    char subpath[256];
    int n = snprintf(subpath, sizeof(subpath), subpath_fmt, epoch);
    if (n < 0 || (size_t)n >= sizeof(subpath)) return NULL;

    char *theme_path = build_path("./html/themes/%s/%s", request_theme(), subpath);
    if (theme_path && access(theme_path, F_OK) == 0) return theme_path;
    free(theme_path);

    // Not overridden by the active theme - shared, theme-agnostic template.
    // No existence check here: a genuinely missing template already means
    // read_file_to_string() returns NULL and every caller already treats
    // that as "template missing" (see e.g. mainbanner()/menu()'s NULL handling).
    int total = snprintf(NULL, 0, "./html/templates/%s", subpath);
    if (total < 0) return NULL;

    char *result = malloc((size_t)total + 1);
    if (!result) return NULL;

    snprintf(result, (size_t)total + 1, "./html/templates/%s", subpath);
    return result;
}
