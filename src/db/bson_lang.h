#ifndef BSON_LANG_H
#define BSON_LANG_H

#include <bson/bson.h>

// Maps configs/settings.conf's "Eng"/"Esp" lang values to ISO 639-1 codes
// used by the embedded schema documents. Defaults to "en".
const char *iso_lang(const char *configured_lang);

// Resolves a `map<lang,string>` subdocument (e.g. {"en": "...", "es": "..."})
// to `lang`'s value, falling back to "en", then "". Always returns a
// malloc'd string.
char *resolve_lang_map(const bson_t *parent, const char *field, const char *lang);

#endif // BSON_LANG_H
