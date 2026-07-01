#ifndef BLOG_LIST_H
#define BLOG_LIST_H

// Builds the full "/blogs" listing page content for `epoch`: every
// db.entries document with type == "blog" (up to BLOG_LIST_LIMIT), newest
// header.date first (see cms_get_blog_entries()). `lang` is Boat Rudder's
// configs/settings.conf "Eng"/"Esp" convention. If there are no blog
// entries, renders the epoch's "No blog entries found" empty state.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *blog_list(int epoch, const char *lang);

// Like blog_list() but filtered to entries belonging to `category_id_hex`
// (a 24-char hex ObjectId). Returns a malloc'd string or NULL on failure.
char *blog_list_category(int epoch, const char *lang, const char *category_id_hex);

#endif // BLOG_LIST_H
