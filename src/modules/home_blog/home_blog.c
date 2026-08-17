#include "home_blog.h"
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

// Renders item->category_names as concatenated category tags, without the
// entry-categories wrapper - the placeholder already sits inside the item's
// own byline container. Returns "" if the item has no categories.
static char *render_item_categories(const CmsBlogListItem *item, int epoch) {
    if (item->category_count == 0) return strdup("");

    char *item_tpl = load_template("elements/category/category_epoch%d.html", epoch);
    if (!item_tpl) return strdup("");

    char *result = strdup("");
    for (size_t i = 0; result && i < item->category_count; i++) {
        char *tag = (epoch >= 3)
            ? render_template(item_tpl, item->category_links[i], item->category_names[i])
            : render_template(item_tpl, item->category_names[i]);
        result = tag ? str_append(result, tag) : NULL;
        free(tag);
    }

    free(item_tpl);
    return result;
}

static char *render_item(const CmsBlogListItem *item, const char *item_tpl, int epoch) {
    char *categories_html = render_item_categories(item, epoch);
    if (!categories_html) return NULL;

    char *result;
    if (epoch >= 1) {
        char *link_url = render_template("/blog/%s", item->link);
        char *thumb = image_url_variant(item->header_image_url, "_small");
        result = (link_url && thumb)
            ? render_template(item_tpl, thumb, link_url, item->header_title,
                               item->header_summary,
                               item->header_hide_author ? "" : item->header_author,
                               categories_html, item->header_date)
            : NULL;
        free(thumb);
        free(link_url);
    } else {
        result = render_template(item_tpl, item->header_title, item->header_date,
                                  item->header_summary, categories_html);
    }

    free(categories_html);
    return result;
}

char *home_blog(int epoch, const char *lang) {
    char *item_path    = generate_url_theme("home-blog/home-blog-item_epoch%d.html", epoch);
    char *content_path = generate_url_theme("home-blog/home-blog_epoch%d.html", epoch);

    char *item_tpl    = item_path    ? read_file_to_string(item_path)    : NULL;
    char *content_tpl = content_path ? read_file_to_string(content_path) : NULL;

    free(item_path);
    free(content_path);

    char *items  = NULL;
    char *result = NULL;

    CmsBlogListItem *entries     = NULL;
    size_t           entry_count = 0;

    if (!item_tpl || !content_tpl) goto cleanup;

    cms_get_blog_entries(lang, HOME_BLOG_LIMIT, &entries, &entry_count);

    if (entry_count == 0) {
        items = load_template("home-blog/empty_epoch%d.html", epoch);
    } else {
        items = strdup("");
        for (size_t i = 0; items && i < entry_count; i++) {
            char *item = render_item(&entries[i], item_tpl, epoch);
            items = item ? str_append(items, item) : NULL;
            free(item);
        }
    }

    if (items) result = render_template(content_tpl, items);

cleanup:
    cms_blog_list_free(entries, entry_count);
    free(item_tpl);
    free(content_tpl);
    free(items);
    return result;
}
