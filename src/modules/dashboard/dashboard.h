#ifndef DASHBOARD_H
#define DASHBOARD_H

// Loads the "/dashboard" content fragment for `epoch`. For EPOCH_MODERN,
// dashboard_epoch<N>.html's entries table is populated via
// entries_admin_rows(epoch, lang) (`lang` is the resolved content language,
// cms_resolve_default_lang()); other epochs' templates are static and `lang`
// is ignored.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *dashboard(int epoch, const char *lang);

#endif // DASHBOARD_H
