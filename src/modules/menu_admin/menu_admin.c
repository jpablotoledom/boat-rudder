#include "menu_admin.h"
#include "../../db/cms_menu.h"
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

char *menu_admin_list(int epoch, const char *lang) {
    char *row_tpl  = load_template("dashboard/menu/list-row_epoch%d.html", epoch);
    char *list_tpl = load_template("dashboard/menu/list_epoch%d.html", epoch);

    char *rows   = NULL;
    char *result = NULL;

    CmsMenuAdminItem *items = NULL;
    size_t item_count = 0;

    if (!row_tpl || !list_tpl) goto cleanup;

    cms_get_menu_admin_items(lang, &items, &item_count);

    if (item_count == 0) {
        rows = load_template("dashboard/menu/list-empty_epoch%d.html", epoch);
    } else {
        rows = strdup("");
        for (size_t i = 0; rows && i < item_count; i++) {
            char *row = render_template(row_tpl, items[i].name, items[i].link, items[i].order,
                                          items[i].enabled ? "Yes" : "No", items[i].id, items[i].id);
            rows = row ? str_append(rows, row) : NULL;
            free(row);
        }
    }

    if (rows) result = render_template(list_tpl, rows);

cleanup:
    cms_menu_admin_free(items, item_count);
    free(row_tpl);
    free(list_tpl);
    free(rows);
    return result;
}

char *menu_admin_form(int epoch, const char *id, const char *link, int order, bool enabled,
                       const CmsLanguageItem *langs, size_t lang_count, char *const *values,
                       const char *error_message) {
    char *field_tpl = load_template("dashboard/menu/form-field_epoch%d.html", epoch);
    char *form_tpl  = load_template("dashboard/menu/form_epoch%d.html", epoch);

    char *fields     = NULL;
    char *error_html = NULL;
    char *action     = NULL;
    char *result     = NULL;

    if (!field_tpl || !form_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/menu/form-error_epoch%d.html", epoch);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    fields = strdup("");
    for (size_t i = 0; fields && i < lang_count; i++) {
        char *field = render_template(field_tpl, langs[i].code, langs[i].name,
                                        langs[i].code, langs[i].code, values[i]);
        fields = field ? str_append(fields, field) : NULL;
        free(field);
    }
    if (!fields) goto cleanup;

    action = (id && id[0])
        ? render_template("/dashboard/menu/%s/edit", id)
        : strdup("/dashboard/menu/new");
    if (!action) goto cleanup;

    result = render_template(form_tpl, error_html ? error_html : "", action, link, order,
                              enabled ? " checked" : "", fields);

cleanup:
    free(field_tpl);
    free(form_tpl);
    free(fields);
    free(error_html);
    free(action);
    return result;
}
