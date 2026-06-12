#ifndef DASHBOARD_H
#define DASHBOARD_H

// Loads the "/dashboard" content fragment for `epoch`. For EPOCH_MODERN,
// dashboard_epoch<N>.html's nav links and entries table are populated based on
// `role` ("admin"/"author"): an Administrador sees the existing
// Categories/Languages/Menu/Users links (dashboard/nav-admin_epoch3.html) and
// every entry (entries_admin_rows(epoch, lang, NULL, NULL)); an Autor sees no
// extra nav links and only their own "blog" entries
// (entries_admin_rows(epoch, lang, "blog", user_id)). `lang` is the resolved
// content language (cms_resolve_default_lang()). Other epochs' templates are
// static and `lang`/`user_id`/`role` are ignored.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *dashboard(int epoch, const char *lang, const char *user_id, const char *role);

#endif // DASHBOARD_H
