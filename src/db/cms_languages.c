#include "cms_languages.h"
#include "bson_lang.h"
#include "language_catalog.h"
#include "mongodb_manager.h"
#include "../utils/config_loader.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

static void parse_language(const bson_t *doc, CmsLanguageItem *out) {
    bson_iter_t iter;

    out->code = (bson_iter_init_find(&iter, doc, "code") && BSON_ITER_HOLDS_UTF8(&iter))
        ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
    out->name = (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
        ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
    out->is_default = (bson_iter_init_find(&iter, doc, "is_default") && BSON_ITER_HOLDS_BOOL(&iter))
        ? bson_iter_bool(&iter) : 0;
}

void cms_get_languages(CmsLanguageItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW("sort", "{", "code", BCON_INT32(1), "}");

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsLanguageItem *items = calloc(LANGUAGE_CATALOG_COUNT, sizeof(CmsLanguageItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < LANGUAGE_CATALOG_COUNT && mongoc_cursor_next(cursor, &doc)) {
        parse_language(doc, &items[count]);
        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_languages: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_languages_free(CmsLanguageItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].code);
        free(items[i].name);
    }

    free(items);
}

void cms_languages_ensure_seeded(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (!collection) return;

    bson_t *empty = bson_new();
    bson_error_t error;
    int64_t count = mongoc_collection_count_documents(collection, empty, NULL, NULL, NULL, &error);
    bson_destroy(empty);

    if (count < 0) {
        LOG_ERROR("cms_languages_ensure_seeded: count failed: %s", error.message);
    } else if (count == 0) {
        bson_t *doc = BCON_NEW(
            "code", BCON_UTF8("en"),
            "name", BCON_UTF8("English"),
            "is_default", BCON_BOOL(true)
        );
        if (!mongoc_collection_insert_one(collection, doc, NULL, NULL, &error))
            LOG_ERROR("cms_languages_ensure_seeded: insert failed: %s", error.message);
        bson_destroy(doc);
    }

    mongoc_collection_destroy(collection);
}

void cms_resolve_default_lang(char *out, size_t out_size) {
    if (out_size == 0) return;

    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (collection) {
        bson_t *query = BCON_NEW("is_default", BCON_BOOL(true));
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

        const bson_t *doc;
        bson_iter_t iter;
        if (mongoc_cursor_next(cursor, &doc) &&
            bson_iter_init_find(&iter, doc, "code") && BSON_ITER_HOLDS_UTF8(&iter)) {
            strncpy(out, bson_iter_utf8(&iter, NULL), out_size - 1);
            out[out_size - 1] = '\0';

            bson_destroy(query);
            mongoc_cursor_destroy(cursor);
            mongoc_collection_destroy(collection);
            return;
        }

        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    strncpy(out, iso_lang(lang), out_size - 1);
    out[out_size - 1] = '\0';
}

int cms_add_language(const char *code) {
    const char *name = language_catalog_name(code);
    if (!name) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (!collection) return -1;

    bson_error_t error;
    bson_t *empty = bson_new();
    int64_t total = mongoc_collection_count_documents(collection, empty, NULL, NULL, NULL, &error);
    bson_destroy(empty);
    if (total < 0) {
        LOG_ERROR("cms_add_language: count failed: %s", error.message);
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *existing_query = BCON_NEW("code", BCON_UTF8(code));
    int64_t existing = mongoc_collection_count_documents(collection, existing_query, NULL, NULL, NULL, &error);
    bson_destroy(existing_query);
    if (existing != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *doc = BCON_NEW(
        "code", BCON_UTF8(code),
        "name", BCON_UTF8(name),
        "is_default", BCON_BOOL(total == 0)
    );
    bool ok = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_add_language: insert failed: %s", error.message);
    bson_destroy(doc);

    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_set_default_language(const char *code) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (!collection) return -1;

    bson_error_t error;
    bson_t *target_query = BCON_NEW("code", BCON_UTF8(code));
    int64_t target_count = mongoc_collection_count_documents(collection, target_query, NULL, NULL, NULL, &error);
    if (target_count != 1) {
        bson_destroy(target_query);
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *clear_query  = bson_new();
    bson_t *clear_update = BCON_NEW("$set", "{", "is_default", BCON_BOOL(false), "}");
    bool ok = mongoc_collection_update_many(collection, clear_query, clear_update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_set_default_language: clear failed: %s", error.message);
    bson_destroy(clear_query);
    bson_destroy(clear_update);

    if (ok) {
        bson_t *set_update = BCON_NEW("$set", "{", "is_default", BCON_BOOL(true), "}");
        ok = mongoc_collection_update_one(collection, target_query, set_update, NULL, NULL, &error);
        if (!ok) LOG_ERROR("cms_set_default_language: set failed: %s", error.message);
        bson_destroy(set_update);
    }

    bson_destroy(target_query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_remove_language(const char *code) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(LANGUAGES_COLLECTION);
    if (!collection) return -1;

    bson_error_t error;
    bson_t *empty = bson_new();
    int64_t total = mongoc_collection_count_documents(collection, empty, NULL, NULL, NULL, &error);
    bson_destroy(empty);
    if (total < 0) {
        LOG_ERROR("cms_remove_language: count failed: %s", error.message);
        mongoc_collection_destroy(collection);
        return -1;
    }
    if (total <= 1) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *query = BCON_NEW("code", BCON_UTF8(code));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    const bson_t *doc;
    bson_iter_t iter;
    int is_default = 0;
    int found = 0;
    if (mongoc_cursor_next(cursor, &doc)) {
        found = 1;
        is_default = (bson_iter_init_find(&iter, doc, "is_default") && BSON_ITER_HOLDS_BOOL(&iter))
            ? bson_iter_bool(&iter) : 0;
    }
    mongoc_cursor_destroy(cursor);

    if (!found || is_default) {
        bson_destroy(query);
        mongoc_collection_destroy(collection);
        return -1;
    }

    bool ok = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_remove_language: delete failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}
