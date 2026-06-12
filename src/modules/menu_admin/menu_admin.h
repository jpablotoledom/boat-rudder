#ifndef MENU_ADMIN_H
#define MENU_ADMIN_H

#include "../../db/cms_languages.h"
#include <stdbool.h>
#include <stddef.h>

// /dashboard/menu - table of every `menu` document (any `enabled` value,
// sorted by `order`), plus a "new menu item" link. Returns a malloc'd
// string, or NULL on a missing template / allocation failure.
char *menu_admin_list(int epoch, const char *lang);

// /dashboard/menu/new (id == "") and /dashboard/menu/<id>/edit (id == hex
// ObjectId). `link`/`order`/`enabled` are the menu item's current
// link/order/enabled ("" / 0 / false for a new item). `langs`/`values` are
// parallel arrays of `lang_count` entries: one per active content language,
// with values[i] the current menu.name.<langs[i].code> (or "" for a new
// item). `error_message` is shown above the form if non-NULL/non-empty
// (e.g. after a failed save). Returns a malloc'd string, or NULL on a
// missing template / allocation failure.
char *menu_admin_form(int epoch, const char *id, const char *link, int order, bool enabled,
                       const CmsLanguageItem *langs, size_t lang_count, char *const *values,
                       const char *error_message);

#endif // MENU_ADMIN_H
