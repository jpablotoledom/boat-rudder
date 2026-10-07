#ifndef CATEGORY_MENU_H
#define CATEGORY_MENU_H

#include "../../db/cms_categories.h"
#include <stddef.h>

// Renders the category sub-menu bar shown below the main navbar on blog pages.
// `categories` is the full list of all entry_categories documents. `current_slug`
// is the URL slug of the currently active category (NULL if none is selected).
// Uses slugify() to convert each category name to a URL slug for matching and
// for building the href. Returns a malloc'd HTML string (possibly "") or NULL
// on allocation failure. The caller must free() the result.
char *category_menu_render(const CmsCategoryItem *categories, size_t count,
                            const char *current_slug, int epoch);

#endif // CATEGORY_MENU_H
