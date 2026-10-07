#ifndef CMS_CATEGORIES_H
#define CMS_CATEGORIES_H

#include "cms_languages.h"
#include <stddef.h>

// Maximum number of `entry_categories` documents returned by
// cms_get_categories() for the /dashboard/categories listing.
#define CATEGORY_LIST_LIMIT 200

// One `entry_categories` document, for the admin listing.
typedef struct {
    char *id;   // hex ObjectId (24 chars + NUL)
    char *name; // entry_categories.name resolved to `lang`
} CmsCategoryItem;

// db.entry_categories.find().sort({_id: 1}).limit(CATEGORY_LIST_LIMIT), with
// `name` resolved to `lang`. On success, *out points to a malloc'd array of
// *out_count items (possibly 0) that must be passed to cms_categories_free().
// On a DB error or if mongodb is not ready, *out = NULL and *out_count = 0.
void cms_get_categories(const char *lang, CmsCategoryItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_categories_free(CmsCategoryItem *items, size_t count);

// For the edit form: out_values[i] is the exact value of
// entry_categories.name.<langs[i].code> for the document {_id: id_hex} (no
// "en" fallback - "" if that language's key is absent). out_values is
// caller-allocated with lang_count entries; free with
// cms_category_name_values_free(). Returns 0 on success, -1 if id_hex is not
// a valid ObjectId, the document doesn't exist, or mongodb is not ready.
int cms_get_category_name_values(const char *id_hex, const CmsLanguageItem *langs,
                                  size_t lang_count, char **out_values);

// Frees each of `count` strings in `values` and the array itself. Safe to
// call with values == NULL.
void cms_category_name_values_free(char **values, size_t count);

// Inserts {name: {<langs[i].code>: values[i], ...}}. Returns 0 on success,
// -1 on a DB error or if mongodb is not ready.
int cms_create_category(const CmsLanguageItem *langs, size_t lang_count, char *const *values);

// db.entry_categories.updateOne({_id: id_hex}, {$set: {name: {...}}}).
// Returns 0 on success, -1 if id_hex is invalid, on a DB error, or if
// mongodb is not ready.
int cms_update_category(const char *id_hex, const CmsLanguageItem *langs,
                         size_t lang_count, char *const *values);

// db.entry_categories.deleteOne({_id: id_hex}). Any entries.categories[]
// referencing this id become dangling - resolve_category_names()'s $in
// lookup already handles missing ids gracefully (they just don't resolve to
// a name). Returns 0 on success, -1 if id_hex is invalid, on a DB error, or
// if mongodb is not ready.
int cms_delete_category(const char *id_hex);

#endif // CMS_CATEGORIES_H
