#include "extensions_admin.h"
#include "../../extensions/extensions.h"
#include "../../utils/detect_epoch.h"
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
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    html_encode(out, src, cap);
    return out;
}

static char *status_html(const ExtensionInfo *in) {
    char *problem = html_encode_alloc(in->problem);
    if (!problem) return NULL;
    char *html = NULL;
    switch (in->state) {
    case EXTENSION_RUNNING:
        html = strdup("<span class=\"boat-rudder-badge boat-rudder-badge--accent\">Running</span>");
        break;
    case EXTENSION_STARTING:
        html = strdup("<span class=\"boat-rudder-badge\">Starting</span>");
        break;
    case EXTENSION_DISABLED:
        html = strdup("<span class=\"boat-rudder-badge\">Disabled</span>");
        break;
    case EXTENSION_FAILED:
        html = render_template("<span class=\"boat-rudder-badge\">Failed</span><br>%s", problem);
        break;
    case EXTENSION_INVALID:
        html = render_template("<span class=\"boat-rudder-badge\">Can't be used</span><br>%s", problem);
        break;
    }
    free(problem);
    return html;
}

static char *actions_html(int epoch, const ExtensionInfo *in) {
    if (in->state == EXTENSION_INVALID) return strdup("");

    char *toggle_tpl = load_template("dashboard/extensions/toggle_epoch%d.html", epoch);
    char *open_tpl   = load_template("dashboard/extensions/open_epoch%d.html", epoch);
    char *html = NULL;
    if (toggle_tpl && open_tpl) {
        char *toggle = in->enabled
            ? render_template(toggle_tpl, in->name, "disable", "", "Disable")
            : render_template(toggle_tpl, in->name, "enable", " boat-rudder-btn--primary", "Enable");
        char *open = (in->state == EXTENSION_RUNNING && in->nav_label[0])
            ? render_template(open_tpl, in->name, in->nav_path) : strdup("");
        if (toggle && open) html = render_template("%s%s", open, toggle);
        free(toggle);
        free(open);
    }
    free(toggle_tpl);
    free(open_tpl);
    return html;
}

char *extensions_admin_page(int epoch, const char *error_message) {
    char *list_tpl = load_template("dashboard/extensions/list_epoch%d.html", epoch);
    char *row_tpl  = load_template("dashboard/extensions/list-row_epoch%d.html", epoch);
    char *error_html = NULL, *rows = NULL, *result = NULL;
    ExtensionInfo *list = NULL;
    size_t count = 0;

    if (!list_tpl || !row_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/extensions/list-error_epoch%d.html", epoch);
        char *encoded = html_encode_alloc(error_message);
        if (error_tpl && encoded) error_html = render_template(error_tpl, encoded);
        free(error_tpl);
        free(encoded);
    }

    count = extensions_list(&list);
    if (count == 0) {
        rows = load_template("dashboard/extensions/list-empty_epoch%d.html", epoch);
    } else {
        rows = strdup("");
        for (size_t i = 0; rows && i < count; i++) {
            const ExtensionInfo *in = &list[i];
            char *title  = html_encode_alloc(in->title[0] ? in->title : in->name);
            char *desc   = html_encode_alloc(in->description);
            char *ver    = html_encode_alloc(in->version[0] ? in->version : "-");
            char *status = status_html(in);
            char *acts   = actions_html(epoch, in);
            char *row    = (title && desc && ver && status && acts)
                ? render_template(row_tpl, title, in->name, desc, ver, status, acts) : NULL;
            rows = row ? str_append(rows, row) : (free(rows), NULL);
            free(title); free(desc); free(ver); free(status); free(acts); free(row);
        }
    }
    if (!rows) goto cleanup;

    result = render_template(list_tpl, error_html ? error_html : "", rows);

cleanup:
    free(list_tpl);
    free(row_tpl);
    free(error_html);
    free(rows);
    free(list);
    return result;
}

char *extensions_admin_nav(int epoch) {
    if (epoch != EPOCH_MODERN) return strdup("");

    ExtensionInfo *list = NULL;
    size_t count = extensions_list(&list);
    char *group_tpl = load_template("dashboard/extensions/nav-group_epoch%d.html", epoch);
    char *item_tpl  = load_template("dashboard/extensions/nav-item_epoch%d.html", epoch);
    char *items = strdup("");
    char *result = NULL;

    for (size_t i = 0; items && item_tpl && i < count; i++) {
        const ExtensionInfo *in = &list[i];
        if (!in->enabled || !in->nav_label[0] || in->state == EXTENSION_INVALID) continue;
        char *label = html_encode_alloc(in->nav_label);
        char *item = label ? render_template(item_tpl, in->name, in->nav_path, label) : NULL;
        items = item ? str_append(items, item) : (free(items), NULL);
        free(label);
        free(item);
    }

    if (items && group_tpl)
        result = items[0] ? render_template(group_tpl, items) : strdup("");
    else if (items && !group_tpl)
        result = strdup("");

    free(items);
    free(group_tpl);
    free(item_tpl);
    free(list);
    return result;
}
