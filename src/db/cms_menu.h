#ifndef CMS_MENU_H
#define CMS_MENU_H

#include <stddef.h>

// Maximum number of "menu" documents (enabled == true) returned by
// cms_get_menu_items(), ordered by `order` ascending.
#define MENU_ITEM_LIMIT 20

// One "menu" document: a navigation link with a multi-language label.
typedef struct {
    char *link; // menu.link, e.g. "/" or "/page/about"
    char *name; // menu.name (map<lang,string>), resolved to `lang`
} CmsMenuItem;

// Looks up db.menu.find({enabled: true}).sort({order: 1}).limit(MENU_ITEM_LIMIT),
// resolving `name` to `lang` (same convention as cms_get_entry_by_link). On
// success, *out points to a malloc'd array of *out_count items (possibly 0)
// that must be passed to cms_menu_free(). On a DB error or if mongodb is not
// ready, *out = NULL and *out_count = 0 - the menu is decorative and must
// never fail the page.
void cms_get_menu_items(const char *lang, CmsMenuItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_menu_free(CmsMenuItem *items, size_t count);

#endif // CMS_MENU_H
