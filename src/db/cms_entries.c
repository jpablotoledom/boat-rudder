#include "cms_entries.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Maps configs/settings.conf's "Eng"/"Esp" lang values to ISO 639-1 codes
// used by the embedded entries schema. Defaults to "en".
static const char *iso_lang(const char *configured_lang) {
    if (configured_lang && strcasecmp(configured_lang, "esp") == 0) return "es";
    return "en";
}

// Resolves a `map<lang,string>` subdocument (e.g. {"en": "...", "es": "..."})
// to `lang`'s value, falling back to "en", then "". Always returns a
// malloc'd string.
static char *resolve_lang_map(const bson_t *parent, const char *field, const char *lang) {
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

    if (strcmp(lang, "en") != 0 &&
        bson_iter_init_find(&map_iter, &map, "en") && BSON_ITER_HOLDS_UTF8(&map_iter))
        return strdup(bson_iter_utf8(&map_iter, NULL));

    return strdup("");
}

static void parse_header(const bson_t *doc, const char *lang, CmsEntry *out) {
    bson_iter_t iter;

    if (!bson_iter_init_find(&iter, doc, "header") || !BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        out->header_image_url = strdup("");
        out->header_title     = strdup("");
        out->header_summary   = strdup("");
        out->header_author    = strdup("");
        out->header_date[0]   = '\0';
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

    out->header_title   = resolve_lang_map(&header, "title", lang);
    out->header_summary = resolve_lang_map(&header, "summary", lang);
    out->header_author  = resolve_lang_map(&header, "author", lang);

    out->header_date[0] = '\0';
    if (bson_iter_init_find(&hiter, &header, "date") && BSON_ITER_HOLDS_DATE_TIME(&hiter)) {
        time_t secs = (time_t)(bson_iter_date_time(&hiter) / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        strftime(out->header_date, sizeof(out->header_date), "%Y-%m-%d", &tm_buf);
    }
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
        const char *resolved_lang = iso_lang(lang);
        bson_iter_t iter;

        out->link = (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup(link);
        out->type = (bson_iter_init_find(&iter, doc, "type") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");

        parse_header(doc, resolved_lang, out);
        parse_content(doc, resolved_lang, out);

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

    for (size_t i = 0; i < entry->content_count; i++) {
        free(entry->content[i].type);
        free(entry->content[i].text);
        free(entry->content[i].extra_data);
    }
    free(entry->content);

    memset(entry, 0, sizeof(*entry));
}
