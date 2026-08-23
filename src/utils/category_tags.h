#ifndef CATEGORY_TAGS_H
#define CATEGORY_TAGS_H

#include <stddef.h>

// Renders a list of categories as tags, with the epoch's separator between
// them and never at either end.
//
// The same list appears in four places - the blog cards on home and /blog, the
// article page, and the dashboard's entry list - and it was written out four
// times, which is how the dashboard ended up without the separator the public
// site had gained. `links` may be NULL for the epochs whose template takes only
// the name.
//
// Returns a malloc'd string (possibly ""), or NULL on allocation failure. The
// caller must free() it.
char *category_tags_render(char **links, char **names, size_t count, int epoch);

#endif // CATEGORY_TAGS_H
