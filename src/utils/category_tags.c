#include "category_tags.h"
#include "generate_url_theme.h"
#include "read_file.h"
#include "template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_part(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

char *category_tags_render(char **links, char **names, size_t count, int epoch) {
    if (count == 0 || !names) return strdup("");

    char *item_tpl = load_part("elements/category/category_epoch%d.html", epoch);
    if (!item_tpl) return strdup("");

    // Every epoch links its tags. An anchor is the one thing all of them can
    // do - WML included - so a category tag is clickable wherever it appears.
    int linked = links != NULL;

    char *sep_tpl = load_part("elements/category/category-separator_epoch%d.html", epoch);

    char *result = strdup("");
    for (size_t i = 0; result && i < count; i++) {
        char *tag = linked ? render_template(item_tpl, links[i], names[i])
                           : render_template(item_tpl, names[i]);
        if (sep_tpl && i > 0) result = str_append(result, sep_tpl);
        if (result && tag) result = str_append(result, tag);
        free(tag);
    }

    free(sep_tpl);
    free(item_tpl);
    return result;
}
