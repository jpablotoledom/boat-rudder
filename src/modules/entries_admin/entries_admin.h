#ifndef ENTRIES_ADMIN_H
#define ENTRIES_ADMIN_H

// Builds the <tbody> rows for the entries table embedded in /dashboard:
// every db.entries document (any type, up to ENTRIES_LIST_LIMIT), newest
// header.date first - same fields as the blog list (image, title, summary,
// author, categories, date) plus the entry's type ("page"/"blog"). `lang` is
// the resolved content language (cms_resolve_default_lang()). Returns
// `dashboard/entries/list-empty_epoch<N>.html` if there are no entries.
char *entries_admin_rows(int epoch, const char *lang);

#endif // ENTRIES_ADMIN_H
