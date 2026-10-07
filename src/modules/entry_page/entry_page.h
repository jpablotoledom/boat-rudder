#ifndef ENTRY_PAGE_H
#define ENTRY_PAGE_H

#include "../../db/cms_entries.h"

// Renders entry->header followed by entry->content[] (in order) into one
// HTML/WML fragment suitable for buildPageWebSite()'s html_content argument.
//
// WML (epoch -1) comes back as one whole deck: the response layer splits
// it into single-packet-sized pages for every WAP client (see
// build_epoch_response.c and wap_gateway/wbxml.h).
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). Unknown content[].type values are skipped, so the
// page still renders if it contains a block type this increment doesn't
// support yet.
char *entry_page(const CmsEntry *entry, int epoch);

// Renders only entry->content[] (in order), without the header or
// categories. Used by home_content to render the "/" entry's body inside
// the home-content wrapper template.
//
// Returns a malloc'd string (possibly empty if content_count == 0), or
// NULL on failure.
char *entry_page_render_content(const CmsEntry *entry, int epoch);

// Renders one content block of `type` (its `text` and `extra_data`) exactly
// as a public page would for `epoch` - the entry editor previews unselected
// code-text/gallery blocks with epoch 3's output, so they can't drift from
// the real thing. Returns a malloc'd string ("" for an unknown type or empty
// block), or NULL on failure.
char *entry_page_render_block(const char *type, const char *text,
                              const char *extra_data, int epoch);

#endif // ENTRY_PAGE_H
