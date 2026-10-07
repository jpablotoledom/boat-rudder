#include "login.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>

char *login(int epoch, const char *error_message) {
    char *path = generate_url_theme("login/login_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    if (epoch != EPOCH_MODERN) return tpl;

    char *error_html = NULL;
    if (error_message && error_message[0]) {
        char *error_path = generate_url_theme("login/login-error_epoch%d.html", epoch);
        char *error_tpl  = error_path ? read_file_to_string(error_path) : NULL;
        free(error_path);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    char *result = render_template(tpl, error_html ? error_html : "");
    free(error_html);
    free(tpl);
    return result;
}
