#define _XOPEN_SOURCE 700
#include "cms_media.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- helpers ----

static void copy_str(char *dst, size_t dst_size, const bson_iter_t *iter) {
    const char *val = bson_iter_utf8(iter, NULL);
    if (val) {
        strncpy(dst, val, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

// ---- directories ----

void cms_get_media_directories(CmsMediaDirectory **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!col) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW("sort", "{", "_id", BCON_INT32(1), "}",
                            "limit", BCON_INT64(MEDIA_LIST_LIMIT));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, query, opts, NULL);
    const bson_t *doc;

    size_t cap = 32;
    CmsMediaDirectory *list = calloc(cap, sizeof(*list));
    size_t count = 0;

    while (mongoc_cursor_next(cursor, &doc)) {
        if (count >= cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(*list));
        }
        CmsMediaDirectory *d = &list[count];
        memset(d, 0, sizeof(*d));

        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), d->id);
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(d->name, sizeof(d->name), &iter);
        if (bson_iter_init_find(&iter, doc, "parent") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(d->parent, sizeof(d->parent), &iter);
        if (bson_iter_init_find(&iter, doc, "author_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), d->author_id);

        count++;
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(query);
    mongoc_collection_destroy(col);

    *out = list;
    *out_count = count;
}

void cms_media_directories_free(CmsMediaDirectory *items, size_t count) {
    (void)count;
    free(items);
}

bool cms_get_media_directory_by_id(const char *id_hex, CmsMediaDirectory *out) {
    memset(out, 0, sizeof(*out));
    if (!id_hex || strlen(id_hex) != 24) return false;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!col) return false;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, query, NULL, NULL);
    const bson_t *doc;
    bool found = false;

    if (mongoc_cursor_next(cursor, &doc)) {
        found = true;
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), out->id);
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(out->name, sizeof(out->name), &iter);
        if (bson_iter_init_find(&iter, doc, "parent") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(out->parent, sizeof(out->parent), &iter);
        if (bson_iter_init_find(&iter, doc, "author_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), out->author_id);
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_collection_destroy(col);
    return found;
}

int cms_create_media_directory(const char *name, const char *parent,
                               const char *author_id, char *out_id) {
    if (!name || !author_id || !bson_oid_is_valid(author_id, strlen(author_id)))
        return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!col) return -1;

    bson_oid_t oid, author_oid;
    bson_oid_init(&oid, NULL);
    bson_oid_init_from_string(&author_oid, author_id);

    bson_t *doc = bson_new();
    BSON_APPEND_OID(doc, "_id", &oid);
    BSON_APPEND_UTF8(doc, "name", name);
    BSON_APPEND_UTF8(doc, "parent", parent ? parent : "posts");
    BSON_APPEND_OID(doc, "author_id", &author_oid);

    bson_error_t error;
    int ret = mongoc_collection_insert_one(col, doc, NULL, NULL, &error) ? 0 : -1;
    if (ret != 0) LOG_ERROR("cms_create_media_directory: %s", error.message);

    if (ret == 0) bson_oid_to_string(&oid, out_id);

    bson_destroy(doc);
    mongoc_collection_destroy(col);
    return ret;
}

int cms_rename_media_directory(const char *id_hex, const char *new_name) {
    if (!id_hex || strlen(id_hex) != 24 || !new_name) return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!col) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);

    bson_t *selector = BCON_NEW("_id", BCON_OID(&oid));
    bson_t *update = BCON_NEW("$set", "{", "name", BCON_UTF8(new_name), "}");

    bson_error_t error;
    int ret = mongoc_collection_update_one(col, selector, update, NULL, NULL, &error) ? 0 : -1;
    if (ret != 0) LOG_ERROR("cms_rename_media_directory: %s", error.message);

    bson_destroy(selector);
    bson_destroy(update);
    mongoc_collection_destroy(col);
    return ret;
}

int cms_delete_media_directory(const char *id_hex) {
    if (!id_hex || strlen(id_hex) != 24) return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!col) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *selector = BCON_NEW("_id", BCON_OID(&oid));

    bson_error_t error;
    int ret = mongoc_collection_delete_one(col, selector, NULL, NULL, &error) ? 0 : -1;
    if (ret != 0) LOG_ERROR("cms_delete_media_directory: %s", error.message);

    bson_destroy(selector);
    mongoc_collection_destroy(col);
    return ret;
}

// ---- media items ----

void cms_get_media_items(const char *dir_id, int skip, int limit,
                         CmsMediaItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_COLLECTION);
    if (!col) return;

    bson_t *pipeline = bson_new();
    int stage = 0;
    char idx[8];

    // $lookup author
    {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s, l;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$lookup", &l);
        BSON_APPEND_UTF8(&l, "from", USERS_COLLECTION);
        BSON_APPEND_UTF8(&l, "localField", "author_id");
        BSON_APPEND_UTF8(&l, "foreignField", "_id");
        BSON_APPEND_UTF8(&l, "as", "author_info");
        bson_append_document_end(&s, &l);
        bson_append_document_end(pipeline, &s);
    }
    // $unwind author_info
    {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s, u;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$unwind", &u);
        BSON_APPEND_UTF8(&u, "path", "$author_info");
        BSON_APPEND_BOOL(&u, "preserveNullAndEmptyArrays", true);
        bson_append_document_end(&s, &u);
        bson_append_document_end(pipeline, &s);
    }
    // $lookup dir
    {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s, l;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$lookup", &l);
        BSON_APPEND_UTF8(&l, "from", MEDIA_DIRECTORIES_COLLECTION);
        BSON_APPEND_UTF8(&l, "localField", "dir_id");
        BSON_APPEND_UTF8(&l, "foreignField", "_id");
        BSON_APPEND_UTF8(&l, "as", "dir_info");
        bson_append_document_end(&s, &l);
        bson_append_document_end(pipeline, &s);
    }
    // $unwind dir_info
    {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s, u;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$unwind", &u);
        BSON_APPEND_UTF8(&u, "path", "$dir_info");
        BSON_APPEND_BOOL(&u, "preserveNullAndEmptyArrays", true);
        bson_append_document_end(&s, &u);
        bson_append_document_end(pipeline, &s);
    }
    // $match dir_id (if specified)
    if (dir_id && strlen(dir_id) == 24 && bson_oid_is_valid(dir_id, 24)) {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_oid_t doid;
        bson_oid_init_from_string(&doid, dir_id);
        bson_t s, m;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$match", &m);
        BSON_APPEND_OID(&m, "dir_id", &doid);
        bson_append_document_end(&s, &m);
        bson_append_document_end(pipeline, &s);
    }
    // $sort by _id desc (newest first)
    {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s, sort;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_DOCUMENT_BEGIN(&s, "$sort", &sort);
        BSON_APPEND_INT32(&sort, "_id", -1);
        bson_append_document_end(&s, &sort);
        bson_append_document_end(pipeline, &s);
    }
    // $skip
    if (skip > 0) {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_INT32(&s, "$skip", skip);
        bson_append_document_end(pipeline, &s);
    }
    // $limit
    if (limit > 0) {
        snprintf(idx, sizeof(idx), "%d", stage++);
        bson_t s;
        BSON_APPEND_DOCUMENT_BEGIN(pipeline, idx, &s);
        BSON_APPEND_INT32(&s, "$limit", limit);
        bson_append_document_end(pipeline, &s);
    }

    mongoc_cursor_t *cursor = mongoc_collection_aggregate(col, MONGOC_QUERY_NONE, pipeline, NULL, NULL);
    const bson_t *doc;

    size_t cap = 32;
    CmsMediaItem *list = calloc(cap, sizeof(*list));
    size_t count = 0;

    while (mongoc_cursor_next(cursor, &doc)) {
        if (count >= cap) { cap *= 2; list = realloc(list, cap * sizeof(*list)); }
        CmsMediaItem *item = &list[count];
        memset(item, 0, sizeof(*item));

        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), item->id);
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(item->name, sizeof(item->name), &iter);
        if (bson_iter_init_find(&iter, doc, "date") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(item->date, sizeof(item->date), &iter);
        if (bson_iter_init_find(&iter, doc, "format") && BSON_ITER_HOLDS_UTF8(&iter))
            copy_str(item->format, sizeof(item->format), &iter);
        if (bson_iter_init_find(&iter, doc, "author_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), item->author_id);

        if (bson_iter_init_find(&iter, doc, "author_info") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            bson_iter_t sub;
            if (bson_iter_recurse(&iter, &sub) && bson_iter_find(&sub, "email") && BSON_ITER_HOLDS_UTF8(&sub)) {
                const char *email = bson_iter_utf8(&sub, NULL);
                const char *at = email ? strchr(email, '@') : NULL;
                size_t len = at ? (size_t)(at - email) : (email ? strlen(email) : 0);
                if (len >= sizeof(item->author_username)) len = sizeof(item->author_username) - 1;
                if (len > 0) memcpy(item->author_username, email, len);
                item->author_username[len] = '\0';
            }
        }
        if (bson_iter_init_find(&iter, doc, "dir_info") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            bson_iter_t sub;
            if (bson_iter_recurse(&iter, &sub)) {
                if (bson_iter_find(&sub, "name") && BSON_ITER_HOLDS_UTF8(&sub))
                    copy_str(item->dir_name, sizeof(item->dir_name), &sub);
                if (bson_iter_find(&sub, "parent") && BSON_ITER_HOLDS_UTF8(&sub))
                    copy_str(item->dir_parent, sizeof(item->dir_parent), &sub);
            }
        }
        if (bson_iter_init_find(&iter, doc, "dir_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), item->dir_id);

        count++;
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(pipeline);
    mongoc_collection_destroy(col);

    *out = list;
    *out_count = count;
}

void cms_media_items_free(CmsMediaItem *items, size_t count) {
    (void)count;
    free(items);
}

int cms_insert_media(const char *filename, const char *author_id,
                     const char *dir_id, char *out_id) {
    if (!filename || !author_id || !dir_id) return -1;
    if (!bson_oid_is_valid(author_id, strlen(author_id))) return -1;
    if (!bson_oid_is_valid(dir_id, strlen(dir_id))) return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_COLLECTION);
    if (!col) return -1;

    bson_oid_t oid, a_oid, d_oid;
    bson_oid_init(&oid, NULL);
    bson_oid_init_from_string(&a_oid, author_id);
    bson_oid_init_from_string(&d_oid, dir_id);

    time_t now = time(NULL);
    struct tm tm_buf;
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm_buf));

    const char *ext = strrchr(filename, '.');
    const char *format = (ext && *(ext + 1)) ? ext + 1 : "unknown";

    bson_t *doc = bson_new();
    BSON_APPEND_OID(doc, "_id", &oid);
    BSON_APPEND_UTF8(doc, "name", filename);
    BSON_APPEND_UTF8(doc, "date", date_str);
    BSON_APPEND_UTF8(doc, "format", format);
    BSON_APPEND_OID(doc, "author_id", &a_oid);
    BSON_APPEND_OID(doc, "dir_id", &d_oid);

    bson_error_t error;
    int ret = mongoc_collection_insert_one(col, doc, NULL, NULL, &error) ? 0 : -1;
    if (ret != 0) LOG_ERROR("cms_insert_media: %s", error.message);
    if (ret == 0) bson_oid_to_string(&oid, out_id);

    bson_destroy(doc);
    mongoc_collection_destroy(col);
    return ret;
}

int cms_delete_media(const char *id_hex) {
    if (!id_hex || strlen(id_hex) != 24) return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_COLLECTION);
    if (!col) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *selector = BCON_NEW("_id", BCON_OID(&oid));

    bson_error_t error;
    int ret = mongoc_collection_delete_one(col, selector, NULL, NULL, &error) ? 0 : -1;
    if (ret != 0) LOG_ERROR("cms_delete_media: %s", error.message);

    bson_destroy(selector);
    mongoc_collection_destroy(col);
    return ret;
}
