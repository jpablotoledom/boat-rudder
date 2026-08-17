#ifndef LANGUAGE_CATALOG_H
#define LANGUAGE_CATALOG_H

#include <stddef.h>

// One selectable content language: ISO 639-1 `code`, its English `name` (used
// by the dashboard, which is English-only), plus how the language calls itself
// - `native` for pickers and `abbr` for the compact nav-bar button.
//
// `abbr` is spelled out rather than derived: truncating `native` would cut
// multi-byte characters in half for Russian, Chinese, Greek and others, and
// the conventional short form is not always the first three letters anyway.
typedef struct {
    const char *code;
    const char *name;
    const char *native;
    const char *abbr;
} LanguageCatalogEntry;

// Curated list of content languages that can be added via
// /dashboard/languages (POST /dashboard/languages/add).
extern const LanguageCatalogEntry LANGUAGE_CATALOG[];
extern const size_t LANGUAGE_CATALOG_COUNT;

// Returns the English name for `code`, or NULL if `code` is not in
// LANGUAGE_CATALOG.
const char *language_catalog_name(const char *code);

// The language's own name ("Espanol") and its short form ("Esp") for `code`.
// Both fall back to `code` itself when it is not in the catalog, so a language
// added by hand still renders something sensible - never NULL.
const char *language_catalog_native(const char *code);
const char *language_catalog_abbr(const char *code);

#endif // LANGUAGE_CATALOG_H
