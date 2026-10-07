#ifndef DASHBOARD_H
#define DASHBOARD_H

// Loads the "/dashboard" content fragment for `epoch`. For EPOCH_MODERN,
// dashboard_epoch<N>.html is populated based on `role` ("admin"/"author"):
//   - the option groups: dashboard/nav-admin_epoch3.html (Content, Site,
//     Administration, Account) for an Administrador, nav-author_epoch3.html
//     (Content, Account) for an Autor - both end with "Log out";
//   - "Entries pending publication": unpublished entries only - every one for
//     an Administrador, only their own "blog" entries for an Autor;
//   - for an Administrador only, analytics_summary() (today / last 7 days).
// `lang` is the resolved content language (cms_resolve_default_lang()).
// Other epochs' templates are static and `lang`/`user_id`/`role` are ignored.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *dashboard(int epoch, const char *lang, const char *user_id, const char *role);

// Loads the "/dashboard/entries" fragment (dashboard/entries/list_epoch3.html,
// "View all"): every entry, published or not, with the same role filter as
// dashboard(). Epoch 3 only - the route redirects other epochs. Returns a
// malloc'd string, or NULL on failure.
char *dashboard_entries(int epoch, const char *lang, const char *user_id, const char *role);

#endif // DASHBOARD_H
