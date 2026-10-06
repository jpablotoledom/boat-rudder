#include "category_tags.h"
#include "detect_epoch.h"
#include "generate_url_theme.h"
#include "read_file.h"
#include "request_theme.h"
#include "template_utils.h"
#include "../db/cms_themes.h"
#include <stdlib.h>
#include <string.h>

static char *load_part(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *render_tags(char *item_tpl, char **links, char **names, size_t count, int epoch) {
    if (!item_tpl) return strdup("");

    // Every epoch links its tags - an anchor is the one thing all of them can
    // do, WML included - except where category_tags_render_list() picked a
    // name-only template and passed no links.
    int linked = links != NULL;

    char *sep_tpl = load_part("elements/category/category-separator_epoch%d.html", epoch);

    // Epoch 1 has no CSS custom properties at all (see develop_docs/plans/
    // theme-system-plan.md's epoch 1/2 analysis), so its category tag color
    // becomes a %s substituted straight into a <font color> attribute
    // instead. Epoch 2 does have CSS1, but real-browser testing (IE5/
    // Windows 3.11, the same engine that forced the navbar's own menu items
    // off inline color entirely - see menu.c's long comment on that) showed
    // it won't let an inline `style="color:..."` be overridden on :hover at
    // all, `!important` included - a category tag colored that way would
    // silently never show a hover color there. Category tags all share one
    // theme-wide color anyway (unlike menu items, which differ selected vs
    // not), so epoch 2 gets no per-tag color argument here either - just an
    // id (see category_epoch2.html) that layout_epoch2.html's own <style>
    // block colors, resting and :hover both, the same way it already colors
    // #boat-rudder-navbar-lang-link/-theme-link. Epoch 3 already carries its
    // own color via the boat-rudder__entry-category CSS class; -1/0 have no
    // color model.
    int needs_color = (epoch == EPOCH_EARLY);
    CmsThemeColors retro;
    if (needs_color) cms_get_theme_colors(request_theme(), &retro);

    char *result = strdup("");
    for (size_t i = 0; result && i < count; i++) {
        char *tag;
        if (needs_color)
            tag = linked ? render_template(item_tpl, links[i], retro.blog_list_item_categories, names[i])
                         : render_template(item_tpl, retro.blog_list_item_categories, names[i]);
        else
            tag = linked ? render_template(item_tpl, links[i], names[i])
                         : render_template(item_tpl, names[i]);
        if (sep_tpl && i > 0) result = str_append(result, sep_tpl);
        if (result && tag) result = str_append(result, tag);
        free(tag);
    }

    free(sep_tpl);
    free(item_tpl);
    return result;
}

char *category_tags_render(char **links, char **names, size_t count, int epoch) {
    if (count == 0 || !names) return strdup("");
    return render_tags(load_part("elements/category/category_epoch%d.html", epoch),
                       links, names, count, epoch);
}

char *category_tags_render_list(char **links, char **names, size_t count, int epoch) {
    if (count == 0 || !names) return strdup("");
    // A category-list template takes the name alone (no link) - WML's blog
    // cards drop the anchors to leave the ~860-byte screen to the summary.
    // Epochs without one show the same linked tags as the article page.
    char *list_tpl = load_part("elements/category/category-list_epoch%d.html", epoch);
    if (list_tpl) return render_tags(list_tpl, NULL, names, count, epoch);
    return category_tags_render(links, names, count, epoch);
}
