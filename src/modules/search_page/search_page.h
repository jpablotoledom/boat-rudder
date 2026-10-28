#ifndef SEARCH_PAGE_H
#define SEARCH_PAGE_H

// /search?q=...: the search form (search/search_epoch<N>.html) with the
// published blog entries and pages matching `raw_query` (as decoded from the
// query string; may be NULL or empty - then just the form). Every epoch.
// Returns a malloc'd fragment for buildPageWebSite(), or NULL on failure.
char *search_page(int epoch, const char *raw_query);

#endif // SEARCH_PAGE_H
