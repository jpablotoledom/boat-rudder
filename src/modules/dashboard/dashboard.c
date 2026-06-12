#include "dashboard.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include "../entries_admin/entries_admin.h"
#include <stdlib.h>
#include <string.h>

char *dashboard(int epoch, const char *lang, const char *user_id, const char *role) {
    char *path = generate_url_theme("dashboard/dashboard_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    if (epoch != EPOCH_MODERN) return tpl;

    int is_admin = strcmp(role, "admin") == 0;

    char *nav;
    if (is_admin) {
        char *nav_path = generate_url_theme("dashboard/nav-admin_epoch%d.html", epoch);
        nav = nav_path ? read_file_to_string(nav_path) : NULL;
        free(nav_path);
    } else {
        nav = strdup("");
    }

    char *rows = is_admin
        ? entries_admin_rows(epoch, lang, NULL, NULL)
        : entries_admin_rows(epoch, lang, "blog", user_id);

    char *result = (nav && rows) ? render_template(tpl, nav, rows) : NULL;

    free(tpl);
    free(nav);
    free(rows);
    return result;
}
