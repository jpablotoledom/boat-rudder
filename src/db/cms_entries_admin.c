#include "cms_entries_admin.h"
#include "cms_users_admin.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Like bson_lang.c's resolve_lang_map(), but returns the exact value of
// `field`.`lang` with no "en" fallback - "" if `lang`'s key is absent. Used
// for editing, where each language field must reflect what's actually stored.
// (Duplicated per module by convention - see cms_menu.c.)
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

// Days since 1970-01-01 for a given Y-M-D (proleptic Gregorian calendar).
// Howard Hinnant's days_from_civil algorithm - avoids timegm(), which isn't
// available under this project's -D_XOPEN_SOURCE=700. The inverse of
// resolve_header_fields()'s gmtime_r/strftime in cms_entries.c.
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void parse_categories_edit(const bson_t *doc, CmsEntryEdit *out) {
    out->category_ids   = NULL;
    out->category_count = 0;

    bson_iter_t iter;
    if (!bson_iter_init_find(&iter, doc, "categories") || !BSON_ITER_HOLDS_ARRAY(&iter)) return;

    uint32_t len;
    const uint8_t *data;
    bson_iter_array(&iter, &len, &data);

    bson_t array;
    bson_init_static(&array, data, len);

    uint32_t count = bson_count_keys(&array);
    if (count == 0) return;

    char **ids = calloc(count, sizeof(char *));
    if (!ids) return;

    size_t n = 0;
    bson_iter_t arr_iter;
    if (bson_iter_init(&arr_iter, &array)) {
        while (n < count && bson_iter_next(&arr_iter)) {
            if (!BSON_ITER_HOLDS_OID(&arr_iter)) continue;

            char id_str[25];
            bson_oid_to_string(bson_iter_oid(&arr_iter), id_str);
            ids[n++] = strdup(id_str);
        }
    }

    out->category_ids   = ids;
    out->category_count = n;
}

// Like resolve_header_fields() in cms_entries.c, but reads title/summary/author
// exactly for each of langs[] (no "en" fallback) instead of resolving to one lang.
static void parse_header_edit(const bson_t *doc, const CmsLanguageItem *langs,
                               size_t lang_count, CmsEntryEdit *out) {
    out->header_title_values   = calloc(lang_count, sizeof(char *));
    out->header_summary_values = calloc(lang_count, sizeof(char *));
    out->header_author_name    = NULL;
    out->header_date[0]        = '\0';
    out->header_hide_author    = false;

    bson_iter_t iter;
    if (!bson_iter_init_find(&iter, doc, "header") || !BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        out->header_image_url = strdup("");
        for (size_t i = 0; i < lang_count; i++) {
            out->header_title_values[i]   = strdup("");
            out->header_summary_values[i] = strdup("");
        }
        out->header_author_name = strdup("");
        return;
    }

    uint32_t len;
    const uint8_t *data;
    bson_iter_document(&iter, &len, &data);

    bson_t header;
    bson_init_static(&header, data, len);

    bson_iter_t hiter;
    out->header_image_url = (bson_iter_init_find(&hiter, &header, "image_url") && BSON_ITER_HOLDS_UTF8(&hiter))
        ? strdup(bson_iter_utf8(&hiter, NULL)) : strdup("");

    for (size_t i = 0; i < lang_count; i++) {
        out->header_title_values[i]   = exact_lang_value(&header, "title", langs[i].code);
        out->header_summary_values[i] = exact_lang_value(&header, "summary", langs[i].code);
    }

    if (bson_iter_init_find(&hiter, &header, "hide_author") && BSON_ITER_HOLDS_BOOL(&hiter))
        out->header_hide_author = bson_iter_bool(&hiter);

    // The editor always shows the real author name, even when the public views
    // hide it, so the owner stays visible to whoever is editing.
    if (bson_iter_init_find(&hiter, &header, "author_id") && BSON_ITER_HOLDS_OID(&hiter)) {
        char author_id_hex[25];
        bson_oid_to_string(bson_iter_oid(&hiter), author_id_hex);
        out->header_author_name = cms_get_user_name_by_id(author_id_hex);
    } else {
        out->header_author_name = strdup("");
    }

    if (bson_iter_init_find(&hiter, &header, "date") && BSON_ITER_HOLDS_DATE_TIME(&hiter)) {
        time_t secs = (time_t)(bson_iter_date_time(&hiter) / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        strftime(out->header_date, sizeof(out->header_date), "%Y-%m-%d", &tm_buf);
    }
}

static int compare_blocks_edit_by_order(const void *a, const void *b) {
    return ((const CmsContentBlockEdit *)a)->order - ((const CmsContentBlockEdit *)b)->order;
}

// Like parse_content() in cms_entries.c, but keeps each block's _id (as hex)
// and reads text exactly for each of langs[] (no "en" fallback).
static void parse_content_edit(const bson_t *doc, const CmsLanguageItem *langs,
                                size_t lang_count, CmsEntryEdit *out) {
    out->content       = NULL;
    out->content_count = 0;

    bson_iter_t iter;
    if (!bson_iter_init_find(&iter, doc, "content") || !BSON_ITER_HOLDS_ARRAY(&iter)) return;

    uint32_t len;
    const uint8_t *data;
    bson_iter_array(&iter, &len, &data);

    bson_t array;
    bson_init_static(&array, data, len);

    uint32_t count = bson_count_keys(&array);
    if (count == 0) return;

    CmsContentBlockEdit *blocks = calloc(count, sizeof(CmsContentBlockEdit));
    if (!blocks) return;

    bson_iter_t arr_iter;
    size_t i = 0;
    if (bson_iter_init(&arr_iter, &array)) {
        while (i < count && bson_iter_next(&arr_iter)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&arr_iter)) continue;

            uint32_t blen;
            const uint8_t *bdata;
            bson_iter_document(&arr_iter, &blen, &bdata);

            bson_t block;
            bson_init_static(&block, bdata, blen);

            bson_iter_t biter;

            char id_str[25] = "";
            if (bson_iter_init_find(&biter, &block, "_id") && BSON_ITER_HOLDS_OID(&biter))
                bson_oid_to_string(bson_iter_oid(&biter), id_str);
            blocks[i].id = strdup(id_str);

            blocks[i].type = (bson_iter_init_find(&biter, &block, "type") && BSON_ITER_HOLDS_UTF8(&biter))
                ? strdup(bson_iter_utf8(&biter, NULL)) : strdup("");
            blocks[i].extra_data = (bson_iter_init_find(&biter, &block, "extra_data") && BSON_ITER_HOLDS_UTF8(&biter))
                ? strdup(bson_iter_utf8(&biter, NULL)) : strdup("");
            blocks[i].order = (bson_iter_init_find(&biter, &block, "order") && BSON_ITER_HOLDS_INT32(&biter))
                ? bson_iter_int32(&biter) : 0;

            blocks[i].text_values = calloc(lang_count, sizeof(char *));
            for (size_t j = 0; j < lang_count; j++)
                blocks[i].text_values[j] = exact_lang_value(&block, "text", langs[j].code);

            i++;
        }
    }

    qsort(blocks, i, sizeof(CmsContentBlockEdit), compare_blocks_edit_by_order);
    out->content       = blocks;
    out->content_count = i;
}

int cms_get_entry_for_edit(const char *id_hex, const CmsLanguageItem *langs,
                            size_t lang_count, CmsEntryEdit *out) {
    memset(out, 0, sizeof(*out));
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return 0;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        out->id   = strdup(id_hex);
        out->link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        out->type = (bson_iter_init_find(&iter, doc, "type") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        out->enabled = (bson_iter_init_find(&iter, doc, "enabled") && BSON_ITER_HOLDS_BOOL(&iter))
            ? bson_iter_bool(&iter) : false;

        if (bson_iter_init_find(&iter, doc, "created_by") && BSON_ITER_HOLDS_OID(&iter)) {
            char created_by_str[25];
            bson_oid_to_string(bson_iter_oid(&iter), created_by_str);
            out->created_by = strdup(created_by_str);
        } else {
            out->created_by = strdup("");
        }

        parse_categories_edit(doc, out);
        parse_header_edit(doc, langs, lang_count, out);
        parse_content_edit(doc, langs, lang_count, out);

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_entry_for_edit: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found;
}

void cms_entry_edit_free(CmsEntryEdit *entry, size_t lang_count) {
    if (!entry) return;

    free(entry->id);
    free(entry->link);
    free(entry->type);
    free(entry->created_by);

    for (size_t i = 0; i < entry->category_count; i++)
        free(entry->category_ids[i]);
    free(entry->category_ids);

    free(entry->header_image_url);
    for (size_t i = 0; i < lang_count; i++) {
        if (entry->header_title_values)   free(entry->header_title_values[i]);
        if (entry->header_summary_values) free(entry->header_summary_values[i]);
    }
    free(entry->header_title_values);
    free(entry->header_summary_values);
    free(entry->header_author_name);

    for (size_t i = 0; i < entry->content_count; i++) {
        free(entry->content[i].id);
        free(entry->content[i].type);
        free(entry->content[i].extra_data);
        for (size_t j = 0; j < lang_count; j++)
            free(entry->content[i].text_values[j]);
        free(entry->content[i].text_values);
    }
    free(entry->content);

    memset(entry, 0, sizeof(*entry));
}

char *cms_create_entry(const char *created_by_id_hex, const char *type) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return NULL;

    bson_oid_t oid;
    bson_oid_init(&oid, NULL);

    bson_t doc;
    bson_init(&doc);
    bson_append_oid(&doc, "_id", -1, &oid);
    bson_append_utf8(&doc, "link", -1, "", -1);
    bson_append_utf8(&doc, "type", -1, type, -1);
    bson_append_bool(&doc, "enabled", -1, false);
    bson_t categories_arr;
    bson_append_array_begin(&doc, "categories", -1, &categories_arr);
    bson_append_array_end(&doc, &categories_arr);
    bson_t header_doc;
    bson_append_document_begin(&doc, "header", -1, &header_doc);
    if (created_by_id_hex && bson_oid_is_valid(created_by_id_hex, strlen(created_by_id_hex))) {
        bson_oid_t author_oid;
        bson_oid_init_from_string(&author_oid, created_by_id_hex);
        bson_append_oid(&header_doc, "author_id", -1, &author_oid);
    }
    bson_append_document_end(&doc, &header_doc);
    bson_t content_arr;
    bson_append_array_begin(&doc, "content", -1, &content_arr);
    bson_append_array_end(&doc, &content_arr);

    if (created_by_id_hex && bson_oid_is_valid(created_by_id_hex, strlen(created_by_id_hex))) {
        bson_oid_t created_by_oid;
        bson_oid_init_from_string(&created_by_oid, created_by_id_hex);
        bson_append_oid(&doc, "created_by", -1, &created_by_oid);
    }

    bson_error_t error;
    bool ok = mongoc_collection_insert_one(collection, &doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_create_entry: insert failed: %s", error.message);

    bson_destroy(&doc);
    mongoc_collection_destroy(collection);

    if (!ok) return NULL;

    char id_str[25];
    bson_oid_to_string(&oid, id_str);
    return strdup(id_str);
}

int cms_update_entry_meta(const char *id_hex, const char *link, const char *type,
                           bool enabled, char *const *category_ids, size_t category_count) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "link", -1, link, -1);
    bson_append_utf8(&set_doc, "type", -1, type, -1);
    bson_append_bool(&set_doc, "enabled", -1, enabled);

    bson_t categories_arr;
    bson_append_array_begin(&set_doc, "categories", -1, &categories_arr);
    size_t n = 0;
    for (size_t i = 0; i < category_count; i++) {
        if (!bson_oid_is_valid(category_ids[i], strlen(category_ids[i]))) continue;

        bson_oid_t cat_oid;
        bson_oid_init_from_string(&cat_oid, category_ids[i]);

        char key[16];
        snprintf(key, sizeof(key), "%zu", n++);
        bson_append_oid(&categories_arr, key, -1, &cat_oid);
    }
    bson_append_array_end(&set_doc, &categories_arr);
    bson_append_document_end(&update, &set_doc);

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_entry_meta: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_entry_header(const char *id_hex, const char *image_url, const char *date,
                             bool hide_author,
                             const CmsLanguageItem *langs, size_t lang_count,
                             char *const *title_values, char *const *summary_values) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "header.image_url", -1, image_url, -1);
    bson_append_bool(&set_doc, "header.hide_author", -1, hide_author);

    for (size_t i = 0; i < lang_count; i++) {
        char key[40];
        snprintf(key, sizeof(key), "header.title.%s", langs[i].code);
        bson_append_utf8(&set_doc, key, -1, title_values[i], -1);
        snprintf(key, sizeof(key), "header.summary.%s", langs[i].code);
        bson_append_utf8(&set_doc, key, -1, summary_values[i], -1);
    }

    int year, month, day;
    if (date && sscanf(date, "%d-%d-%d", &year, &month, &day) == 3) {
        int64_t days = days_from_civil(year, month, day);
        bson_append_date_time(&set_doc, "header.date", -1, days * 86400 * 1000);
    }

    bson_append_document_end(&update, &set_doc);

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_entry_header: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_entry_content(const char *id_hex, const CmsLanguageItem *langs,
                              size_t lang_count, CmsContentBlockEdit *blocks,
                              size_t block_count) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_t content_arr;
    bson_append_array_begin(&set_doc, "content", -1, &content_arr);

    size_t n = 0;
    for (size_t i = 0; i < block_count; i++) {
        bson_oid_t block_oid;
        if (blocks[i].id && bson_oid_is_valid(blocks[i].id, strlen(blocks[i].id))) {
            bson_oid_init_from_string(&block_oid, blocks[i].id);
        } else {
            // Blocks written before content[]._id existed (or imported without
            // one) reach the editor with an empty id. Mint one instead of
            // dropping the block - skipping it here would silently erase it
            // from the $set'd array.
            bson_oid_init(&block_oid, NULL);
            char new_id[25];
            bson_oid_to_string(&block_oid, new_id);
            free(blocks[i].id);
            blocks[i].id = strdup(new_id);
        }

        char key[16];
        snprintf(key, sizeof(key), "%zu", n++);

        bson_t block_doc;
        bson_append_document_begin(&content_arr, key, -1, &block_doc);
        bson_append_oid(&block_doc, "_id", -1, &block_oid);
        bson_append_utf8(&block_doc, "type", -1, blocks[i].type, -1);
        bson_append_int32(&block_doc, "order", -1, blocks[i].order);
        bson_append_utf8(&block_doc, "extra_data", -1, blocks[i].extra_data, -1);

        bson_t text_doc;
        bson_append_document_begin(&block_doc, "text", -1, &text_doc);
        for (size_t j = 0; j < lang_count; j++)
            bson_append_utf8(&text_doc, langs[j].code, -1, blocks[i].text_values[j], -1);
        bson_append_document_end(&block_doc, &text_doc);

        bson_append_document_end(&content_arr, &block_doc);
    }

    bson_append_array_end(&set_doc, &content_arr);
    bson_append_document_end(&update, &set_doc);

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_entry_content: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_add_entry_content_block(const char *id_hex, const char *type, int order,
                                 char *out_block_id, size_t out_block_id_size) {
    if (out_block_id_size < 25) return -1;
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_oid_t block_oid;
    bson_oid_init(&block_oid, NULL);

    bson_t update;
    bson_init(&update);
    bson_t push_doc;
    bson_append_document_begin(&update, "$push", -1, &push_doc);
    bson_t block_doc;
    bson_append_document_begin(&push_doc, "content", -1, &block_doc);
    bson_append_oid(&block_doc, "_id", -1, &block_oid);
    bson_append_utf8(&block_doc, "type", -1, type, -1);
    bson_append_int32(&block_doc, "order", -1, order);
    bson_append_utf8(&block_doc, "extra_data", -1, "", -1);
    bson_t text_doc;
    bson_append_document_begin(&block_doc, "text", -1, &text_doc);
    bson_append_document_end(&block_doc, &text_doc);
    bson_append_document_end(&push_doc, &block_doc);
    bson_append_document_end(&update, &push_doc);

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_add_entry_content_block: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);

    if (!ok) return -1;

    bson_oid_to_string(&block_oid, out_block_id);
    return 0;
}

int cms_remove_entry_content_block(const char *id_hex, const char *block_id) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;
    if (!bson_oid_is_valid(block_id, strlen(block_id))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid, block_oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_oid_init_from_string(&block_oid, block_id);

    bson_t *query  = BCON_NEW("_id", BCON_OID(&oid));
    bson_t *update = BCON_NEW("$pull", "{", "content", "{", "_id", BCON_OID(&block_oid), "}", "}");

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_remove_entry_content_block: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_delete_entry(const char *id_hex) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_delete_entry: delete failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}
