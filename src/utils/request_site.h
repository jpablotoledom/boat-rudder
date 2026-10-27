#ifndef REQUEST_SITE_H
#define REQUEST_SITE_H

// Whether the request being served on this thread must carry this site's
// `?site=` key on everything it links to - stored per thread for the same
// reason request_theme.h's theme is: build_epoch_response.c, several layers
// below the router, needs it for every page and redirect.
//
// Behind a front that serves several sites on one address (multi-br's
// HAProxy), the Host header is what picks the site. A browser that sends
// none (HTTP/0.9, the earliest HTTP/1.0 browsers) names no site, so the
// front routes it by `?site=<key>` in the request line instead - and only as
// long as every link it follows carries it. Boat Rudder knows nothing of the
// other sites: it only adds its own key (config `site_key`) to its own links
// when the request had no Host. A browser that sends Host never sees it.

// Records whether the request had a Host header (`host_header` NULL or
// empty: it had none). Call once per request, alongside request_theme_set()
// in http_router.c.
void request_site_set(const char *host_header);

// This site's key when the request had no Host and `site_key` is
// configured - the value to stitch onto links as "site=<key>" - else NULL.
const char *request_site(void);

// Appends "site=<key>" to the site-relative `location` when request_site()
// says so; otherwise (or for any other location) returns a plain copy.
// Returns a malloc'd string, or NULL on allocation failure.
char *request_site_tag_location(const char *location);

// Validates a site key: 1-32 of a-z 0-9 _ -, as multi-br's site names.
int request_site_key_is_valid(const char *key);

#endif // REQUEST_SITE_H
