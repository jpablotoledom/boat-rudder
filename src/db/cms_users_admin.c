#include "cms_users_admin.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

static int is_valid_role(const char *role) {
    return strcmp(role, "admin") == 0 || strcmp(role, "author") == 0;
}

void cms_get_users(CmsUserAdminItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW(
        "sort", "{", "email", BCON_INT32(1), "}",
        "limit", BCON_INT64((int64_t)USERS_LIST_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsUserAdminItem *items = calloc(USERS_LIST_LIMIT, sizeof(CmsUserAdminItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < USERS_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        char id_str[25] = "";
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), id_str);

        items[count].id = strdup(id_str);
        items[count].email = (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("admin");

        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_users: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_users_admin_free(CmsUserAdminItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].id);
        free(items[i].email);
        free(items[i].role);
    }

    free(items);
}

int cms_get_user_values(const char *id_hex, char *out_email, size_t out_email_size,
                         char *out_role, size_t out_role_size) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        const char *email = (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "";
        strncpy(out_email, email, out_email_size - 1);
        out_email[out_email_size - 1] = '\0';

        const char *role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "admin";
        strncpy(out_role, role, out_role_size - 1);
        out_role[out_role_size - 1] = '\0';

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_user_values: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

int cms_get_user_role(const char *id_hex, char *out_role, size_t out_role_size) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        const char *role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "admin";
        strncpy(out_role, role, out_role_size - 1);
        out_role[out_role_size - 1] = '\0';

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_user_role: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

int cms_count_admins(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW(
        "$or", "[",
            "{", "role", BCON_UTF8("admin"), "}",
            "{", "role", "{", "$exists", BCON_BOOL(false), "}", "}",
        "]"
    );

    bson_error_t error;
    int64_t count = mongoc_collection_count_documents(collection, query, NULL, NULL, NULL, &error);
    if (count < 0) LOG_ERROR("cms_count_admins: count failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return (int)count;
}

int cms_create_user(const char *email, const char *password, const char *role) {
    if (!is_valid_role(role)) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_error_t error;
    bson_t *email_query = BCON_NEW("email", BCON_UTF8(email));
    int64_t existing = mongoc_collection_count_documents(collection, email_query, NULL, NULL, NULL, &error);
    bson_destroy(email_query);
    if (existing != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password, strlen(password),
                           crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *doc = BCON_NEW(
        "email", BCON_UTF8(email),
        "password", BCON_UTF8(hash),
        "role", BCON_UTF8(role)
    );
    bool ok = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_create_user: insert failed: %s", error.message);
    bson_destroy(doc);

    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_user(const char *id_hex, const char *email, const char *role,
                     const char *new_password) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;
    if (!is_valid_role(role)) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);

    bson_error_t error;
    bson_t *email_query = BCON_NEW(
        "email", BCON_UTF8(email),
        "_id", "{", "$ne", BCON_OID(&oid), "}"
    );
    int64_t existing = mongoc_collection_count_documents(collection, email_query, NULL, NULL, NULL, &error);
    bson_destroy(email_query);
    if (existing != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "email", -1, email, -1);
    bson_append_utf8(&set_doc, "role", -1, role, -1);

    if (new_password[0] != '\0') {
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(hash, new_password, strlen(new_password),
                               crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
            bson_append_document_end(&update, &set_doc);
            bson_destroy(&update);
            bson_destroy(query);
            mongoc_collection_destroy(collection);
            return -1;
        }
        bson_append_utf8(&set_doc, "password", -1, hash, -1);
    }

    bson_append_document_end(&update, &set_doc);

    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_user: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_delete_user(const char *id_hex) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t reply;
    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, &reply, &error);
    if (!ok) LOG_ERROR("cms_delete_user: delete failed: %s", error.message);

    int deleted = 0;
    if (ok) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT32(&iter))
            deleted = bson_iter_int32(&iter);
    }
    bson_destroy(&reply);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return (ok && deleted > 0) ? 0 : -1;
}

int cms_get_username_by_id(const char *id_hex, char *out, size_t out_size) {
    if (out_size == 0) return -1;
    out[0] = '\0';

    char email[256], role[32];
    if (cms_get_user_values(id_hex, email, sizeof(email), role, sizeof(role)) != 0)
        return -1;

    const char *at = strchr(email, '@');
    size_t len = at ? (size_t)(at - email) : strlen(email);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, email, len);
    out[len] = '\0';

    // Sanitize: keep only alphanumeric, dash, underscore, dot
    for (size_t i = 0; i < len; i++) {
        char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            out[i] = '-';
    }

    return 0;
}
