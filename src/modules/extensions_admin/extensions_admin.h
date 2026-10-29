#ifndef EXTENSIONS_ADMIN_H
#define EXTENSIONS_ADMIN_H

// /dashboard/settings/extensions (dashboard/extensions/list_epoch3.html):
// every extension found in ./extensions/, with its state and an
// Enable/Disable button - see extensions/extensions.h. `error_message` (may
// be NULL) shows above the table. Epoch 3 only. Returns a malloc'd string,
// or NULL on failure.
char *extensions_admin_page(int epoch, const char *error_message);

// The dashboard menu's "Extensions" group: one link per enabled extension
// whose manifest has a nav= entry, "" when there is none (or not epoch 3).
// Returns a malloc'd string, or NULL on allocation failure.
char *extensions_admin_nav(int epoch);

#endif // EXTENSIONS_ADMIN_H
