#include "entry_index.h"
#include "../blog_list/blog_list.h"
#include "../../db/cms_entries.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/related_ids.h"
#include "../../utils/request_lang.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// Up to twenty on a page, in a small cell: epoch 3 needs no bigger than `_small`.
static char *row_thumb_url(const char *image_url, int epoch) {
    return epoch == EPOCH_MODERN ? image_url_variant(image_url, "_small")
                                 : blog_list_thumb_url(image_url, epoch);
}

static char *render_row(const CmsBlogListItem *item, const char *item_tpl, const char *image_tpl, int epoch) {
    char *link = render_template("/blog/%s", item->link);
    if (!link) return NULL;

    char *result = NULL;
    if (epoch >= EPOCH_EARLY) {
        char *thumb = (item->header_image_url && item->header_image_url[0])
            ? row_thumb_url(item->header_image_url, epoch) : NULL;
        char *image_html = thumb ? render_template(image_tpl, thumb, link) : strdup("");
        if (image_html)
            result = render_template(item_tpl, link, image_html, item->header_title, item->header_summary);
        free(image_html);
        free(thumb);
    } else {
        // Text-only browsers and WML: no picture.
        result = render_template(item_tpl, link, item->header_title, item->header_summary);
    }

    free(link);
    return result;
}

char *entry_index_render(const char *extra_data, const char *entry_id, int epoch) {
    char ids[ENTRY_INDEX_MAX][RELATED_ID_BUF_SIZE];
    size_t chosen = related_ids_parse(extra_data, entry_id, ids, ENTRY_INDEX_MAX);

    const char *id_ptrs[ENTRY_INDEX_MAX];
    for (size_t i = 0; i < chosen; i++) id_ptrs[i] = ids[i];

    CmsBlogListItem *items = NULL;
    size_t count = 0;
    cms_get_blog_entries_by_ids(request_lang(), id_ptrs, chosen, &items, &count);
    if (count == 0) return strdup("");

    char *block_tpl = load_template("elements/entry-index/entry-index_epoch%d.html", epoch);
    char *item_tpl  = load_template("elements/entry-index/entry-index-item_epoch%d.html", epoch);
    char *image_tpl = epoch >= EPOCH_EARLY
        ? load_template("elements/entry-index/entry-index-image_epoch%d.html", epoch) : NULL;

    char *rows = NULL;
    char *result = NULL;
    if (block_tpl && item_tpl && (epoch < EPOCH_EARLY || image_tpl)) {
        rows = strdup("");
        for (size_t i = 0; rows && i < count; i++) {
            char *row = render_row(&items[i], item_tpl, image_tpl, epoch);
            rows = row ? str_append(rows, row) : (free(rows), NULL);
            free(row);
        }
        if (rows) result = render_template(block_tpl, rows);
    }

    free(rows);
    free(block_tpl);
    free(item_tpl);
    free(image_tpl);
    cms_blog_list_free(items, count);
    return result;
}
