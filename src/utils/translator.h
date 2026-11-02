#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <stddef.h>

// Machine translation for the entry editor (entry-translation-plan.md), the
// server's side: Azure AI Translator, called with the key stored in
// site_settings. The "local" provider never comes through here - the
// editor's browser calls it.

// Most texts one call takes, and their total length in characters (Azure
// takes 1,000 texts and 50,000 characters a request).
#define TRANSLATOR_MAX_TEXTS 50
#define TRANSLATOR_MAX_CHARS 50000

// Must run once before any request thread starts: libcurl's global setup.
void translator_init(void);
void translator_cleanup(void);

// Translates texts[0..count) from `source` to `target` (ISO 639-1 codes, as
// the site's languages are) through Azure with `key` and the resource's
// `region` ("" for a global resource); `html` keeps the markup. The address
// is configs/settings.conf's translate_azure_url if set, else Azure's
// global endpoint. Fills out[0..count) with malloc'd translations and
// returns 0, or returns -1 with nothing allocated and a short reason for
// the editor in `error`.
int translator_azure(const char *key, const char *region, const char *const *texts, size_t count,
                     const char *source, const char *target, int html, char **out,
                     char *error, size_t error_size);

#endif // TRANSLATOR_H
