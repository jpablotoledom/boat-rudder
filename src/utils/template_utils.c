#include "template_utils.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *str_replace_first(const char *src, const char *needle, const char *replacement) {
    const char *pos = strstr(src, needle);
    if (!pos) return strdup(src);

    size_t prefix_len      = (size_t)(pos - src);
    size_t needle_len      = strlen(needle);
    size_t replacement_len = strlen(replacement);
    size_t suffix_len      = strlen(pos + needle_len);

    char *result = malloc(prefix_len + replacement_len + suffix_len + 1);
    if (!result) return NULL;

    memcpy(result, src, prefix_len);
    memcpy(result + prefix_len, replacement, replacement_len);
    memcpy(result + prefix_len + replacement_len, pos + needle_len, suffix_len + 1);

    return result;
}

char *render_template(const char *tpl, ...) {
    va_list args1, args2;
    va_start(args1, tpl);
    va_copy(args2, args1);

    int len = vsnprintf(NULL, 0, tpl, args1);
    va_end(args1);
    if (len < 0) {
        va_end(args2);
        return NULL;
    }

    char *result = malloc((size_t)len + 1);
    if (!result) {
        va_end(args2);
        return NULL;
    }

    vsnprintf(result, (size_t)len + 1, tpl, args2);
    va_end(args2);

    return result;
}

char *str_append(char *dst, const char *src) {
    size_t dst_len = dst ? strlen(dst) : 0;
    size_t src_len = strlen(src);

    char *result = realloc(dst, dst_len + src_len + 1);
    if (!result) {
        free(dst);
        return NULL;
    }

    memcpy(result + dst_len, src, src_len + 1);
    return result;
}

char *build_title_tag(const char *page_title) {
    return render_template("<title>%s</title>", page_title);
}
