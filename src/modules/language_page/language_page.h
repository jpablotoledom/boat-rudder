#ifndef LANGUAGE_PAGE_H
#define LANGUAGE_PAGE_H

#include <stddef.h>

// Builds the /language page: every configured content language as a plain
// link to /language/set, which stores the choice in a cookie and bounces the
// reader back to `return_url`. Plain links so the switch works with no
// JavaScript and no CSS - epoch 3's nav bar offers the same list as a
// drop-down, but every epoch can reach this page.
//
// `return_url` is the page to return to; it is validated by the caller and
// falls back to "/" when NULL or empty.
//
// Returns a malloc'd string, or NULL if a template is missing or on
// allocation failure. The caller must free() it.
char *language_page(int epoch, const char *return_url);

// Copies `raw` into `out` iff it is a safe in-site path to redirect to: it
// must start with a single '/' and contain no control characters. Anything
// else (absolute URLs, protocol-relative "//host", newlines) yields "/" -
// this is what stops /language/set from being used as an open redirect.
void language_sanitize_return(const char *raw, char *out, size_t out_size);

#endif // LANGUAGE_PAGE_H
