#ifndef CMS_EXTENSIONS_H
#define CMS_EXTENSIONS_H

#include <stddef.h>

// Which installed extensions an admin switched on: one document per
// extension that was ever enabled, {name, enabled}. An extension found on
// disk with no document is disabled - installing one never runs it by
// itself (see extensions/extensions.h).

// Fills `names` (up to `max` entries) with the name of every
// extension whose `enabled` is true. Returns how many, or -1 on a DB error /
// MongoDB not ready.
int cms_extensions_enabled(char (*names)[33], size_t max);

// Upserts {name, enabled}. Returns 0 on success, -1 on a DB error or if
// MongoDB is not ready.
int cms_extensions_set_enabled(const char *name, int enabled);

#endif // CMS_EXTENSIONS_H
