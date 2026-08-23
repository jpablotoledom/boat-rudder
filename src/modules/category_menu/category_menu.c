#include "category_menu.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

char *category_menu_render(const CmsCategoryItem *categories, size_t count,
                            const char *current_slug, int epoch) {
    if (count == 0) return strdup("");

    char *path = generate_url_theme("category-menu/category-menu_epoch%d.html", epoch);
    char *container_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    path = generate_url_theme("category-menu/category-menu-item_epoch%d.html", epoch);
    char *item_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    path = generate_url_theme("category-menu/category-menu-item-selected_epoch%d.html", epoch);
    char *selected_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    // What goes *between* two items, never before the first or after the last.
    // Epochs that separate the items by layout - a row of <td> in epoch 1, a
    // flex container in epoch 3 - hold nothing but whitespace here.
    path = generate_url_theme("category-menu/category-menu-separator_epoch%d.html", epoch);
    char *separator_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    if (!container_tpl || !item_tpl) {
        free(container_tpl); free(item_tpl); free(selected_tpl); free(separator_tpl);
        return strdup("");
    }
    if (!selected_tpl) selected_tpl = strdup(item_tpl);

    char *items = strdup("");
    for (size_t i = 0; items && i < count; i++) {
        char *slug = slugify(categories[i].name);
        int is_selected = (current_slug && slug && strcmp(slug, current_slug) == 0);
        const char *tpl = is_selected ? selected_tpl : item_tpl;
        char *item = render_template(tpl, slug ? slug : "", categories[i].name);
        free(slug);
        if (separator_tpl && i > 0) items = str_append(items, separator_tpl);
        if (items && item) items = str_append(items, item);
        free(item);
    }

    char *result = items ? render_template(container_tpl, items) : NULL;
    free(items);
    free(container_tpl); free(item_tpl); free(selected_tpl); free(separator_tpl);
    return result ? result : strdup("");
}
