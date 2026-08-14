#include "cms_entries.h"
#include "bson_lang.h"
#include "cms_users_admin.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include "../utils/template_utils.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Resolves header.image_url/title/summary/author/date from doc's "header" subdocument,
// with title/summary/author resolved to `lang` (map<lang,string>). All output strings are
// malloc'd ("" on absence); date is "YYYY-MM-DD" or "" if absent.
static void resolve_header_fields(const bson_t *doc, const char *lang,
                                   char **image_url, char **title, char **summary,
                                   char **author, char date[16]) {
    bson_iter_t iter;

    if (!bson_iter_init_find(&iter, doc, "header") || !BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        *image_url = strdup("");
        *title     = strdup("");
        *summary   = strdup("");
        *author    = strdup("");
        date[0]    = '\0';
        return;
    }

    uint32_t len;
    const uint8_t *data;
    bson_iter_document(&iter, &len, &data);

    bson_t header;
    bson_init_static(&header, data, len);

    bson_iter_t hiter;
    *image_url = (bson_iter_init_find(&hiter, &header, "image_url") && BSON_ITER_HOLDS_UTF8(&hiter))
        ? strdup(bson_iter_utf8(&hiter, NULL)) : strdup("");

    *title   = resolve_lang_map(&header, "title", lang);
    *summary = resolve_lang_map(&header, "summary", lang);

    if (bson_iter_init_find(&hiter, &header, "author_id") && BSON_ITER_HOLDS_OID(&hiter)) {
        char author_id_hex[25];
        bson_oid_to_string(bson_iter_oid(&hiter), author_id_hex);
        *author = cms_get_user_name_by_id(author_id_hex);
    } else {
        *author = strdup("");
    }

    date[0] = '\0';
    if (bson_iter_init_find(&hiter, &header, "date") && BSON_ITER_HOLDS_DATE_TIME(&hiter)) {
        time_t secs = (time_t)(bson_iter_date_time(&hiter) / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        strftime(date, 16, "%Y-%m-%d", &tm_buf);
    }
}

static void parse_header(const bson_t *doc, const char *lang, CmsEntry *out) {
    resolve_header_fields(doc, lang, &out->header_image_url, &out->header_title,
                           &out->header_summary, &out->header_author, out->header_date);
}

// Resolves doc.categories[] (ObjectId[]) to entry_categories.name (resolved to
// `lang`) and a URL slug ("/blog/category/<slug>"). *out_names/*out_links/*out_count
// are NULL/0 on any failure - categories are decorative and must never fail the caller.
static void resolve_category_names(const bson_t *doc, const char *lang,
                                    char ***out_names, char ***out_links, size_t *out_count) {
    *out_names = NULL;
    *out_links = NULL;
    *out_count = 0;

    bson_iter_t iter;
    if (!bson_iter_init_find(&iter, doc, "categories") || !BSON_ITER_HOLDS_ARRAY(&iter)) return;

    uint32_t len;
    const uint8_t *data;
    bson_iter_array(&iter, &len, &data);

    bson_t array;
    bson_init_static(&array, data, len);

    uint32_t count = bson_count_keys(&array);
    if (count == 0) return;

    bson_oid_t *oids = calloc(count, sizeof(bson_oid_t));
    if (!oids) return;

    uint32_t oid_count = 0;
    bson_iter_t arr_iter;
    if (bson_iter_init(&arr_iter, &array)) {
        while (oid_count < count && bson_iter_next(&arr_iter)) {
            if (BSON_ITER_HOLDS_OID(&arr_iter))
                bson_oid_copy(bson_iter_oid(&arr_iter), &oids[oid_count++]);
        }
    }

    if (oid_count == 0) {
        free(oids);
        return;
    }

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRY_CATEGORIES_COLLECTION);
    if (!collection) {
        free(oids);
        return;
    }

    bson_t query;
    bson_init(&query);
    bson_t in_doc;
    bson_append_document_begin(&query, "_id", -1, &in_doc);
    bson_t in_array;
    bson_append_array_begin(&in_doc, "$in", -1, &in_array);
    for (uint32_t i = 0; i < oid_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%u", i);
        bson_append_oid(&in_array, key, -1, &oids[i]);
    }
    bson_append_array_end(&in_doc, &in_array);
    bson_append_document_end(&query, &in_doc);
    free(oids);

    char **names = calloc(oid_count, sizeof(char *));
    char **links = calloc(oid_count, sizeof(char *));
    if (!names || !links) {
        free(names);
        free(links);
        bson_destroy(&query);
        mongoc_collection_destroy(collection);
        return;
    }

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, &query, NULL, NULL);
    size_t found = 0;
    const bson_t *cdoc;
    while (found < oid_count && mongoc_cursor_next(cursor, &cdoc)) {
        names[found] = resolve_lang_map(cdoc, "name", lang);
        char *slug = slugify(names[found]);
        char *link = NULL;
        if (slug) {
            size_t len = strlen("/blog/category/") + strlen(slug) + 1;
            link = malloc(len);
            if (link) snprintf(link, len, "/blog/category/%s", slug);
            free(slug);
        }
        links[found] = link ? link : strdup("#");
        found++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("resolve_category_names: cursor error: %s", error.message);

    mongoc_cursor_destroy(cursor);
    bson_destroy(&query);
    mongoc_collection_destroy(collection);

    *out_names = names;
    *out_links = links;
    *out_count = found;
}

static void parse_categories(const bson_t *doc, const char *lang, CmsEntry *out) {
    resolve_category_names(doc, lang, &out->category_names, &out->category_links, &out->category_count);
}

static int compare_blocks_by_order(const void *a, const void *b) {
    return ((const CmsContentBlock *)a)->order - ((const CmsContentBlock *)b)->order;
}

static void parse_content(const bson_t *doc, const char *lang, CmsEntry *out) {
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

    CmsContentBlock *blocks = calloc(count, sizeof(CmsContentBlock));
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
            blocks[i].type = (bson_iter_init_find(&biter, &block, "type") && BSON_ITER_HOLDS_UTF8(&biter))
                ? strdup(bson_iter_utf8(&biter, NULL)) : strdup("");
            blocks[i].extra_data = (bson_iter_init_find(&biter, &block, "extra_data") && BSON_ITER_HOLDS_UTF8(&biter))
                ? strdup(bson_iter_utf8(&biter, NULL)) : strdup("");
            blocks[i].order = (bson_iter_init_find(&biter, &block, "order") && BSON_ITER_HOLDS_INT32(&biter))
                ? bson_iter_int32(&biter) : 0;
            blocks[i].text = resolve_lang_map(&block, "text", lang);

            i++;
        }
    }

    qsort(blocks, i, sizeof(CmsContentBlock), compare_blocks_by_order);
    out->content       = blocks;
    out->content_count = i;
}

int cms_get_entry_by_link(const char *link, const char *lang, CmsEntry *out) {
    memset(out, 0, sizeof(*out));

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return 0;

    bson_t *query = BCON_NEW("link", BCON_UTF8(link), "enabled", BCON_BOOL(true));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        out->link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup(link);
        out->type = (bson_iter_init_find(&iter, doc, "type") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");

        parse_header(doc, lang, out);
        parse_categories(doc, lang, out);
        parse_content(doc, lang, out);

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("cms_get_entry_by_link: cursor error: %s", error.message);
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found;
}

void cms_entry_free(CmsEntry *entry) {
    if (!entry) return;

    free(entry->link);
    free(entry->type);
    free(entry->header_image_url);
    free(entry->header_title);
    free(entry->header_summary);
    free(entry->header_author);

    for (size_t i = 0; i < entry->category_count; i++) {
        free(entry->category_names[i]);
        free(entry->category_links[i]);
    }
    free(entry->category_names);
    free(entry->category_links);

    for (size_t i = 0; i < entry->content_count; i++) {
        free(entry->content[i].type);
        free(entry->content[i].text);
        free(entry->content[i].extra_data);
    }
    free(entry->content);

    memset(entry, 0, sizeof(*entry));
}

// Populates *item from doc: link, type, header.* (resolved to lang) and
// categories[] (resolved to lang). Shared by cms_get_blog_entries() and
// cms_get_admin_entries().
static void populate_entry_list_item(const bson_t *doc, const char *lang, CmsBlogListItem *item) {
    bson_iter_t iter;

    char id_str[25] = "";
    if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
        bson_oid_to_string(bson_iter_oid(&iter), id_str);
    item->id = strdup(id_str);

    item->link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
        ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
    item->type = (bson_iter_init_find(&iter, doc, "type") && BSON_ITER_HOLDS_UTF8(&iter))
        ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");

    resolve_header_fields(doc, lang, &item->header_image_url, &item->header_title,
                           &item->header_summary, &item->header_author, item->header_date);
    resolve_category_names(doc, lang, &item->category_names, &item->category_links, &item->category_count);
}

void cms_get_blog_entries(const char *lang, size_t limit, CmsBlogListItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return;

    bson_t *query = BCON_NEW("type", BCON_UTF8("blog"), "enabled", BCON_BOOL(true));
    bson_t *opts = BCON_NEW(
        "sort", "{", "header.date", BCON_INT32(-1), "}",
        "limit", BCON_INT64((int64_t)limit)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsBlogListItem *items = calloc(limit, sizeof(CmsBlogListItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < limit && mongoc_cursor_next(cursor, &doc))
        populate_entry_list_item(doc, lang, &items[count++]);

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_blog_entries: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_get_admin_entries(const char *lang, const char *type_filter, const char *created_by_hex,
                            CmsBlogListItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    if (type_filter) bson_append_utf8(query, "type", -1, type_filter, -1);
    if (created_by_hex && bson_oid_is_valid(created_by_hex, strlen(created_by_hex))) {
        bson_oid_t created_by_oid;
        bson_oid_init_from_string(&created_by_oid, created_by_hex);
        bson_append_oid(query, "created_by", -1, &created_by_oid);
    }

    bson_t *opts = BCON_NEW(
        "sort", "{", "header.date", BCON_INT32(-1), "}",
        "limit", BCON_INT64((int64_t)ENTRIES_LIST_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsBlogListItem *items = calloc(ENTRIES_LIST_LIMIT, sizeof(CmsBlogListItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < ENTRIES_LIST_LIMIT && mongoc_cursor_next(cursor, &doc))
        populate_entry_list_item(doc, lang, &items[count++]);

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_admin_entries: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_get_blog_entries_by_category(const char *lang, size_t limit,
                                       const char *category_id_hex,
                                       CmsBlogListItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    if (!category_id_hex || !bson_oid_is_valid(category_id_hex, strlen(category_id_hex))) return;

    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!collection) return;

    bson_oid_t cat_oid;
    bson_oid_init_from_string(&cat_oid, category_id_hex);

    bson_t *query = BCON_NEW(
        "type",       BCON_UTF8("blog"),
        "enabled",    BCON_BOOL(true),
        "categories", "{", "$in", "[", BCON_OID(&cat_oid), "]", "}"
    );
    bson_t *opts = BCON_NEW(
        "sort",  "{", "header.date", BCON_INT32(-1), "}",
        "limit", BCON_INT64((int64_t)limit)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsBlogListItem *items = calloc(limit, sizeof(CmsBlogListItem));
    if (!items) {
        bson_destroy(query); bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < limit && mongoc_cursor_next(cursor, &doc))
        populate_entry_list_item(doc, lang, &items[count++]);

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_blog_entries_by_category: cursor error: %s", error.message);

    bson_destroy(query); bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_blog_list_free(CmsBlogListItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].id);
        free(items[i].link);
        free(items[i].type);
        free(items[i].header_image_url);
        free(items[i].header_title);
        free(items[i].header_summary);
        free(items[i].header_author);

        for (size_t j = 0; j < items[i].category_count; j++) {
            free(items[i].category_names[j]);
            free(items[i].category_links[j]);
        }
        free(items[i].category_names);
        free(items[i].category_links);
    }

    free(items);
}
