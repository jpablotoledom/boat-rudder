#ifndef CMS_LANGUAGES_H
#define CMS_LANGUAGES_H

#include <stddef.h>

// One `languages` document: a content language available for `map<lang,...>`
// fields across `entries`/`menu`/`entry_categories`. Exactly one document has
// is_default == 1 at any time.
typedef struct {
    char *code; // ISO 639-1, e.g. "en"
    char *name; // English display name, e.g. "English"
    int   is_default;
} CmsLanguageItem;

// db.languages.find().sort({code: 1}). On success, *out points to a malloc'd
// array of *out_count items (possibly 0) that must be passed to
// cms_languages_free(). On a DB error or if mongodb is not ready,
// *out = NULL and *out_count = 0.
void cms_get_languages(CmsLanguageItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_languages_free(CmsLanguageItem *items, size_t count);

// Inserts {code:"en", name:"English", is_default:true} iff `languages` is
// currently empty. Called once at startup, after mongodb_manager_init()
// succeeds. No-op if mongodb is not ready or the collection is non-empty.
void cms_languages_ensure_seeded(void);

// Writes db.languages.findOne({is_default:true}).code into `out` (truncated
// to out_size - 1, always NUL-terminated). Falls back to iso_lang(lang)
// (configs/settings.conf's global `lang`) if mongodb is not ready or no
// document has is_default:true.
void cms_resolve_default_lang(char *out, size_t out_size);

// Adds `code` as an active content language (name looked up in
// LANGUAGE_CATALOG). The new language's is_default is true iff `languages`
// was empty. Returns 0 on success, -1 if `code` is not in LANGUAGE_CATALOG,
// already present in `languages`, or mongodb is not ready.
int cms_add_language(const char *code);

// Clears is_default on every document and sets it on {code}. Returns 0 on
// success, -1 if `code` is not present in `languages` or mongodb is not ready.
int cms_set_default_language(const char *code);

// Removes {code} from `languages`. Returns -1 (no change) if `code` is not
// present, is the current default, is the only remaining language, or
// mongodb is not ready - callers must pick a different default / keep at
// least one language. Returns 0 on success.
int cms_remove_language(const char *code);

#endif // CMS_LANGUAGES_H
