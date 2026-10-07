#include "dashboard.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include "../analytics_view/analytics_view.h"
#include "../entries_admin/entries_admin.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// An Autor only ever sees their own blog entries, and cannot delete them; an
// Administrador sees (and may delete) all.
static char *role_entries_rows(int epoch, const char *lang, const char *user_id,
                                bool is_admin, bool unpublished_only) {
    return is_admin
        ? entries_admin_rows(epoch, lang, NULL, NULL, unpublished_only, true)
        : entries_admin_rows(epoch, lang, "blog", user_id, unpublished_only, false);
}

char *dashboard(int epoch, const char *lang, const char *user_id, const char *role) {
    char *tpl = load_template("dashboard/dashboard_epoch%d.html", epoch);
    if (!tpl) return NULL;

    if (epoch != EPOCH_MODERN) return tpl;

    int is_admin = strcmp(role, "admin") == 0;

    // An Autor's option list only has the content options it can use.
    char *nav  = load_template(is_admin ? "dashboard/nav-admin_epoch%d.html"
                                        : "dashboard/nav-author_epoch%d.html", epoch);
    char *rows = role_entries_rows(epoch, lang, user_id, is_admin, true);
    // /dashboard/analytics is admin-only, and so is its summary (shown above
    // the pending entries).
    char *summary = is_admin ? analytics_summary(epoch) : strdup("");

    char *result = (nav && rows && summary) ? render_template(tpl, nav, summary, rows) : NULL;

    free(tpl);
    free(nav);
    free(rows);
    free(summary);
    return result;
}

char *dashboard_entries(int epoch, const char *lang, const char *user_id, const char *role) {
    char *tpl = load_template("dashboard/entries/list_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *rows = role_entries_rows(epoch, lang, user_id, strcmp(role, "admin") == 0, false);
    char *result = rows ? render_template(tpl, rows) : NULL;

    free(tpl);
    free(rows);
    return result;
}
