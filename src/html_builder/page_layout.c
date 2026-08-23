#include "page_layout.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Replaces `marker` with the contents of layout/<part>_epoch<N>.html. A part
// that has no file for this epoch resolves to nothing, which is how the retro
// epochs drop the epoch-3 viewers without needing an empty file each.
static char *splice_part(char *html, const char *marker, const char *part, int epoch) {
    if (!html || !strstr(html, marker)) return html;

    char subpath[128];
    snprintf(subpath, sizeof(subpath), "layout/%s_epoch%%d.html", part);

    char *path = generate_url_theme(subpath, epoch);
    char *body = path ? read_file_to_string(path) : NULL;
    free(path);

    char *result = str_replace_first(html, marker, body ? body : "");
    free(body);
    free(html);
    return result;
}

char *page_layout_wrap(char *fragment_html, const char *page_title, int epoch,
                       const char *body_background) {
    if (!fragment_html) return NULL;

    fragment_html = splice_part(fragment_html, "{{FOOTER}}",     "footer",     epoch);
    fragment_html = splice_part(fragment_html, "{{LIGHTBOX}}",   "lightbox",   epoch);
    fragment_html = splice_part(fragment_html, "{{HOME-MODAL}}", "home-modal", epoch);
    if (!fragment_html) return NULL;

    char *path   = generate_url_theme("layout/layout_epoch%d.html", epoch);
    char *layout = path ? read_file_to_string(path) : NULL;
    free(path);
    if (!layout) {
        free(fragment_html);
        return NULL;
    }

    char *title_tag = build_title_tag(page_title);
    char *titled    = title_tag ? str_replace_first(layout, "{{PAGE_TITLE}}", title_tag) : NULL;
    free(title_tag);
    free(layout);
    if (!titled) {
        free(fragment_html);
        return NULL;
    }

    char *with_bg = str_replace_first(titled, "{{BODY_BACKGROUND}}",
                                      body_background ? body_background : "");
    free(titled);
    if (!with_bg) {
        free(fragment_html);
        return NULL;
    }

    char *result = str_replace_first(with_bg, "{{CONTENT}}", fragment_html);
    free(with_bg);
    free(fragment_html);
    return result;
}
