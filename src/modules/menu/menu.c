#include "menu.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *link;
    const char *label;
} menu_route_t;

// MVP: a single "Home" entry. Add more routes here as new pages are built;
// the templates already iterate over this list with separators.
static const menu_route_t MENU_ROUTES[] = {
    {"/", "Home"},
};

#define MENU_ROUTE_COUNT (sizeof(MENU_ROUTES) / sizeof(MENU_ROUTES[0]))

char *menu(const char *current_url, int epoch) {
    (void)current_url;

    char *menu_item_path = generate_url_theme("menu/menu-item_epoch%d.html", epoch);
    char *separator_path = generate_url_theme("menu/menu-item-separator_epoch%d.html", epoch);
    char *menu_path      = generate_url_theme("menu/menu_epoch%d.html", epoch);

    char *menu_item_tpl = menu_item_path ? read_file_to_string(menu_item_path) : NULL;
    char *separator     = separator_path ? read_file_to_string(separator_path) : NULL;
    char *menu_tpl      = menu_path      ? read_file_to_string(menu_path)      : NULL;

    free(menu_item_path);
    free(separator_path);
    free(menu_path);

    char *items  = NULL;
    char *result = NULL;

    if (!menu_item_tpl || !separator || !menu_tpl) goto cleanup;

    items = strdup("");
    if (!items) goto cleanup;

    for (size_t i = 0; i < MENU_ROUTE_COUNT; i++) {
        const char *sep = (i + 1 < MENU_ROUTE_COUNT) ? separator : "";

        char *item = render_template(menu_item_tpl, MENU_ROUTES[i].link, MENU_ROUTES[i].label, sep);
        if (!item) goto cleanup;

        items = str_append(items, item);
        free(item);
        if (!items) goto cleanup;
    }

    result = render_template(menu_tpl, items);

cleanup:
    free(menu_item_tpl);
    free(separator);
    free(menu_tpl);
    free(items);
    return result;
}
