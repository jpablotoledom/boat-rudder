#ifndef REQUEST_WML_H
#define REQUEST_WML_H

// Per-request state for paginating epoch -1 (WML) responses in
// build_epoch_response.c, set once per request from http_router.c like
// request_theme_set().
//
// `raw_target` is the request line's own path?query (still URL-encoded) -
// the base the "<< Prev"/"Next >>" links are rebuilt from. `wml_pages` is
// the "wml_pages" query value: "all" asks for the whole deck unpaginated,
// which is what the WAP gateway (src/wap_gateway/) requests, since it
// paginates into single packets itself.
void request_wml_set(const char *raw_target, const char *wml_pages);
const char *request_wml_target(void);   // never NULL
int request_wml_unpaged(void);

#endif
