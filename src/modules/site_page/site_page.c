#include "site_page.h"
#include "../../utils/config_loader.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/request_site.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Shown above the list. Not translated, for the same reason as
// language_page.c's title: the reader hasn't reached any site yet.
#define SITE_PAGE_TITLE "Sites / Sitios"

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// The title goes into the page unescaped, so anything that could open a tag
// or an attribute rules the entry out. It comes from the machine's own
// configuration, not from a visitor - this only guards against a typo.
static int title_is_safe(const char *title) {
    if (!title[0]) return 0;
    for (const char *p = title; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '<' || *p == '>' || *p == '"' || *p == '&')
            return 0;
    }
    return 1;
}

int site_page_applies(const char *site_query) {
    return request_site() && site_directory[0] && !(site_query && site_query[0]);
}

char *site_page(int epoch) {
    char *page_tpl = load_template("site/site_epoch%d.html", epoch);
    char *item_tpl = load_template("site/site-item_epoch%d.html", epoch);
    if (!page_tpl || !item_tpl) {
        free(page_tpl);
        free(item_tpl);
        return NULL;
    }

    char list[sizeof(site_directory)];
    strncpy(list, site_directory, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    // "key|Title;key|Title;..." - see config_loader.h.
    int shown = 0;
    char *items = strdup("");
    char *save = NULL;
    for (char *entry = strtok_r(list, ";", &save); items && entry;
         entry = strtok_r(NULL, ";", &save)) {
        char *bar = strchr(entry, '|');
        if (!bar) continue;
        *bar = '\0';
        const char *key = entry;
        const char *title = bar + 1;
        if (!request_site_key_is_valid(key) || !title_is_safe(title)) continue;

        char href[64];
        snprintf(href, sizeof(href), "/?site=%s", key);
        char *item = render_template(item_tpl, href, title);
        items = item ? str_append(items, item) : NULL;
        free(item);
        shown++;
    }

    char *result = (items && shown) ? render_template(page_tpl, SITE_PAGE_TITLE, items) : NULL;

    free(items);
    free(page_tpl);
    free(item_tpl);
    return result;
}
