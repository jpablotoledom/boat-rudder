#ifndef HOME_BLOG_H
#define HOME_BLOG_H

// Builds the "Latest Blog Posts" section for `epoch`, listing up to
// HOME_BLOG_LIMIT db.entries documents with type == "blog", newest first
// (see cms_get_blog_entries()). `lang` is Boat Rudder's
// configs/settings.conf "Eng"/"Esp" convention. If there are no blog
// entries, renders the epoch's "No blog entries found" empty state instead.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *home_blog(int epoch, const char *lang);

#endif // HOME_BLOG_H
