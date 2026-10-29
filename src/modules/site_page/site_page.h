#ifndef SITE_PAGE_H
#define SITE_PAGE_H

// Builds the site chooser: every site in the `site_directory` config as a
// plain link to "/?site=<key>". Shown in place of the home page to a browser
// that sent no Host header and so named no site - see utils/request_site.h.
// Plain links, like language_page.h's, so it works in every epoch.
//
// Returns a malloc'd string, or NULL if a template is missing, the
// directory lists no valid site, or on allocation failure. The caller must
// free() it.
char *site_page(int epoch);

// Whether the request for "/" gets the chooser instead of the home page: it
// had no Host (request_site() is set), it names no site yet (`site_query`,
// the raw `?site=` value, is NULL or empty) and a directory is configured.
int site_page_applies(const char *site_query);

#endif // SITE_PAGE_H
