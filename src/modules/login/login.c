#include "login.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>

char *login(int epoch, const char *error_message) {
    char *path = generate_url_theme("login/login_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    if (epoch != EPOCH_MODERN) return tpl;

    char error_html[512] = "";
    if (error_message && error_message[0]) {
        snprintf(error_html, sizeof(error_html),
                 "<p class=\"boat-rudder__login__error\">%s</p>", error_message);
    }

    char *result = render_template(tpl, error_html);
    free(tpl);
    return result;
}
