#include "home_content.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *title;
    const char *date;
    const char *text;
} update_item_t;

// MVP: static "updates" list, no database/CMS backing yet.
static const update_item_t UPDATES[] = {
    {"Boat Rudder is online", "2026",
     "A retro-compatible site that serves the right HTML for every browser, "
     "from WAP phones to modern Chrome."},
    {"Five epochs, one codebase", "2026",
     "Every page is assembled from epoch-specific templates: WML, plain "
     "text, HTML 3.2, HTML4+CSS and HTML5+CSS3."},
    {"More to come", "2026",
     "New sections will be added soon, all following the same "
     "retro-compatible approach."},
};

#define UPDATE_COUNT (sizeof(UPDATES) / sizeof(UPDATES[0]))

char *home_content(int epoch, const char *lang) {
    (void)lang;

    char *item_path    = generate_url_theme("home-content/home-content-item_epoch%d.html", epoch);
    char *content_path = generate_url_theme("home-content/home-content_epoch%d.html", epoch);

    char *item_tpl    = item_path    ? read_file_to_string(item_path)    : NULL;
    char *content_tpl = content_path ? read_file_to_string(content_path) : NULL;

    free(item_path);
    free(content_path);

    char *items  = NULL;
    char *result = NULL;

    if (!item_tpl || !content_tpl) goto cleanup;

    items = strdup("");
    if (!items) goto cleanup;

    for (size_t i = 0; i < UPDATE_COUNT; i++) {
        char *item = render_template(item_tpl, UPDATES[i].title, UPDATES[i].date, UPDATES[i].text);
        if (!item) goto cleanup;

        items = str_append(items, item);
        free(item);
        if (!items) goto cleanup;
    }

    result = render_template(content_tpl, items);

cleanup:
    free(item_tpl);
    free(content_tpl);
    free(items);
    return result;
}
