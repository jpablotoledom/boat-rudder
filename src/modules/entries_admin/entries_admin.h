#ifndef ENTRIES_ADMIN_H
#define ENTRIES_ADMIN_H

#include <stdbool.h>

// Builds the <tbody> rows for the entries tables of /dashboard (drafts only,
// `unpublished_only`) and /dashboard/entries (everything): db.entries
// documents (any type, up to ENTRIES_LIST_LIMIT), newest header.date first - same fields as the blog list (image, title, summary,
// author, categories, date) plus the entry's type ("page"/"blog"). `lang` is
// the resolved content language (cms_resolve_default_lang()). `type_filter`
// and `created_by_hex` are passed straight through to cms_get_admin_entries()
// - NULL/NULL for Administrador (every entry), ("blog", user_id) for Autor
// (only their own blog entries). A draft's title carries a "Draft" badge
// (list-draft_epoch<N>.html) and every title links to the editor (the public
// URL is the row's "View" action). `can_delete` adds the Delete button
// (list-row-delete_epoch<N>.html) - true only for an Administrador, the only
// role /dashboard/entries/<id>/delete accepts. Returns
// `dashboard/entries/list-empty_epoch<N>.html` if there are no entries.
char *entries_admin_rows(int epoch, const char *lang, const char *type_filter,
                          const char *created_by_hex, bool unpublished_only,
                          bool can_delete);

#endif // ENTRIES_ADMIN_H
