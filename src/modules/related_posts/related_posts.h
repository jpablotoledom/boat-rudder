#ifndef RELATED_POSTS_H
#define RELATED_POSTS_H

#include <stddef.h>

// The related-posts content block: one or two blog entries as cards. Its
// extra_data (utils/related_ids.h) is empty for the automatic mode - the
// newest published blog entry sharing a category with this one, at full width
// - or the one or two entries the author chose: two go side by side, left the
// previous article and right the next. It stores no text: titles, summaries
// and pictures are read from the entries, in the reader's language.

// What a block needs to know of the entry it sits in. Empty category list and
// NULL entry_id are fine: there is simply nothing to relate to.
typedef struct {
    const char        *entry_id;
    const char *const *category_ids;
    size_t             category_count;
} RelatedPostsContext;

// The block's HTML/WML for `epoch` in the reader's language, "" when there is
// nothing to show. Malloc'd; NULL on a missing template or allocation failure.
char *related_posts_render(const char *extra_data, const RelatedPostsContext *ctx, int epoch);

// The entry editor's picker: {"ok":true,"items":[{"id","title","date"}...]}
// for the entries of `ids_csv` (comma-separated, in that order) when given,
// else those whose title contains `query` (any, newest first, if empty),
// never `exclude_id`. Malloc'd JSON, NULL on allocation failure.
char *related_posts_lookup_json(const char *query, char *ids_csv, const char *exclude_id);

#endif // RELATED_POSTS_H
