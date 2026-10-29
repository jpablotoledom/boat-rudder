#include "cms_extensions.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdio.h>

#define EXTENSIONS_COLLECTION "extensions"

int cms_extensions_enabled(char (*names)[33], size_t max) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(EXTENSIONS_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW("enabled", BCON_BOOL(true));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int count = 0;
    const bson_t *doc;
    while ((size_t)count < max && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            snprintf(names[count++], sizeof(names[0]), "%s", bson_iter_utf8(&iter, NULL));
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("cms_extensions_enabled: %s", error.message);
        count = -1;
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return count;
}

int cms_extensions_set_enabled(const char *name, int enabled) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(EXTENSIONS_COLLECTION);
    if (!collection) return -1;

    bson_t *selector = BCON_NEW("name", BCON_UTF8(name));
    bson_t *update = BCON_NEW("$set", "{", "name", BCON_UTF8(name),
                                           "enabled", BCON_BOOL(enabled != 0), "}");
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    int rc = mongoc_collection_update_one(collection, selector, update, opts, NULL, &error) ? 0 : -1;
    if (rc != 0) LOG_ERROR("cms_extensions_set_enabled: %s", error.message);

    bson_destroy(opts);
    bson_destroy(update);
    bson_destroy(selector);
    mongoc_collection_destroy(collection);
    return rc;
}
