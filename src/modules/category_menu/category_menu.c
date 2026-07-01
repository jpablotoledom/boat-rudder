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

    if (!container_tpl || !item_tpl) {
        free(container_tpl); free(item_tpl); free(selected_tpl);
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
        if (item) items = str_append(items, item);
        free(item);
    }

    char *result = items ? render_template(container_tpl, items) : NULL;
    free(items);
    free(container_tpl); free(item_tpl); free(selected_tpl);
    return result ? result : strdup("");
}
