#include "cms_menu.h"
#include "bson_lang.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

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

    size_t count = 0;
    const bson_t *doc;
    while (count < MENU_ITEM_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        items[count].link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].name = resolve_lang_map(doc, "name", lang);

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

// Like resolve_lang_map(), but returns the exact value of `field`.`lang`
// with no "en" fallback - "" if `lang`'s key is absent. Used for editing,
// where each language field must reflect what's actually stored.
static char *exact_lang_value(const bson_t *parent, const char *field, const char *lang) {
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

    return strdup("");
}

void cms_get_menu_admin_items(const char *lang, CmsMenuAdminItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW(
        "sort", "{", "order", BCON_INT32(1), "}",
        "limit", BCON_INT64((int64_t)MENU_ADMIN_LIST_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsMenuAdminItem *items = calloc(MENU_ADMIN_LIST_LIMIT, sizeof(CmsMenuAdminItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < MENU_ADMIN_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        char id_str[25] = "";
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), id_str);

        items[count].id = strdup(id_str);
        items[count].link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].name = resolve_lang_map(doc, "name", lang);
        items[count].order = (bson_iter_init_find(&iter, doc, "order") && BSON_ITER_HOLDS_INT32(&iter))
            ? bson_iter_int32(&iter) : 0;
        items[count].enabled = (bson_iter_init_find(&iter, doc, "enabled") && BSON_ITER_HOLDS_BOOL(&iter))
            ? bson_iter_bool(&iter) : false;

        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_menu_admin_items: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_menu_admin_free(CmsMenuAdminItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].id);
        free(items[i].link);
        free(items[i].name);
    }

    free(items);
}

int cms_get_menu_item_values(const char *id_hex, const CmsLanguageItem *langs, size_t lang_count,
                              char **out_values, char *out_link, size_t out_link_size,
                              int *out_order, bool *out_enabled) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        for (size_t i = 0; i < lang_count; i++)
            out_values[i] = exact_lang_value(doc, "name", langs[i].code);

        const char *link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "";
        strncpy(out_link, link, out_link_size - 1);
        out_link[out_link_size - 1] = '\0';

        *out_order = (bson_iter_init_find(&iter, doc, "order") && BSON_ITER_HOLDS_INT32(&iter))
            ? bson_iter_int32(&iter) : 0;

        *out_enabled = (bson_iter_init_find(&iter, doc, "enabled") && BSON_ITER_HOLDS_BOOL(&iter))
            ? bson_iter_bool(&iter) : false;

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_menu_item_values: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

void cms_menu_name_values_free(char **values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; i++) free(values[i]);
    free(values);
}

int cms_create_menu_item(const char *link, int order, bool enabled,
                          const CmsLanguageItem *langs, size_t lang_count, char *const *values) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return -1;

    bson_t doc;
    bson_init(&doc);
    bson_append_utf8(&doc, "link", -1, link, -1);
    bson_append_int32(&doc, "order", -1, order);
    bson_append_bool(&doc, "enabled", -1, enabled);
    bson_t name_doc;
    bson_append_document_begin(&doc, "name", -1, &name_doc);
    for (size_t i = 0; i < lang_count; i++)
        bson_append_utf8(&name_doc, langs[i].code, -1, values[i], -1);
    bson_append_document_end(&doc, &name_doc);

    bson_error_t error;
    bool ok = mongoc_collection_insert_one(collection, &doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_create_menu_item: insert failed: %s", error.message);

    bson_destroy(&doc);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_menu_item(const char *id_hex, const char *link, int order, bool enabled,
                          const CmsLanguageItem *langs, size_t lang_count, char *const *values) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "link", -1, link, -1);
    bson_append_int32(&set_doc, "order", -1, order);
    bson_append_bool(&set_doc, "enabled", -1, enabled);
    bson_t name_doc;
    bson_append_document_begin(&set_doc, "name", -1, &name_doc);
    for (size_t i = 0; i < lang_count; i++)
        bson_append_utf8(&name_doc, langs[i].code, -1, values[i], -1);
    bson_append_document_end(&set_doc, &name_doc);
    bson_append_document_end(&update, &set_doc);

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_menu_item: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_delete_menu_item(const char *id_hex) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(MENU_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_delete_menu_item: delete failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}
