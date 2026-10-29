#include "dashboard.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include "../analytics_view/analytics_view.h"
#include "../entries_admin/entries_admin.h"
#include "../extensions_admin/extensions_admin.h"
#include "../comments_admin/comments_admin.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// An Autor only ever sees their own blog entries, and cannot delete them; an
// Administrador sees (and may delete) all - and so does an Observador, the
// read-only role, whose deletes the router refuses.
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

    // A moderator only reviews comments: its own, smaller home.
    if (strcmp(role, "moderator") == 0) {
        free(tpl);
        char *mod_tpl = load_template("dashboard/dashboard-moderator_epoch%d.html", epoch);
        char *nav = load_template("dashboard/nav-moderator_epoch%d.html", epoch);
        char *pending = comments_admin_pending_notice(epoch, NULL);
        char *result = (mod_tpl && nav && pending) ? render_template(mod_tpl, nav, pending) : NULL;
        free(mod_tpl);
        free(nav);
        free(pending);
        return result;
    }

    // An Observador (viewer) gets the Administrador's page: every option,
    // the analytics summary, every entry - read-only.
    int is_admin = strcmp(role, "admin") == 0 || strcmp(role, "viewer") == 0;

    // An Autor's option list only has the content options it can use.
    char *nav_tpl = load_template(is_admin ? "dashboard/nav-admin_epoch%d.html"
                                           : "dashboard/nav-author_epoch%d.html", epoch);
    // Enabled extensions add their own group, for every role.
    char *ext_nav = extensions_admin_nav(epoch);
    char *nav = (nav_tpl && ext_nav) ? render_template(nav_tpl, ext_nav) : NULL;
    free(nav_tpl);
    free(ext_nav);
    char *rows = role_entries_rows(epoch, lang, user_id, is_admin, true);
    // /dashboard/analytics is admin-only, and so is its summary (shown above
    // the pending entries).
    char *summary = is_admin ? analytics_summary(epoch) : strdup("");
    // Comments waiting for moderation lead it, for whoever can moderate them.
    char *pending = comments_admin_pending_notice(epoch, is_admin ? NULL : user_id);
    if (summary && pending && pending[0]) {
        char *joined = str_append(pending, summary);
        free(summary);
        summary = joined;
        pending = NULL;
    }
    free(pending);

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

    char *rows = role_entries_rows(epoch, lang, user_id,
                                   strcmp(role, "admin") == 0 || strcmp(role, "viewer") == 0, false);
    char *result = rows ? render_template(tpl, rows) : NULL;

    free(tpl);
    free(rows);
    return result;
}
