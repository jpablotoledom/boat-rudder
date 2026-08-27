#include "request_lang.h"
#include "../db/cms_languages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANG_CODE_BUF 16

// Per-thread, per-request. See request_lang.h for why this is not a parameter.
static __thread char current_lang[LANG_CODE_BUF] = "";
static __thread char current_path[512] = "";

// Reads the value of `name` from a raw Cookie header ("a=1; lang=es; b=2").
// Returns 1 and fills `out` on a match, 0 otherwise.
static int cookie_value(const char *cookie_header, const char *name,
                        char *out, size_t out_size) {
    if (!cookie_header || !name || out_size == 0) return 0;

    size_t name_len = strlen(name);
    const char *p = cookie_header;

    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        const char *eq = strchr(p, '=');
        if (!eq) break;

        // Match the whole name, so "xlang=" never satisfies a query for "lang".
        if ((size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            const char *val = eq + 1;
            const char *end = strchr(val, ';');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, val, len);
            out[len] = '\0';
            return 1;
        }

        const char *next = strchr(p, ';');
        if (!next) break;
        p = next + 1;
    }
    return 0;
}

int request_lang_validate(const char *code, char *out, size_t out_size) {
    if (!code || !code[0] || out_size == 0) return 0;

    CmsLanguageItem *langs = NULL;
    size_t count = 0;
    cms_get_languages(&langs, &count);

    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (langs[i].code && strcmp(langs[i].code, code) == 0) {
            strncpy(out, langs[i].code, out_size - 1);
            out[out_size - 1] = '\0';
            found = 1;
            break;
        }
    }

    cms_languages_free(langs, count);
    return found;
}

void request_lang_set(const char *cookie_header, const char *lang_query) {
    if (lang_query && lang_query[0] &&
        request_lang_validate(lang_query, current_lang, sizeof(current_lang))) {
        return;
    }

    char cookie_lang[LANG_CODE_BUF] = "";

    if (cookie_value(cookie_header, "lang", cookie_lang, sizeof(cookie_lang)) &&
        request_lang_validate(cookie_lang, current_lang, sizeof(current_lang))) {
        return;
    }

    // No cookie, or it names a language that is no longer configured.
    cms_resolve_default_lang(current_lang, sizeof(current_lang));
}

const char *request_lang(void) {
    if (!current_lang[0])
        cms_resolve_default_lang(current_lang, sizeof(current_lang));
    return current_lang;
}

void request_path_set(const char *path) {
    if (!path || path[0] != '/') {
        current_path[0] = '\0';
        return;
    }
    strncpy(current_path, path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
}

const char *request_path(void) {
    return current_path[0] ? current_path : "/";
}
