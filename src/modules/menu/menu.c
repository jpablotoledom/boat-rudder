#include "menu.h"
#include "../../db/cms_menu.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/config_loader.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// Used when the `menu` collection is empty, unreachable, or mongodb is not
// ready, so the nav bar is never empty.
static const CmsMenuItem FALLBACK_ITEMS[] = {
    {.link = "/", .name = "Home"},
};

#define FALLBACK_ITEM_COUNT (sizeof(FALLBACK_ITEMS) / sizeof(FALLBACK_ITEMS[0]))

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

    CmsMenuItem *db_items = NULL;
    size_t db_count = 0;
    if (mongodb_manager_is_ready()) cms_get_menu_items(lang, &db_items, &db_count);

    const CmsMenuItem *menu_items = db_count > 0 ? db_items : FALLBACK_ITEMS;
    size_t item_count = db_count > 0 ? db_count : FALLBACK_ITEM_COUNT;

    items = strdup("");
    if (!items) {
        cms_menu_free(db_items, db_count);
        goto cleanup;
    }

    for (size_t i = 0; i < item_count; i++) {
        const char *sep = (i + 1 < item_count) ? separator : "";

        char *item = render_template(menu_item_tpl, menu_items[i].link, menu_items[i].name, sep);
        if (!item) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }

        items = str_append(items, item);
        free(item);
        if (!items) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }
    }

    cms_menu_free(db_items, db_count);

    result = render_template(menu_tpl, items);

cleanup:
    free(menu_item_tpl);
    free(separator);
    free(menu_tpl);
    free(items);
    return result;
}
