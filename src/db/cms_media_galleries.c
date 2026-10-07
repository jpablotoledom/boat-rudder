#include "cms_media_galleries.h"
#include "bson_lang.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

int cms_upsert_media_gallery(const char *gallery_id, const char *entry_id,
                              const char *urls_csv, char *out_id) {
    if (!entry_id || !urls_csv) return -1;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_GALLERIES_COLLECTION);
    if (!col) return -1;

    // Build BSON array from semicolon-separated URLs
    bson_t content_arr;
    bson_init(&content_arr);
    char *copy = strdup(urls_csv);
    char *saveptr = NULL;
    int idx = 0;
    for (char *tok = strtok_r(copy, ";", &saveptr); tok; tok = strtok_r(NULL, ";", &saveptr)) {
        while (*tok == ' ') tok++;
        if (*tok) {
            char key[16];
            snprintf(key, sizeof(key), "%d", idx++);
            bson_append_utf8(&content_arr, key, -1, tok, -1);
        }
    }
    free(copy);

    bson_error_t error;
    int ret;

    bool is_update = gallery_id && strlen(gallery_id) == 24 &&
                     bson_oid_is_valid(gallery_id, 24);

    if (is_update) {
        bson_oid_t oid;
        bson_oid_init_from_string(&oid, gallery_id);

        bson_t *selector = BCON_NEW("_id", BCON_OID(&oid));
        bson_t *update = bson_new();
        bson_t set_doc;
        BSON_APPEND_DOCUMENT_BEGIN(update, "$set", &set_doc);
        BSON_APPEND_ARRAY(&set_doc, "content", &content_arr);
        bson_append_document_end(update, &set_doc);

        ret = mongoc_collection_update_one(col, selector, update, NULL, NULL, &error) ? 0 : -1;

        bson_destroy(selector);
        bson_destroy(update);

        if (ret == 0) {
            strncpy(out_id, gallery_id, 24);
            out_id[24] = '\0';
        }
    } else {
        bson_oid_t oid;
        bson_oid_init(&oid, NULL);

        bson_oid_t entry_oid;
        if (bson_oid_is_valid(entry_id, strlen(entry_id)))
            bson_oid_init_from_string(&entry_oid, entry_id);
        else
            bson_oid_init(&entry_oid, NULL);

        bson_t *doc = bson_new();
        BSON_APPEND_OID(doc, "_id", &oid);
        BSON_APPEND_ARRAY(doc, "content", &content_arr);
        BSON_APPEND_OID(doc, "entry_id", &entry_oid);

        ret = mongoc_collection_insert_one(col, doc, NULL, NULL, &error) ? 0 : -1;
        bson_destroy(doc);

        if (ret == 0) bson_oid_to_string(&oid, out_id);
    }

    bson_destroy(&content_arr);
    mongoc_collection_destroy(col);

    if (ret != 0) LOG_ERROR("cms_upsert_media_gallery: %s", error.message);
    return ret;
}

bool cms_get_media_gallery(const char *id_hex, CmsMediaGallery *out) {
    memset(out, 0, sizeof(*out));
    if (!id_hex || strlen(id_hex) != 24) return false;

    mongoc_collection_t *col = mongodb_manager_get_collection(MEDIA_GALLERIES_COLLECTION);
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
        if (bson_iter_init_find(&iter, doc, "entry_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), out->entry_id);

        if (bson_iter_init_find(&iter, doc, "content") && BSON_ITER_HOLDS_ARRAY(&iter)) {
            bson_iter_t arr;
            if (bson_iter_recurse(&iter, &arr)) {
                size_t cap = 16;
                out->urls = calloc(cap, sizeof(char *));
                while (bson_iter_next(&arr) && out->url_count < 128) {
                    if (BSON_ITER_HOLDS_UTF8(&arr)) {
                        if (out->url_count >= cap) {
                            cap *= 2;
                            out->urls = realloc(out->urls, cap * sizeof(char *));
                        }
                        out->urls[out->url_count++] = strdup(bson_iter_utf8(&arr, NULL));
                    }
                }
            }
        }
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_collection_destroy(col);
    return found;
}

void cms_media_gallery_free(CmsMediaGallery *g) {
    if (!g) return;
    for (size_t i = 0; i < g->url_count; i++) free(g->urls[i]);
    free(g->urls);
    g->urls = NULL;
    g->url_count = 0;
}

bool cms_get_entry_backlink(const char *entry_id_hex, const char *lang,
                             char *out_url, size_t url_size,
                             char *out_title, size_t title_size) {
    out_url[0] = '\0';
    out_title[0] = '\0';
    if (!entry_id_hex || strlen(entry_id_hex) != 24 || !bson_oid_is_valid(entry_id_hex, 24))
        return false;

    mongoc_collection_t *col = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!col) return false;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, entry_id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, query, NULL, NULL);
    const bson_t *doc;
    bool found = false;

    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        char link[64] = "";
        char type[32] = "blog";

        if (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            strncpy(link, bson_iter_utf8(&iter, NULL), sizeof(link) - 1);
        if (bson_iter_init_find(&iter, doc, "type") && BSON_ITER_HOLDS_UTF8(&iter))
            strncpy(type, bson_iter_utf8(&iter, NULL), sizeof(type) - 1);

        if (link[0]) {
            snprintf(out_url, url_size, "/%s/%s", strcmp(type, "page") == 0 ? "page" : "blog", link);
            found = true;
        }

        if (bson_iter_init_find(&iter, doc, "header") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            uint32_t len; const uint8_t *data;
            bson_iter_document(&iter, &len, &data);
            bson_t header;
            bson_init_static(&header, data, len);

            char *title = resolve_lang_map(&header, "title", lang);
            if (title) {
                strncpy(out_title, title, title_size - 1);
                out_title[title_size - 1] = '\0';
                free(title);
            }
        }
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_collection_destroy(col);
    return found;
}
