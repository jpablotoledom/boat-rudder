#ifndef CMS_SEARCH_H
#define CMS_SEARCH_H

#include <stddef.h>

// Site search (/search): published (enabled) "blog" and "page" entries whose
// title, summary or title/paragraph/list blocks contain the query, in the
// reader's language - the same text they would read on the entry, so a hit
// never comes from a translation they can't see.

#define SEARCH_QUERY_MIN       2     // bytes, after trimming
#define SEARCH_QUERY_MAX       100   // bytes
#define SEARCH_RESULTS_LIMIT   30
#define SEARCH_CANDIDATES_MAX  200   // entries MongoDB pre-selects before the exact check

typedef struct {
    char *link;          // entries.link
    char *type;          // "blog" | "page"
    char *title;         // header title, or the entry's first title block
    char  date[16];      // "YYYY-MM-DD" ("" for none)
    // A plain-text excerpt around the first match: before + match + after.
    // The match is the text as written in the entry (its own capitals).
    char *before;
    char *match;
    char *after;
} CmsSearchResult;

// Searches for `query` (UTF-8, already trimmed; case-insensitive for ASCII
// and Latin-1 letters) in `lang` (ISO 639-1, with the entries' usual "en"
// fallback), newest first. On success *out is a malloc'd array of *count
// results (possibly 0) for cms_search_free() and 0 is returned; -1 on a DB
// error or MongoDB not ready (*out NULL, *count 0).
int cms_search_entries(const char *query, const char *lang, CmsSearchResult **out, size_t *count);

void cms_search_free(CmsSearchResult *results, size_t count);

#endif // CMS_SEARCH_H
