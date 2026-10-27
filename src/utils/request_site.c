#include "request_site.h"
#include "config_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Per-thread, per-request. See request_site.h for why this is not a parameter.
static __thread int current_hostless = 0;

void request_site_set(const char *host_header) {
    current_hostless = !host_header || !host_header[0];
}

const char *request_site(void) {
    if (!current_hostless || !request_site_key_is_valid(site_key)) return NULL;
    return site_key;
}

int request_site_key_is_valid(const char *key) {
    if (!key || !key[0]) return 0;
    size_t len = 0;
    for (const char *p = key; *p; p++, len++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || len >= 32) return 0;
    }
    return 1;
}

char *request_site_tag_location(const char *location) {
    const char *key = request_site();
    int site_relative = location[0] == '/' && location[1] != '/';
    if (!key || !site_relative || strstr(location, "site=")) return strdup(location);

    // Before any "#fragment", as build_epoch_response.c's link tagging does.
    const char *hash = strchr(location, '#');
    size_t base_len = hash ? (size_t)(hash - location) : strlen(location);
    const char *sep = memchr(location, '?', base_len) ? "&" : "?";

    size_t len = strlen(location) + strlen(sep) + 5 + strlen(key) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%.*s%ssite=%s%s", (int)base_len, location, sep, key, hash ? hash : "");
    return out;
}
