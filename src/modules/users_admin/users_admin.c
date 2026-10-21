#include "users_admin.h"
#include "../../db/cms_users_admin.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
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

// html_encode() needs a caller-sized buffer; worst case every byte of `src`
// expands to "&quot;" (6 bytes), so 6x + 1 is always enough.
static char *html_encode_alloc(const char *src) {
    if (!src) src = "";
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    html_encode(out, src, cap);
    return out;
}

static const char *role_label(const char *role) {
    if (strcmp(role, "admin") == 0) return "Administrator";
    if (strcmp(role, "viewer") == 0) return "Viewer";
    return "Author";
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
            char *username = html_encode_alloc(items[i].username);
            char *email    = html_encode_alloc(items[i].email);
            char *row = username && email
                ? render_template(row_tpl, username, email, role_label(items[i].role),
                                  items[i].id, items[i].id)
                : NULL;
            free(username);
            free(email);
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

char *users_admin_form(int epoch, const char *id, const char *name, const char *username,
                        const char *email, const char *role, const char *error_message) {
    char *form_tpl = load_template("dashboard/users/form_epoch%d.html", epoch);

    char *error_html = NULL;
    char *action      = NULL;
    char *result      = NULL;
    char *enc_name     = html_encode_alloc(name);
    char *enc_username = html_encode_alloc(username);
    char *enc_email    = html_encode_alloc(email);

    if (!form_tpl || !enc_name || !enc_username || !enc_email) goto cleanup;

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
        ? "New password (leave blank to keep the current one)" : "Password";

    // The username names the user's media folder, so it is fixed once the
    // account exists: read-only on edit (and cms_update_user() ignores it).
    int editing = id && id[0];
    result = render_template(form_tpl, error_html ? error_html : "", action, enc_name,
                              enc_username, editing ? " readonly" : "",
                              editing ? "Fixed: it names this user's media folder."
                                      : "3-32 characters: a-z, 0-9, dot, dash, underscore. "
                                        "It names the user's media folder and cannot be changed later.",
                              enc_email, password_label,
                              strcmp(role, "admin") == 0 ? " selected" : "",
                              strcmp(role, "author") == 0 ? " selected" : "",
                              strcmp(role, "viewer") == 0 ? " selected" : "");

cleanup:
    free(form_tpl);
    free(error_html);
    free(action);
    free(enc_name);
    free(enc_username);
    free(enc_email);
    return result;
}
