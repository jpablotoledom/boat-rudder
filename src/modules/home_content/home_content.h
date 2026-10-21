#ifndef HOME_CONTENT_H
#define HOME_CONTENT_H

#include "../../db/cms_entries.h"

// Builds the home page's welcome text plus a static "updates" list for
// `epoch` and `lang`. `lang` is reserved for future translations.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *home_content(int epoch, const char *lang);

// `entry`'s content blocks in the same home-content wrapper home_content()
// puts the "/" entry in - the under-construction page's body (see
// buildUnderConstructionWebSite()). Returns a malloc'd string, or NULL on
// failure.
char *home_content_entry(const CmsEntry *entry, int epoch);

#endif // HOME_CONTENT_H
