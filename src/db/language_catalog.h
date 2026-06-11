#ifndef LANGUAGE_CATALOG_H
#define LANGUAGE_CATALOG_H

#include <stddef.h>

// One selectable content language: ISO 639-1 `code` + its English `name`.
typedef struct {
    const char *code;
    const char *name;
} LanguageCatalogEntry;

// Curated list of content languages that can be added via
// /dashboard/languages (POST /dashboard/languages/add).
extern const LanguageCatalogEntry LANGUAGE_CATALOG[];
extern const size_t LANGUAGE_CATALOG_COUNT;

// Returns the English name for `code`, or NULL if `code` is not in
// LANGUAGE_CATALOG.
const char *language_catalog_name(const char *code);

#endif // LANGUAGE_CATALOG_H
