#ifndef REQUEST_LANG_H
#define REQUEST_LANG_H

#include <stddef.h>

// Content language for the request being served on this thread.
//
// The language is a cross-cutting property of a request: the router needs it
// to resolve `map<lang,string>` fields, and so does the menu, several layers
// down a call chain that does not carry it (buildPageWebSite() alone has ~26
// call sites). Rather than thread the value through every signature, it is
// stored per thread - the server runs one detached thread per connection, so
// a thread never serves two requests at once.

// Resolves the language for this request and stores it for request_lang().
// `lang_query` (may be NULL) is the raw `?lang=` query value, if any, and
// wins over everything else when it names a configured language: it is how
// epoch 0/1 switch language at all, since those templates link straight to
// "<page>?lang=xx" instead of the cookie-setting /language/set redirect (see
// language_page.c - HTTP/1.0-era clients, including real NCSA Mosaic builds,
// take a redirect's Location literally as the whole next request and choke
// on anything that isn't a full absolute URI, which a same-origin app has no
// reliable way to always supply). Otherwise `cookie_header` (the raw
// `Cookie:` header, may be NULL) is used: a `lang` cookie naming a
// configured language wins, otherwise the site default
// (cms_resolve_default_lang()) is used. Call once per request, before
// rendering anything.
void request_lang_set(const char *cookie_header, const char *lang_query);

// The language resolved by the last request_lang_set() on this thread, as an
// ISO 639-1 code. Never NULL; returns the site default if the setter has not
// run on this thread yet. The pointer stays valid until the next
// request_lang_set() on the same thread.
const char *request_lang(void);

// Stores the path of the request being served on this thread ("/blog/x"),
// for code that needs the actual URL rather than the menu's `current_url` -
// which is a *section* ("/blog" for every article, so the menu can highlight
// one item) and would send a reader switching language on an article back to
// the listing. Call once per request, alongside request_lang_set().
void request_path_set(const char *path);

// The path stored by the last request_path_set() on this thread, or "/" if
// none. Never NULL; valid until the next request_path_set() on this thread.
const char *request_path(void);

// Copies `code` into `out` iff it names a configured language, and returns 1.
// Returns 0 and leaves `out` untouched otherwise - used to validate the code
// coming from /language/set before it is written to a cookie.
int request_lang_validate(const char *code, char *out, size_t out_size);

#endif // REQUEST_LANG_H
