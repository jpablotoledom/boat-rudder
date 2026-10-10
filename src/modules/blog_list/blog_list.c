#include "blog_list.h"
#include "../../utils/category_tags.h"
#include "../../db/cms_entries.h"
#include "../../db/cms_themes.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *render_item(const CmsBlogListItem *item, const char *item_tpl, int epoch) {
    char *categories_html = category_tags_render_list(item->category_links, item->category_names,
                                                       item->category_count, epoch);
    if (!categories_html) return NULL;

    char *result;
    if (epoch >= EPOCH_EARLY) {
        char *link_url = render_template("/blog/%s", item->link);
        // Epoch 1 predates progressive JPEG, so it gets the GIF the optimizer
        // writes - same variant and rewrite as image_for_epoch()/gallery_thumb().
        // Epoch 2 gets the same lighter `_micro` GIF now instead of `_small`
        // (a JPEG, and the biggest file of the two by far) - real epoch-2
        // browsers (Netscape 4 and up, over what was often a slow link) pay
        // for up to HOME_BLOG_LIMIT of these on the home page alone, and a
        // small blog-list thumbnail doesn't need `_small`'s extra detail to
        // read as a thumbnail. Epoch 3 shows the thumbnail as a CSS
        // background that stretches to the card's full width on narrow
        // screens, so it alone still gets `_half`.
        char *thumb = (epoch < EPOCH_MODERN)
            ? image_url_variant(item->header_image_url, "_micro")
            : image_url_variant(item->header_image_url, "_half");
        if (epoch < EPOCH_MODERN && thumb) {
            char *dot = strrchr(thumb, '.');
            if (dot) strcpy(dot, ".gif");
        }

        if (epoch == EPOCH_MODERN) {
            // home-blog-item_epoch3.html carries its own color via CSS
            // classes (boat-rudder-home-blog-item__byline-owner etc.) -
            // no color arguments to insert. Unlike epoch 2's item template,
            // this one addresses its %s slots sequentially, not by %N$s
            // position, so passing the epoch 1/2 color arguments here would
            // shift every slot after them - not merely "extra, ignored"
            // varargs - and print a hex string where the author name goes.
            result = (link_url && thumb)
                ? render_template(item_tpl, thumb, link_url, item->header_title,
                                   item->header_summary,
                                   item->header_hide_author ? "" : item->header_author,
                                   categories_html, item->header_date)
                : NULL;
        } else {
            // Epoch 1/2 have no CSS custom properties (epoch 1: no CSS at
            // all; epoch 2's inline <style> predates CSS3 variables), so
            // the theme's colors go straight into <font color>/bordercolor
            // attributes as %s substitutions instead. See
            // develop_docs/plans/theme-system-plan.md's epoch 1/2 analysis.
            CmsThemeColors retro;
            cms_get_theme_colors(request_theme(), &retro);

            // blog_list_item_background may carry an alpha byte for epoch
            // 3's CSS background-color; a plain HTML bgcolor attribute has
            // no notion of one (see cms_themes.h's own doc comment on that
            // field, and the table block's identical fix), so this strips
            // it down to the opaque "#rrggbb" before using it as one -
            // replacing the item card's own background GIF, which Netscape
            // 4 redraws slowly and which repeated once per item (up to
            // HOME_BLOG_LIMIT of them) on the home page compounded that.
            char item_bg[8];
            int unused_alpha;
            cms_split_hex_alpha(retro.blog_list_item_background, item_bg, &unused_alpha);

            result = (link_url && thumb)
                ? render_template(item_tpl, thumb, link_url, item->header_title,
                                   item->header_summary,
                                   retro.blog_list_item_author,
                                   item->header_hide_author ? "" : item->header_author,
                                   categories_html,
                                   retro.blog_list_item_date, item->header_date,
                                   retro.blog_list_item_border, item_bg)
                : NULL;
        }
        free(thumb);
        free(link_url);
    } else {
        // Text-only and WML, like render_image()'s epoch <= EPOCH_PRESTANDARD
        // branch: no thumbnail, but still a real link to the article and a
        // byline - a title with nothing to click and no author was just a
        // template that had never been given the arguments to draw them,
        // not a deliberate restriction for either epoch.
        char *link_url = render_template("/blog/%s", item->link);
        result = link_url
            ? render_template(item_tpl, link_url, item->header_title,
                               item->header_summary,
                               item->header_hide_author ? "" : item->header_author,
                               categories_html, item->header_date)
            : NULL;
        free(link_url);
    }

    free(categories_html);
    return result;
}

// Epoch 2's home blog list and each of its items are a <table> (the theme's
// home-blog_epoch2.html / home-blog-item_epoch2.html). A background image
// set in the theme customizer goes into the first table's background="..."
// - replacing the template's own value, or added to it when it has none -
// so a theme without the setting, or an updated template, renders exactly
// as shipped. Takes ownership of `html`; NULL only on allocation failure.
static char *set_table_background(char *html, const char *url) {
    static const char ATTR[] = " background=\"";
    char *value = strstr(html, ATTR);
    char *end = value ? strchr(value + sizeof(ATTR) - 1, '"') : NULL;
    char *insert_at;
    const char *piece;
    char *attr = NULL;

    if (end) {
        insert_at = value + sizeof(ATTR) - 1;   // replace [insert_at, end)
        piece = url;
    } else {
        char *table = strstr(html, "<table");
        if (!table) return html;
        insert_at = end = table + 6;            // insert right after "<table"
        attr = render_template(" background=\"%s\"", url);
        if (!attr) return html;
        piece = attr;
    }

    size_t head = (size_t)(insert_at - html), piece_len = strlen(piece), tail = strlen(end);
    char *out = malloc(head + piece_len + tail + 1);
    if (out) {
        memcpy(out, html, head);
        memcpy(out + head, piece, piece_len);
        memcpy(out + head + piece_len, end, tail + 1);
    }
    free(attr);
    free(html);
    return out;
}

// The URL of an epoch 2 home blog image (`file`, as stored), NULL when unset.
static char *epoch2_image_url(char *file) {
    char *url = file && file[0]
        ? render_template("/content/themes/%s/home-blog/epoch2/%s", request_theme(), file)
        : NULL;
    free(file);
    return url;
}

// The one implementation. `heading` is printed above the list; `limit` caps
// the query; `category_id_hex` filters it when non-NULL.
static char *render_list(int epoch, const char *lang, const char *heading,
                          int limit, const char *category_id_hex) {
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

    if (category_id_hex)
        cms_get_blog_entries_by_category(lang, limit, category_id_hex, &entries, &entry_count);
    else
        cms_get_blog_entries(lang, limit, &entries, &entry_count);

    // No stylesheet on epoch 1, so both the heading and the "no entries"
    // message's text color have to be a real attribute - were hardcoded
    // white, unreadable on light-background themes; now take the theme's
    // own home-content-text color (the "Home content" setting).
    int needs_color = (epoch == EPOCH_EARLY);
    CmsThemeColors colors;
    if (needs_color) cms_get_theme_colors(request_theme(), &colors);

    if (entry_count == 0) {
        char *empty_tpl = load_template("home-blog/empty_epoch%d.html", epoch);
        items = needs_color && empty_tpl ? render_template(empty_tpl, colors.home_content_text)
                                          : empty_tpl;
        if (needs_color) free(empty_tpl);
    } else {
        char *item_bg = epoch == EPOCH_MIDDLE
            ? epoch2_image_url(cms_get_theme_home_blog_item_background(request_theme(), EPOCH_MIDDLE))
            : NULL;
        items = strdup("");
        for (size_t i = 0; items && i < entry_count; i++) {
            char *item = render_item(&entries[i], item_tpl, epoch);
            if (item && item_bg) item = set_table_background(item, item_bg);
            items = item ? str_append(items, item) : NULL;
            free(item);
        }
        free(item_bg);
    }

    if (items) {
        result = needs_color
            ? render_template(content_tpl, colors.home_content_text, heading, items)
            : render_template(content_tpl, heading, items);
    }
    if (result && epoch == EPOCH_MIDDLE) {
        char *list_bg = epoch2_image_url(cms_get_theme_home_blog_background(request_theme(), EPOCH_MIDDLE));
        if (list_bg) result = set_table_background(result, list_bg);
        free(list_bg);
    }

cleanup:
    cms_blog_list_free(entries, entry_count);
    free(item_tpl);
    free(content_tpl);
    free(items);
    return result;
}

char *home_blog(int epoch, const char *lang) {
    return render_list(epoch, lang, "Latest Blog Posts", HOME_BLOG_LIMIT, NULL);
}

char *blog_list(int epoch, const char *lang) {
    return render_list(epoch, lang, "Blog", BLOG_LIST_LIMIT, NULL);
}

char *blog_list_category(int epoch, const char *lang, const char *category_id_hex) {
    return render_list(epoch, lang, "Blog", BLOG_LIST_LIMIT, category_id_hex);
}
