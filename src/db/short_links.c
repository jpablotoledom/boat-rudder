#include "short_links.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define SHORT_LINKS_COLLECTION "short_links"
#define SHORT_CODE_LEN 7
// Base62 keeps every code URL-safe with no escaping, and 62^7 (~3.5e12)
// combinations makes a random collision astronomically unlikely for the
// number of galleries/entries this project will ever hold - the retry loop
// below exists only as a backstop, not because collisions are expected.
static const char CODE_ALPHABET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void random_code(char out[SHORT_CODE_LEN + 1]) {
    for (int i = 0; i < SHORT_CODE_LEN; i++)
        out[i] = CODE_ALPHABET[randombytes_uniform(sizeof(CODE_ALPHABET) - 1)];
    out[SHORT_CODE_LEN] = '\0';
}

static char *find_code_for_path(mongoc_collection_t *collection, const char *target_path) {
    bson_t *query = BCON_NEW("target_path", BCON_UTF8(target_path));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    return result;
}

char *short_link_get_or_create(const char *target_path) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SHORT_LINKS_COLLECTION);
    if (!collection) return NULL;

    char *existing = find_code_for_path(collection, target_path);
    if (existing) {
        mongoc_collection_destroy(collection);
        return existing;
    }

    char code[SHORT_CODE_LEN + 1];
    for (int attempt = 0; attempt < 5; attempt++) {
        random_code(code);

        bson_t *doc = BCON_NEW("_id", BCON_UTF8(code), "target_path", BCON_UTF8(target_path));
        bson_error_t error;
        bool ok = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);
        bson_destroy(doc);

        if (ok) {
            mongoc_collection_destroy(collection);
            return strdup(code);
        }
        if (error.code != 11000) { // not a duplicate-key error - give up
            LOG_ERROR("short_links: insert failed: %s", error.message);
            break;
        }
    }

    mongoc_collection_destroy(collection);
    return NULL;
}

char *short_link_resolve(const char *code) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SHORT_LINKS_COLLECTION);
    if (!collection) return NULL;

    bson_t *query = BCON_NEW("_id", BCON_UTF8(code));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "target_path") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result;
}
