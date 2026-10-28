#ifndef PAGE_LAYOUT_H
#define PAGE_LAYOUT_H

#include <stddef.h>

// Assembles a finished page out of the parts each page type needs.
//
// A page is built in two stages with two different mechanisms, and the split
// matters. The *fragment* - container/page/page-entry/page-blog - is the only
// piece that varies in shape between page types, so it is the only piece that
// goes through render_template(): its `%s` count is fixed by the file, which
// is why the home shell (menu, mainbanner, home-content, home-blog) and a content
// page (menu, content) cannot be the same file.
//
// Everything around it is identical for every page of an epoch - doctype,
// head, body, footer, and on epoch 3 the lightbox - so it lives once per epoch
// under `layout/` and is spliced in by name with str_replace_first(). That is
// plain text substitution, not printf, which also means those files carry a
// literal `%` rather than `%%`.
//
// Markers a fragment may carry, each replaced by `layout/<name>_epoch<N>.html`
// when present and dropped when the file is missing:
//   {{FOOTER}}      the site footer
//   {{LIGHTBOX}}    epoch 3 gallery viewer (page, page-entry)
//
// Takes ownership of `fragment_html` and frees it. Returns NULL on failure.
char *page_layout_wrap(char *fragment_html, const char *page_title, int epoch);

// The site's uploaded favicons (/dashboard/settings): epoch 2 and 3 each have
// one file in the static root. page_layout_wrap() points the layout's favicon
// <link> at it when it exists (with a ?v=<mtime> cache-buster), and leaves the
// theme's own link alone otherwise.
//
// page_layout_favicon_path() writes "./html/favicon<N>.ico" for epoch 2/3 and
// returns 1, or returns 0 for any other epoch.
int page_layout_favicon_path(int epoch, char *out, size_t out_size);

// The favicon URL an epoch's pages use - the uploaded file's, else the href
// of the active theme's layout <link> - as a malloc'd string, NULL if there
// is none. *is_custom (if non-NULL) says whether it is the uploaded one.
char *page_layout_favicon_url(int epoch, int *is_custom);

#endif // PAGE_LAYOUT_H
