#include "cms_site_settings.h"
#include "mongodb_manager.h"
#include "../utils/config_loader.h"
#include "../utils/theme_catalog.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SITE_SETTINGS_COLLECTION "site_settings"
#define SITE_NAME_DEFAULT "Boat Rudder"

// site_settings is a singleton: every write is an upsert against the empty
// filter {}, same shape as mongoc_collection_find_with_opts()'s read side
// below - the first save creates the document, every later save updates it.
static int upsert_set(bson_t *set_doc_owner /* consumed */) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) {
        bson_destroy(set_doc_owner);
        return -1;
    }

    bson_t *query = bson_new();
    bson_t *update = bson_new();
    BSON_APPEND_DOCUMENT(update, "$set", set_doc_owner);
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_site_settings: upsert failed: %s", error.message);

    bson_destroy(set_doc_owner);
    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

char *cms_get_site_name(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return strdup(SITE_NAME_DEFAULT);

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "site_name") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup(SITE_NAME_DEFAULT);
}

int cms_update_site_name(const char *name) {
    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "site_name", -1, name, -1);
    return upsert_set(set_doc);
}

char *cms_get_active_theme_key(void) {
    char *db_value = NULL;

    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            if (bson_iter_init_find(&iter, doc, "active_theme") && BSON_ITER_HOLDS_UTF8(&iter))
                db_value = strdup(bson_iter_utf8(&iter, NULL));
        }

        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (db_value && theme_key_is_valid(db_value)) return db_value;
    free(db_value);
    return strdup(theme); // configs/settings.conf fallback - see request_theme.h
}

int cms_set_active_theme(const char *key) {
    if (!theme_key_is_valid(key)) return -1;

    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "active_theme", -1, key, -1);
    return upsert_set(set_doc);
}

bool cms_get_hide_home_blog(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return false;

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    bool result = false;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "hide_home_blog") && BSON_ITER_HOLDS_BOOL(&iter))
            result = bson_iter_bool(&iter);
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result;
}

int cms_update_hide_home_blog(bool hide) {
    bson_t *set_doc = bson_new();
    bson_append_bool(set_doc, "hide_home_blog", -1, hide);
    return upsert_set(set_doc);
}

void cms_get_under_construction(bool *enabled, char **page_link) {
    *enabled = false;
    char *link = NULL;

    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            if (bson_iter_init_find(&iter, doc, "under_construction") && BSON_ITER_HOLDS_BOOL(&iter))
                *enabled = bson_iter_bool(&iter);
            if (page_link && bson_iter_init_find(&iter, doc, "under_construction_page") &&
                BSON_ITER_HOLDS_UTF8(&iter))
                link = strdup(bson_iter_utf8(&iter, NULL));
        }

        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (page_link) *page_link = link ? link : strdup("");
}

int cms_update_under_construction(bool enabled, const char *page_link) {
    bson_t *set_doc = bson_new();
    bson_append_bool(set_doc, "under_construction", -1, enabled);
    bson_append_utf8(set_doc, "under_construction_page", -1, page_link, -1);
    return upsert_set(set_doc);
}

// One bool field of the singleton; `fallback` if unset/unreachable.
static bool get_bool(const char *field, bool fallback) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return fallback;

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);
    bool result = fallback;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, field) && BSON_ITER_HOLDS_BOOL(&iter))
            result = bson_iter_bool(&iter);
    }
    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result;
}

bool cms_get_comments_enabled(void) {
    return get_bool("comments_enabled", false);
}

int cms_update_comments_enabled(bool enabled) {
    bson_t *set_doc = bson_new();
    bson_append_bool(set_doc, "comments_enabled", -1, enabled);
    return upsert_set(set_doc);
}

static pthread_mutex_t secret_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   secret_cache[SITE_SECRET_BYTES];
static int             secret_loaded;

// Reads site_settings.secret into `key`; 1 if present and well-formed.
static int read_secret(mongoc_collection_t *collection, unsigned char *key) {
    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);
    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        size_t len = 0;
        if (bson_iter_init_find(&iter, doc, "secret") && BSON_ITER_HOLDS_UTF8(&iter)) {
            const char *hex = bson_iter_utf8(&iter, NULL);
            found = sodium_hex2bin(key, SITE_SECRET_BYTES, hex, strlen(hex), NULL, &len, NULL) == 0 &&
                    len == SITE_SECRET_BYTES;
        }
    }
    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    return found;
}

int cms_get_site_secret(unsigned char key[SITE_SECRET_BYTES]) {
    pthread_mutex_lock(&secret_lock);
    if (secret_loaded) {
        memcpy(key, secret_cache, SITE_SECRET_BYTES);
        pthread_mutex_unlock(&secret_lock);
        return 0;
    }

    int rc = -1;
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        if (!read_secret(collection, secret_cache)) {
            // Create it only where there is none: make sure the singleton
            // exists, then set the field on it only if still missing - two
            // servers starting together both end up reading the same one.
            unsigned char fresh[SITE_SECRET_BYTES];
            char hex[SITE_SECRET_BYTES * 2 + 1];
            randombytes_buf(fresh, sizeof(fresh));
            sodium_bin2hex(hex, sizeof(hex), fresh, sizeof(fresh));

            bson_t *empty = bson_new();
            bson_t *on_insert = BCON_NEW("$setOnInsert", "{", "created_at", BCON_DATE_TIME((int64_t)time(NULL) * 1000), "}");
            bson_t *upsert = BCON_NEW("upsert", BCON_BOOL(true));
            bson_t *missing = BCON_NEW("secret", "{", "$exists", BCON_BOOL(false), "}");
            bson_t *set = BCON_NEW("$set", "{", "secret", BCON_UTF8(hex), "}");
            bson_error_t error;
            if (!mongoc_collection_update_one(collection, empty, on_insert, upsert, NULL, &error) ||
                !mongoc_collection_update_one(collection, missing, set, NULL, NULL, &error))
                LOG_ERROR("cms_get_site_secret: %s", error.message);
            bson_destroy(empty);
            bson_destroy(on_insert);
            bson_destroy(upsert);
            bson_destroy(missing);
            bson_destroy(set);
            sodium_memzero(fresh, sizeof(fresh));
            sodium_memzero(hex, sizeof(hex));
        }
        if (read_secret(collection, secret_cache)) {
            secret_loaded = 1;
            memcpy(key, secret_cache, SITE_SECRET_BYTES);
            rc = 0;
        }
        mongoc_collection_destroy(collection);
    }
    pthread_mutex_unlock(&secret_lock);
    return rc;
}
