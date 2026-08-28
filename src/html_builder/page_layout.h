#ifndef PAGE_LAYOUT_H
#define PAGE_LAYOUT_H

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
//   {{HOME-MODAL}}  epoch 3 home thumbnail modal (container)
//
// `body_background` fills {{BODY_BACKGROUND}} in the epoch-2 layout, where
// home and the blog listing carry the tiled backdrop and content pages do
// not. Pass NULL for none.
//
// Takes ownership of `fragment_html` and frees it. Returns NULL on failure.
char *page_layout_wrap(char *fragment_html, const char *page_title, int epoch,
                       const char *body_background);

#endif // PAGE_LAYOUT_H
