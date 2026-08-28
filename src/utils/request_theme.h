#ifndef REQUEST_THEME_H
#define REQUEST_THEME_H

// The active theme key ("dark", ...) for the request being served on this
// thread - a cross-cutting property the same way request_lang.h's language
// is: generate_url_theme() (several layers below the router, in nearly
// every module) needs it on every call. Rather than thread it through every
// signature, it is resolved once per request and stored per thread (one
// detached thread per connection, so a thread never serves two requests at
// once - same reasoning as request_lang.h).

// Resolves the active theme (site_settings.active_theme if set and its
// directory exists under html/themes/, otherwise configs/settings.conf's
// `theme`) and stores it for request_theme(). Call once per request,
// alongside request_lang_set()/request_user_set() in http_router.c.
void request_theme_set(void);

// The theme key resolved by the last request_theme_set() on this thread,
// or configs/settings.conf's `theme` if the setter has not run on this
// thread yet. Never NULL/empty. The pointer stays valid until the next
// request_theme_set() on the same thread.
const char *request_theme(void);

#endif // REQUEST_THEME_H
