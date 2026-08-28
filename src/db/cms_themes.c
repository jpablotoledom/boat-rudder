#include "cms_themes.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <string.h>

#define THEMES_COLLECTION "themes"

// Epoch 3's own hardcoded styles_epoch3.css values - the reference palette
// every field defaults to, for every epoch alike (see cms_themes.h: one
// shared set of colors, not one per epoch). `category` has no prior
// independent value (epoch 3 always shared it with `border` before this
// field existed), so it defaults equal to `border` - preserving epoch 3's
// exact current look until an admin deliberately splits them.
static const CmsThemeColors DEFAULT_COLORS = {
    .background = "#000000",
    .text       = "#ffffff",
    .accent     = "#56e9fd",
    .author     = "#f2e200",
    .date       = "#f9964f",
    .category   = "#349b00",
    .border     = "#349b00",
};

static void copy_field(const bson_t *doc, const char *field, char *out, size_t out_size) {
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, doc, field) && BSON_ITER_HOLDS_UTF8(&iter)) {
        strncpy(out, bson_iter_utf8(&iter, NULL), out_size - 1);
        out[out_size - 1] = '\0';
    }
}

int cms_get_theme_colors(const char *key, CmsThemeColors *out) {
    *out = DEFAULT_COLORS;
    if (!key || !key[0]) return 1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return 1;

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "colors") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            uint32_t len;
            const uint8_t *data;
            bson_iter_document(&iter, &len, &data);

            bson_t colors;
            bson_init_static(&colors, data, len);

            copy_field(&colors, "background", out->background, sizeof(out->background));
            copy_field(&colors, "text", out->text, sizeof(out->text));
            copy_field(&colors, "accent", out->accent, sizeof(out->accent));
            copy_field(&colors, "author", out->author, sizeof(out->author));
            copy_field(&colors, "date", out->date, sizeof(out->date));
            copy_field(&colors, "category", out->category, sizeof(out->category));
            copy_field(&colors, "border", out->border, sizeof(out->border));
        }
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_theme_colors: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return 1;
}

int cms_update_theme_colors(const char *key, const CmsThemeColors *colors) {
    if (!key || !key[0]) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    bson_t *update = BCON_NEW(
        "$set", "{",
            "key", BCON_UTF8(key),
            "colors", "{",
                "background", BCON_UTF8(colors->background),
                "text", BCON_UTF8(colors->text),
                "accent", BCON_UTF8(colors->accent),
                "author", BCON_UTF8(colors->author),
                "date", BCON_UTF8(colors->date),
                "category", BCON_UTF8(colors->category),
                "border", BCON_UTF8(colors->border),
            "}",
        "}"
    );
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_theme_colors: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}
