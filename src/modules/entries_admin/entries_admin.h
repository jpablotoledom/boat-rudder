#ifndef ENTRIES_ADMIN_H
#define ENTRIES_ADMIN_H

// Builds the <tbody> rows for the entries table embedded in /dashboard:
// every db.entries document (any type, up to ENTRIES_LIST_LIMIT), newest
// header.date first - same fields as the blog list (image, title, summary,
// author, categories, date) plus the entry's type ("page"/"blog"). `lang` is
// the resolved content language (cms_resolve_default_lang()). `type_filter`
// and `created_by_hex` are passed straight through to cms_get_admin_entries()
// - NULL/NULL for Administrador (every entry), ("blog", user_id) for Autor
// (only their own blog entries). Returns
// `dashboard/entries/list-empty_epoch<N>.html` if there are no entries.
char *entries_admin_rows(int epoch, const char *lang, const char *type_filter,
                          const char *created_by_hex);

#endif // ENTRIES_ADMIN_H
