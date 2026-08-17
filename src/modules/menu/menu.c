#include "menu.h"
#include "../../db/cms_languages.h"
#include "../../db/language_catalog.h"
#include "../../db/cms_menu.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_lang.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// Used when the `menu` collection is empty, unreachable, or mongodb is not
// ready, so the nav bar is never empty.
static const CmsMenuItem FALLBACK_ITEMS[] = {
    {.link = "/", .name = "Home"},
};

#define FALLBACK_ITEM_COUNT (sizeof(FALLBACK_ITEMS) / sizeof(FALLBACK_ITEMS[0]))

// Builds the language selector appended to the nav bar. Epoch 3 renders a
// drop-down whose entries link straight to /language/set; older epochs, where
// a drop-down cannot be relied on, link to the /language page instead. Both
// paths end at the same place - a plain GET that sets the cookie and bounces
// back - so switching language never needs JavaScript.
//
// Returns a malloc'd string ("" when there is nothing to offer: fewer than two
// languages, or the templates are missing), never NULL unless allocation fails.
static char *language_selector(int epoch) {
    CmsLanguageItem *langs = NULL;
    size_t count = 0;
    if (mongodb_manager_is_ready()) cms_get_languages(&langs, &count);

    // A single language offers no choice; showing the control would be noise.
    if (count < 2) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    // The button is tight on space, so it shows the short form ("Esp"); the
    // list has room for the language's own name ("Espanol"), which is what a
    // reader looking for their language actually scans for.
    const char *active = request_lang();
    const char *active_abbr = language_catalog_abbr(active);

    // Come back to the page actually being read. `current_url` is the menu's
    // section (always "/blog" for an article), which would drop the reader on
    // the listing instead.
    char return_enc[1024];
    url_encode(return_enc, request_path(), sizeof(return_enc));

    char *tpl_path = generate_url_theme("menu/menu-lang_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    char *result = NULL;
    if (epoch >= EPOCH_MODERN) {
        char *item_path = generate_url_theme("menu/menu-lang-item_epoch%d.html", epoch);
        char *item_tpl = item_path ? read_file_to_string(item_path) : NULL;
        free(item_path);

        if (item_tpl) {
            char *items = strdup("");
            for (size_t i = 0; items && i < count; i++) {
                if (!langs[i].code) continue;
                const char *is_active = strcmp(langs[i].code, active) == 0
                    ? " boat-rudder__navbar__lang__item--active" : "";
                char *item = render_template(item_tpl, is_active, langs[i].code, return_enc,
                                              language_catalog_native(langs[i].code));
                items = item ? str_append(items, item) : NULL;
                free(item);
            }
            if (items) result = render_template(tpl, active_abbr, items);
            free(items);
            free(item_tpl);
        }
    } else {
        result = render_template(tpl, return_enc, active_abbr);
    }

    free(tpl);
    cms_languages_free(langs, count);
    return result ? result : strdup("");
}

char *menu(const char *current_url, int epoch) {
    char *menu_item_path    = generate_url_theme("menu/menu-item_epoch%d.html", epoch);
    char *selected_item_path = generate_url_theme("menu/menu-item-selected_epoch%d.html", epoch);
    char *separator_path    = generate_url_theme("menu/menu-item-separator_epoch%d.html", epoch);
    char *menu_path         = generate_url_theme("menu/menu_epoch%d.html", epoch);

    char *menu_item_tpl     = menu_item_path     ? read_file_to_string(menu_item_path)     : NULL;
    char *selected_item_tpl = selected_item_path ? read_file_to_string(selected_item_path) : NULL;
    char *separator         = separator_path     ? read_file_to_string(separator_path)     : NULL;
    char *menu_tpl          = menu_path          ? read_file_to_string(menu_path)          : NULL;

    free(menu_item_path);
    free(selected_item_path);
    free(separator_path);
    free(menu_path);

    char *items  = NULL;
    char *result = NULL;

    if (!menu_item_tpl || !separator || !menu_tpl) goto cleanup;
    if (!selected_item_tpl) selected_item_tpl = strdup(menu_item_tpl);

    CmsMenuItem *db_items = NULL;
    size_t db_count = 0;
    if (mongodb_manager_is_ready())
        cms_get_menu_items(request_lang(), &db_items, &db_count);

    const CmsMenuItem *menu_items = db_count > 0 ? db_items : FALLBACK_ITEMS;
    size_t item_count = db_count > 0 ? db_count : FALLBACK_ITEM_COUNT;

    items = strdup("");
    if (!items) {
        cms_menu_free(db_items, db_count);
        goto cleanup;
    }

    for (size_t i = 0; i < item_count; i++) {
        const char *sep = (i + 1 < item_count) ? separator : "";

        int is_selected = current_url && strcmp(current_url, menu_items[i].link) == 0;
        const char *tpl = is_selected ? selected_item_tpl : menu_item_tpl;
        char *item = render_template(tpl, menu_items[i].link, menu_items[i].name, sep);
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

    char *lang_html = language_selector(epoch);
    result = lang_html ? render_template(menu_tpl, items, lang_html) : NULL;
    free(lang_html);

cleanup:
    free(menu_item_tpl);
    free(selected_item_tpl);
    free(separator);
    free(menu_tpl);
    free(items);
    return result;
}
