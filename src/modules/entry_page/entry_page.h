#ifndef ENTRY_PAGE_H
#define ENTRY_PAGE_H

#include "../../db/cms_entries.h"

// Renders entry->header followed by entry->content[] (in order) into one
// HTML/WML fragment suitable for buildPageWebSite()'s html_content argument.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). Unknown content[].type values are skipped, so the
// page still renders if it contains a block type this increment doesn't
// support yet.
char *entry_page(const CmsEntry *entry, int epoch);

#endif // ENTRY_PAGE_H
