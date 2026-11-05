#ifndef BLOG_LIST_H
#define BLOG_LIST_H

// The blog list, in its three appearances.
//
// The home section, the /blog page and a category listing are one component:
// the same card, the same container, the same empty state. What separates them
// is how many entries they ask for, whether the query filters by category, and
// the heading printed above the list - so those are parameters, not three
// copies. They all render `home-blog/home-blog_epoch<N>.html`, which takes the
// heading and the rendered items.
//
// `lang` follows Boat Rudder's configs/settings.conf "Eng"/"Esp" convention.
// With no entries to show, each one renders the epoch's "No blog entries
// found" empty state instead of an empty container.
//
// All three return a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.

// Up to HOME_BLOG_LIMIT entries, newest first, under "Latest Blog Posts".
char *home_blog(int epoch, const char *lang);

// Page `page` (1 = newest) of BLOG_LIST_LIMIT entries, newest first, under
// "Blog", with a pager under them when there is more than one page - its
// links are `base_url` ("/blog") and `base_url?page=N`. A page past the last
// one sets *found to 0 (the caller answers 404); otherwise *found is 1.
char *blog_list(int epoch, const char *lang, const char *base_url, int page, int *found);

// As blog_list(), restricted to entries carrying `category_id_hex`
// (a 24-char hex ObjectId); `base_url` is the category's "/blog/category/<slug>".
char *blog_list_category(int epoch, const char *lang, const char *category_id_hex,
                         const char *base_url, int page, int *found);

#endif // BLOG_LIST_H
