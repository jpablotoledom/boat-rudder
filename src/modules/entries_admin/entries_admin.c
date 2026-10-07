#include "entries_admin.h"
#include "../../utils/category_tags.h"
#include "../../db/cms_entries.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static const char *type_label(const char *type) {
    if (strcmp(type, "page") == 0) return "Page";
    if (strcmp(type, "blog") == 0) return "Blog";
    return type;
}

// `delete_tpl` is NULL when the viewer may not delete entries (an Autor):
// the row then shows no Delete button at all.
static char *render_row(const CmsBlogListItem *item, const char *row_tpl,
                         const char *draft_badge, const char *delete_tpl, int epoch) {
    char *categories = category_tags_render(item->category_links, item->category_names,
                                             item->category_count, epoch);
    if (!categories) return NULL;

    const char *link_prefix = strcmp(item->type, "blog") == 0 ? "blog" : "page";
    char *link_url = render_template("/%s/%s", link_prefix, item->link);

    char *thumb = image_url_variant(item->header_image_url, "_small");
    char *delete_html = delete_tpl ? render_template(delete_tpl, item->id) : strdup("");
    char *result = (link_url && thumb && delete_html)
        ? render_template(row_tpl, thumb, item->id, item->header_title,
                           item->enabled ? "" : draft_badge, type_label(item->type), item->header_summary, item->header_author,
                           item->header_date, categories, item->id, link_url, delete_html)
        : NULL;

    free(delete_html);
    free(thumb);
    free(link_url);
    free(categories);
    return result;
}

char *entries_admin_rows(int epoch, const char *lang, const char *type_filter,
                          const char *created_by_hex, bool unpublished_only, bool can_delete) {
    char *row_tpl     = load_template("dashboard/entries/list-row_epoch%d.html", epoch);
    char *draft_badge = load_template("dashboard/entries/list-draft_epoch%d.html", epoch);
    char *delete_tpl  = can_delete ? load_template("dashboard/entries/list-row-delete_epoch%d.html", epoch)
                                   : NULL;

    char *rows = NULL;

    CmsBlogListItem *items = NULL;
    size_t item_count = 0;

    if (!row_tpl || !draft_badge || (can_delete && !delete_tpl)) goto cleanup;

    cms_get_admin_entries(lang, type_filter, created_by_hex, unpublished_only,
                          &items, &item_count);

    if (item_count == 0) {
        char *empty_tpl = load_template("dashboard/entries/list-empty_epoch%d.html", epoch);
        rows = empty_tpl ? render_template(empty_tpl, unpublished_only
                                               ? "No entries pending publication"
                                               : "No entries found")
                         : NULL;
        free(empty_tpl);
    } else {
        rows = strdup("");
        for (size_t i = 0; rows && i < item_count; i++) {
            char *row = render_row(&items[i], row_tpl, draft_badge, delete_tpl, epoch);
            rows = row ? str_append(rows, row) : NULL;
            free(row);
        }
    }

cleanup:
    cms_blog_list_free(items, item_count);
    free(row_tpl);
    free(draft_badge);
    free(delete_tpl);
    return rows;
}
