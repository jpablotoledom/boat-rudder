#ifndef CATEGORIES_ADMIN_H
#define CATEGORIES_ADMIN_H

#include "../../db/cms_languages.h"
#include <stddef.h>

// /dashboard/categories - table of every entry_categories document (name
// resolved to `lang`), plus a "new category" link. Returns a malloc'd
// string, or NULL on a missing template / allocation failure.
char *categories_admin_list(int epoch, const char *lang);

// /dashboard/categories/new (id == "") and /dashboard/categories/<id>/edit
// (id == hex ObjectId). `langs`/`values` are parallel arrays of `lang_count`
// entries: one per active content language, with values[i] the current
// entry_categories.name.<langs[i].code> (or "" for a new category).
// `error_message` is shown above the form if non-NULL/non-empty (e.g. after
// a failed save). Returns a malloc'd string, or NULL on a missing template /
// allocation failure.
char *categories_admin_form(int epoch, const char *id, const CmsLanguageItem *langs,
                             size_t lang_count, char *const *values, const char *error_message);

#endif // CATEGORIES_ADMIN_H
