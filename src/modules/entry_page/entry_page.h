#ifndef ENTRY_PAGE_H
#define ENTRY_PAGE_H

#include "../../db/cms_entries.h"

// Renders entry->header followed by entry->content[] (in order) into one
// HTML/WML fragment suitable for buildPageWebSite()'s html_content argument.
//
// `page` (1-based) only matters for WML: a real WAP 1.x deck has to fit in
// a few KB of memory on the device (a Nokia 7110 topped out around 1400
// compiled bytes; even the generous ~16KB an emulator tolerates is often
// blown by one full article with a table or gallery), so entry content is
// split across as many decks as it takes to keep each one small, with a
// [Prev]/[Next] line at the end of each. Every other epoch ignores `page`
// and always returns the whole entry. If total_pages_out is non-NULL, it
// is set to the entry's total page count for this epoch (always 1 outside
// WML) - out-of-range `page` values are clamped into range rather than
// producing an empty page.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). Unknown content[].type values are skipped, so the
// page still renders if it contains a block type this increment doesn't
// support yet.
char *entry_page(const CmsEntry *entry, int epoch, int page, int *total_pages_out);

// Renders only entry->content[] (in order), without the header or
// categories. Used by home_content to render the "/" entry's body inside
// the home-content wrapper template.
//
// Returns a malloc'd string (possibly empty if content_count == 0), or
// NULL on failure.
char *entry_page_render_content(const CmsEntry *entry, int epoch);

#endif // ENTRY_PAGE_H
