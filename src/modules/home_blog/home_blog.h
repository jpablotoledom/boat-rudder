#ifndef HOME_BLOG_H
#define HOME_BLOG_H

// Builds the "Latest Blog Posts" section for `epoch` from a static list of
// sample posts.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *home_blog(int epoch);

#endif // HOME_BLOG_H
