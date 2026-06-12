#ifndef CMS_MENU_H
#define CMS_MENU_H

#include "cms_languages.h"
#include <stdbool.h>
#include <stddef.h>

// Maximum number of "menu" documents (enabled == true) returned by
// cms_get_menu_items(), ordered by `order` ascending.
#define MENU_ITEM_LIMIT 20

// Maximum number of "menu" documents returned by cms_get_menu_admin_items()
// for the /dashboard/menu listing.
#define MENU_ADMIN_LIST_LIMIT 100

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

// One "menu" document, for the /dashboard/menu admin listing.
typedef struct {
    char *id;      // hex ObjectId (24 chars + NUL)
    char *link;    // menu.link
    char *name;    // menu.name resolved to `lang`
    int   order;   // menu.order
    bool  enabled; // menu.enabled
} CmsMenuAdminItem;

// db.menu.find().sort({order: 1}).limit(MENU_ADMIN_LIST_LIMIT), with `name`
// resolved to `lang` (no `enabled` filter - the admin must see disabled
// items too). On success, *out points to a malloc'd array of *out_count
// items (possibly 0) that must be passed to cms_menu_admin_free(). On a DB
// error or if mongodb is not ready, *out = NULL and *out_count = 0.
void cms_get_menu_admin_items(const char *lang, CmsMenuAdminItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_menu_admin_free(CmsMenuAdminItem *items, size_t count);

// For the edit form: out_values[i] is the exact value of
// menu.name.<langs[i].code> for the document {_id: id_hex} (no "en"
// fallback - "" if that language's key is absent). out_link/out_order/
// out_enabled receive menu.link/.order/.enabled. out_values is
// caller-allocated with lang_count entries; free with
// cms_menu_name_values_free(). Returns 0 on success, -1 if id_hex is not a
// valid ObjectId, the document doesn't exist, or mongodb is not ready.
int cms_get_menu_item_values(const char *id_hex, const CmsLanguageItem *langs, size_t lang_count,
                              char **out_values, char *out_link, size_t out_link_size,
                              int *out_order, bool *out_enabled);

// Frees each of `count` strings in `values` and the array itself. Safe to
// call with values == NULL.
void cms_menu_name_values_free(char **values, size_t count);

// Inserts {link, order, enabled, name: {<langs[i].code>: values[i], ...}}.
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_create_menu_item(const char *link, int order, bool enabled,
                          const CmsLanguageItem *langs, size_t lang_count, char *const *values);

// db.menu.updateOne({_id: id_hex}, {$set: {link, order, enabled, name: {...}}}).
// Returns 0 on success, -1 if id_hex is invalid, on a DB error, or if
// mongodb is not ready.
int cms_update_menu_item(const char *id_hex, const char *link, int order, bool enabled,
                          const CmsLanguageItem *langs, size_t lang_count, char *const *values);

// db.menu.deleteOne({_id: id_hex}). Returns 0 on success, -1 if id_hex is
// invalid, on a DB error, or if mongodb is not ready.
int cms_delete_menu_item(const char *id_hex);

#endif // CMS_MENU_H
