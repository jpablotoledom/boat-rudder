#include "home_content.h"
#include "../entry_page/entry_page.h"
#include "../../db/cms_entries.h"
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

static char *render_static_items(int epoch) {
    char *item_path = generate_url_theme("home-content/home-content-item_epoch%d.html", epoch);
    char *item_tpl  = item_path ? read_file_to_string(item_path) : NULL;
    free(item_path);
    if (!item_tpl) return NULL;

    char *items = strdup("");
    for (size_t i = 0; items && i < UPDATE_COUNT; i++) {
        char *item = render_template(item_tpl, UPDATES[i].title, UPDATES[i].date, UPDATES[i].text);
        items = item ? str_append(items, item) : NULL;
        free(item);
    }

    free(item_tpl);
    return items;
}

char *home_content(int epoch, const char *lang) {
    char *content_path = generate_url_theme("home-content/home-content_epoch%d.html", epoch);
    char *content_tpl  = content_path ? read_file_to_string(content_path) : NULL;
    free(content_path);
    if (!content_tpl) return NULL;

    char *items = NULL;

    CmsEntry entry;
    if (cms_get_entry_by_link("/", lang, &entry)) {
        items = entry_page_render_content(&entry, epoch);
        cms_entry_free(&entry);
    }

    if (!items)
        items = render_static_items(epoch);

    char *result = items ? render_template(content_tpl, items) : NULL;

    free(content_tpl);
    free(items);
    return result;
}
