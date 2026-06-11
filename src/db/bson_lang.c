#include "bson_lang.h"
#include <string.h>
#include <strings.h>

const char *iso_lang(const char *configured_lang) {
    if (configured_lang && strcasecmp(configured_lang, "esp") == 0) return "es";
    return "en";
}

char *resolve_lang_map(const bson_t *parent, const char *field, const char *lang) {
    bson_iter_t iter, map_iter;

    if (!bson_iter_init_find(&iter, parent, field) || !BSON_ITER_HOLDS_DOCUMENT(&iter))
        return strdup("");

    uint32_t len;
    const uint8_t *data;
    bson_iter_document(&iter, &len, &data);

    bson_t map;
    bson_init_static(&map, data, len);

    if (bson_iter_init_find(&map_iter, &map, lang) && BSON_ITER_HOLDS_UTF8(&map_iter))
        return strdup(bson_iter_utf8(&map_iter, NULL));

    if (strcmp(lang, "en") != 0 &&
        bson_iter_init_find(&map_iter, &map, "en") && BSON_ITER_HOLDS_UTF8(&map_iter))
        return strdup(bson_iter_utf8(&map_iter, NULL));

    return strdup("");
}
