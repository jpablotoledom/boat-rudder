#ifndef REQUEST_USER_H
#define REQUEST_USER_H

// The logged-in user's display name for the request being served on this
// thread, if any - a cross-cutting property the same way request_lang.h's
// language is: menu() (several layers below the router) shows it on epoch
// 3's navbar when there is an active session, and nothing when there is
// not. Rather than thread a user id through every buildXWebSite() call
// site, it is resolved once per request and stored per thread (one
// detached thread per connection, so a thread never serves two requests at
// once - same reasoning as request_lang.h).

// Resolves the session in `cookie_header` (may be NULL), if any, and
// stores its user's display name for request_user_name(). Call once per
// request, alongside request_lang_set()/request_path_set().
void request_user_set(const char *cookie_header);

// The display name resolved by the last request_user_set() on this thread,
// or "" if there is no valid session (including if the setter has not run
// on this thread yet). Never NULL. The pointer stays valid until the next
// request_user_set() on the same thread.
const char *request_user_name(void);

#endif // REQUEST_USER_H
