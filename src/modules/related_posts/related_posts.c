#include "related_posts.h"
#include "../blog_list/blog_list.h"
#include "../../db/cms_entries.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/json_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/related_ids.h"
#include "../../utils/request_lang.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// What the picker shows, plus room for the ones it already holds and hides.
#define RELATED_LOOKUP_LIMIT (8 + ENTRY_INDEX_MAX)

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// The label over a chosen pair. The site has no translation table for its
// fixed texts, so Spanish is spelled out here and any other language reads
// English, like the rest of them.
static const char *pair_label(int is_next) {
    if (strcmp(request_lang(), "es") == 0) return is_next ? "Art\xc3\xad" "culo siguiente" : "Art\xc3\xad" "culo anterior";
    return is_next ? "Next article" : "Previous article";
}

static char *render_item(const CmsBlogListItem *item, const char *item_tpl, const char *image_tpl,
                         const char *label_tpl, const char *label, const char *width, int epoch) {
    char *link = render_template("/blog/%s", item->link);
    char *label_html = label ? render_template(label_tpl, label) : strdup("");
    char *result = NULL;

    if (link && label_html && epoch >= EPOCH_EARLY) {
        char *thumb = (item->header_image_url && item->header_image_url[0])
            ? blog_list_thumb_url(item->header_image_url, epoch) : NULL;
        char *image_html = thumb ? render_template(image_tpl, thumb, link) : strdup("");
        if (image_html)
            result = render_template(item_tpl, link, image_html, label_html,
                                     item->header_title, item->header_summary, width);
        free(image_html);
        free(thumb);
    } else if (link && label_html) {
        // Text-only browsers and WML: no picture.
        result = render_template(item_tpl, link, label_html, item->header_title, item->header_summary);
    }

    free(label_html);
    free(link);
    return result;
}

char *related_posts_render(const char *extra_data, const RelatedPostsContext *ctx, int epoch) {
    char ids[RELATED_POSTS_MAX][RELATED_ID_BUF_SIZE];
    size_t chosen = related_ids_parse(extra_data, ctx ? ctx->entry_id : NULL, ids, RELATED_POSTS_MAX);

    CmsBlogListItem *items = NULL;
    size_t count = 0;
    if (chosen > 0) {
        const char *id_ptrs[RELATED_POSTS_MAX] = { ids[0], ids[1] };
        cms_get_blog_entries_by_ids(request_lang(), id_ptrs, chosen, &items, &count);
    } else if (ctx) {
        cms_get_latest_related_entry(request_lang(), ctx->entry_id, ctx->category_ids,
                                     ctx->category_count, &items, &count);
    }
    if (count == 0) return strdup("");

    char *block_tpl = load_template("elements/related-posts/related-posts_epoch%d.html", epoch);
    char *item_tpl  = load_template("elements/related-posts/related-posts-item_epoch%d.html", epoch);
    char *label_tpl = load_template("elements/related-posts/related-posts-label_epoch%d.html", epoch);
    char *image_tpl = epoch >= EPOCH_EARLY
        ? load_template("elements/related-posts/related-posts-image_epoch%d.html", epoch) : NULL;

    char *rendered = NULL;
    char *result = NULL;
    if (block_tpl && item_tpl && label_tpl && (epoch < EPOCH_EARLY || image_tpl)) {
        // The labels say left is before and right is after: only a pair has both.
        int labelled = chosen == 2 && count == 2;
        rendered = strdup("");
        for (size_t i = 0; rendered && i < count; i++) {
            const char *label = labelled ? pair_label((int)i) : NULL;
            char *item = render_item(&items[i], item_tpl, image_tpl, label_tpl, label,
                                     count > 1 ? "50%" : "100%", epoch);
            rendered = item ? str_append(rendered, item) : (free(rendered), NULL);
            free(item);
        }
        if (rendered)
            result = render_template(block_tpl, rendered,
                                     count > 1 ? "" : " boat-rudder-related-posts--single");
    }

    free(rendered);
    free(block_tpl);
    free(item_tpl);
    free(label_tpl);
    free(image_tpl);
    cms_blog_list_free(items, count);
    return result;
}

char *related_posts_lookup_json(const char *query, char *ids_csv, const char *exclude_id) {
    CmsBlogListItem *items = NULL;
    size_t count = 0;

    if (ids_csv && ids_csv[0]) {
        const char *ids[ENTRY_INDEX_MAX];
        size_t id_count = related_ids_split_csv(ids_csv, ids, ENTRY_INDEX_MAX);
        cms_get_blog_entries_by_ids(request_lang(), ids, id_count, &items, &count);
    } else {
        cms_find_blog_entries_by_title(query, request_lang(), exclude_id, RELATED_LOOKUP_LIMIT,
                                       &items, &count);
    }

    char *json = strdup("{\"ok\":true,\"items\":[");
    for (size_t i = 0; json && i < count; i++) {
        char *title = json_escape_alloc(items[i].header_title ? items[i].header_title : "");
        char *row = title ? render_template("%s{\"id\":\"%s\",\"title\":\"%s\",\"date\":\"%s\"}",
                                            i ? "," : "", items[i].id, title, items[i].header_date)
                          : NULL;
        json = row ? str_append(json, row) : (free(json), NULL);
        free(row);
        free(title);
    }
    if (json) json = str_append(json, "]}");

    cms_blog_list_free(items, count);
    return json;
}
