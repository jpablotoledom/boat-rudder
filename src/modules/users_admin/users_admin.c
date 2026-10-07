#include "users_admin.h"
#include "../../db/cms_users_admin.h"
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

static const char *role_label(const char *role) {
    return strcmp(role, "admin") == 0 ? "Administrador" : "Autor";
}

char *users_admin_list(int epoch, const char *error_message) {
    char *row_tpl  = load_template("dashboard/users/list-row_epoch%d.html", epoch);
    char *list_tpl = load_template("dashboard/users/list_epoch%d.html", epoch);

    char *rows       = NULL;
    char *error_html = NULL;
    char *result     = NULL;

    CmsUserAdminItem *items = NULL;
    size_t item_count = 0;

    if (!row_tpl || !list_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/users/list-error_epoch%d.html", epoch);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    cms_get_users(&items, &item_count);

    if (item_count == 0) {
        rows = load_template("dashboard/users/list-empty_epoch%d.html", epoch);
    } else {
        rows = strdup("");
        for (size_t i = 0; rows && i < item_count; i++) {
            char *row = render_template(row_tpl, items[i].email, role_label(items[i].role),
                                          items[i].id, items[i].id);
            rows = row ? str_append(rows, row) : NULL;
            free(row);
        }
    }

    if (rows) result = render_template(list_tpl, error_html ? error_html : "", rows);

cleanup:
    cms_users_admin_free(items, item_count);
    free(row_tpl);
    free(list_tpl);
    free(rows);
    free(error_html);
    return result;
}

char *users_admin_form(int epoch, const char *id, const char *name, const char *email,
                        const char *role, const char *error_message) {
    char *form_tpl = load_template("dashboard/users/form_epoch%d.html", epoch);

    char *error_html = NULL;
    char *action      = NULL;
    char *result      = NULL;

    if (!form_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/users/form-error_epoch%d.html", epoch);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    action = (id && id[0])
        ? render_template("/dashboard/users/%s/edit", id)
        : strdup("/dashboard/users/new");
    if (!action) goto cleanup;

    const char *password_label = (id && id[0])
        ? "Nueva contrasena (dejar en blanco para no cambiar)" : "Password";

    result = render_template(form_tpl, error_html ? error_html : "", action, name, email,
                              password_label,
                              strcmp(role, "admin") == 0 ? " selected" : "",
                              strcmp(role, "author") == 0 ? " selected" : "");

cleanup:
    free(form_tpl);
    free(error_html);
    free(action);
    return result;
}
