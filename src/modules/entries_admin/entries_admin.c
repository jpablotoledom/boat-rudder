#include "entries_admin.h"
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

// Renders item->category_names as concatenated category tags (see
// blog_list.c's render_item_categories()). Returns "" if the item has no
// categories.
static char *render_categories(const CmsBlogListItem *item, int epoch) {
    if (item->category_count == 0) return strdup("");

    char *item_tpl = load_template("elements/category/category_epoch%d.html", epoch);
    if (!item_tpl) return strdup("");

    char *result = strdup("");
    for (size_t i = 0; result && i < item->category_count; i++) {
        char *tag = render_template(item_tpl, item->category_names[i]);
        result = tag ? str_append(result, tag) : NULL;
        free(tag);
    }

    free(item_tpl);
    return result;
}

static char *render_row(const CmsBlogListItem *item, const char *row_tpl, int epoch) {
    char *categories = render_categories(item, epoch);
    if (!categories) return NULL;

    const char *link_prefix = strcmp(item->type, "blog") == 0 ? "blog" : "page";
    char *link_url = render_template("/%s/%s", link_prefix, item->link);

    char *result = link_url
        ? render_template(row_tpl, item->header_image_url, link_url, item->header_title,
                           type_label(item->type), item->header_summary, item->header_author,
                           item->header_date, categories, item->id, item->id)
        : NULL;

    free(link_url);
    free(categories);
    return result;
}

char *entries_admin_rows(int epoch, const char *lang, const char *type_filter,
                          const char *created_by_hex) {
    char *row_tpl = load_template("dashboard/entries/list-row_epoch%d.html", epoch);

    char *rows = NULL;

    CmsBlogListItem *items = NULL;
    size_t item_count = 0;

    if (!row_tpl) goto cleanup;

    cms_get_admin_entries(lang, type_filter, created_by_hex, &items, &item_count);

    if (item_count == 0) {
        rows = load_template("dashboard/entries/list-empty_epoch%d.html", epoch);
    } else {
        rows = strdup("");
        for (size_t i = 0; rows && i < item_count; i++) {
            char *row = render_row(&items[i], row_tpl, epoch);
            rows = row ? str_append(rows, row) : NULL;
            free(row);
        }
    }

cleanup:
    cms_blog_list_free(items, item_count);
    free(row_tpl);
    return rows;
}
