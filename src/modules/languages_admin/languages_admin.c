#include "languages_admin.h"
#include "../../db/cms_languages.h"
#include "../../db/language_catalog.h"
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

static int is_active(const CmsLanguageItem *items, size_t item_count, const char *code) {
    for (size_t i = 0; i < item_count; i++) {
        if (strcmp(items[i].code, code) == 0) return 1;
    }
    return 0;
}

char *languages_admin(int epoch, const char *error_message) {
    char *list_tpl        = load_template("dashboard/languages/list_epoch%d.html", epoch);
    char *row_tpl         = load_template("dashboard/languages/list-row_epoch%d.html", epoch);
    char *row_actions_tpl = load_template("dashboard/languages/list-row-actions_epoch%d.html", epoch);
    char *option_tpl      = load_template("dashboard/languages/option_epoch%d.html", epoch);

    char *error_html = NULL;
    char *rows       = NULL;
    char *options    = NULL;
    char *result     = NULL;

    CmsLanguageItem *items = NULL;
    size_t item_count = 0;

    if (!list_tpl || !row_tpl || !row_actions_tpl || !option_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/languages/list-error_epoch%d.html", epoch);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    cms_get_languages(&items, &item_count);

    rows = strdup("");
    for (size_t i = 0; rows && i < item_count; i++) {
        char *row = items[i].is_default
            ? render_template(row_tpl, items[i].code, items[i].name)
            : render_template(row_actions_tpl, items[i].code, items[i].name,
                               items[i].code, items[i].code);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) goto cleanup;

    options = strdup("");
    for (size_t i = 0; options && i < LANGUAGE_CATALOG_COUNT; i++) {
        if (is_active(items, item_count, LANGUAGE_CATALOG[i].code)) continue;

        char *option = render_template(option_tpl, LANGUAGE_CATALOG[i].code, LANGUAGE_CATALOG[i].name);
        options = option ? str_append(options, option) : NULL;
        free(option);
    }
    if (!options) goto cleanup;

    result = render_template(list_tpl, error_html ? error_html : "", rows, options);

cleanup:
    cms_languages_free(items, item_count);
    free(list_tpl);
    free(row_tpl);
    free(row_actions_tpl);
    free(option_tpl);
    free(error_html);
    free(rows);
    free(options);
    return result;
}
