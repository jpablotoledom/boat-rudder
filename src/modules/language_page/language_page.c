#include "language_page.h"
#include "../../db/cms_languages.h"
#include "../../db/language_catalog.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_lang.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// Shown above the list. Not translated: it is the one string a reader who
// cannot read the current language still needs to understand.
#define LANGUAGE_PAGE_TITLE "Language / Idioma"

void language_sanitize_return(const char *raw, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';

    // Must be a site-relative path. "//evil.example" is protocol-relative and
    // would leave the site, so a second leading slash is rejected too.
    if (!raw || raw[0] != '/' || raw[1] == '/' || raw[1] == '\\') {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    for (const char *p = raw; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '"' || *p == '<' || *p == '>') {
            strncpy(out, "/", out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }

    strncpy(out, raw, out_size - 1);
    out[out_size - 1] = '\0';
}

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

char *language_page(int epoch, const char *return_url) {
    char *page_tpl = load_template("language/language_epoch%d.html", epoch);
    char *item_tpl = load_template("language/language-item_epoch%d.html", epoch);
    if (!page_tpl || !item_tpl) {
        free(page_tpl);
        free(item_tpl);
        return NULL;
    }

    CmsLanguageItem *langs = NULL;
    size_t count = 0;
    if (mongodb_manager_is_ready()) cms_get_languages(&langs, &count);

    char safe_return[512];
    language_sanitize_return(return_url, safe_return, sizeof(safe_return));

    char return_enc[1024];
    url_encode(return_enc, safe_return, sizeof(return_enc));

    const char *active = request_lang();

    char *items = strdup("");
    for (size_t i = 0; items && i < count; i++) {
        if (!langs[i].code) continue;

        int is_active = strcmp(langs[i].code, active) == 0;
        // Epoch 3 marks the current language with a class; the older templates
        // have no stylesheet to hang it on, so they get an inert attribute slot.
        const char *active_attr = epoch >= EPOCH_MODERN
            ? (is_active ? " boat-rudder__language__item--active" : "")
            : (is_active ? " class=\"br-language__item--active\"" : "");

        // Each language named in its own language, so it is recognisable to
        // the reader who is looking for it.
        char *item = render_template(item_tpl, active_attr, langs[i].code, return_enc,
                                      language_catalog_native(langs[i].code));
        items = item ? str_append(items, item) : NULL;
        free(item);
    }

    cms_languages_free(langs, count);

    char *result = items ? render_template(page_tpl, LANGUAGE_PAGE_TITLE, items) : NULL;

    free(items);
    free(page_tpl);
    free(item_tpl);
    return result;
}
