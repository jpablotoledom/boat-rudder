#include "dashboard.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include "../entries_admin/entries_admin.h"
#include <stdlib.h>

char *dashboard(int epoch, const char *lang) {
    char *path = generate_url_theme("dashboard/dashboard_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    if (epoch != EPOCH_MODERN) return tpl;

    char *rows   = entries_admin_rows(epoch, lang);
    char *result = rows ? render_template(tpl, rows) : NULL;

    free(tpl);
    free(rows);
    return result;
}
