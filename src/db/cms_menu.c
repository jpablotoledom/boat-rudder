#include "cms_menu.h"
#include "bson_lang.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>

void cms_get_menu_items(const char *lang, CmsMenuItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return;

    bson_t *query = BCON_NEW("enabled", BCON_BOOL(true));
    bson_t *opts = BCON_NEW(
        "sort", "{", "order", BCON_INT32(1), "}",
        "limit", BCON_INT64((int64_t)MENU_ITEM_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsMenuItem *items = calloc(MENU_ITEM_LIMIT, sizeof(CmsMenuItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    const char *resolved_lang = iso_lang(lang);

    size_t count = 0;
    const bson_t *doc;
    while (count < MENU_ITEM_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        items[count].link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].name = resolve_lang_map(doc, "name", resolved_lang);

        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_menu_items: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_menu_free(CmsMenuItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].link);
        free(items[i].name);
    }

    free(items);
}
