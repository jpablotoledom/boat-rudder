#ifndef REQUEST_CHARSET_H
#define REQUEST_CHARSET_H

// Whether *this* request's real browser needs the UTF-8-to-Latin-1 downgrade
// (build_epoch_response.c's utf8_to_latin1()) despite rendering under
// EPOCH_PRESTANDARD's own plain-text templates - a cross-cutting property
// the same way request_theme.h's theme key is (see that header's own doc
// comment on why this is a per-thread global instead of a parameter):
// content_type_for_epoch()/retrofit_body_for_epoch() only ever see the
// resolved `epoch` int, several calls removed from the original User-Agent
// header, and EPOCH_PRESTANDARD alone covers two audiences with opposite
// charset needs - a modern Lynx/w3m/ELinks running in a UTF-8 locale (the
// epoch's designed-for reader) and a genuinely pre-Unicode browser like
// Cello (1993) that only lands there because its own feature set (no inline
// images, see detect_epoch.c's own note) matches epoch 0's templates, not
// epoch 1's. This is resolved from the request's *real* User-Agent
// regardless of force_epoch/preview_epoch, which only ever pick which
// templates render, never what the actual visiting browser can decode - a
// forced/previewed epoch 0 is for previewing epoch 0 in whatever browser is
// doing the previewing, not for emulating Cello specifically.

// Resolves whether `user_agent` belongs to a browser needing this, and
// stores it for request_needs_legacy_charset(). Call once per request,
// alongside request_lang_set()/request_theme_set() in http_router.c.
void request_charset_set(const char *user_agent);

// True if the last request_charset_set() on this thread found a genuinely
// pre-Unicode browser. False (including before any request_charset_set()
// call on this thread) is the safe default - epoch 0's designed-for UTF-8
// terminal reader.
int request_needs_legacy_charset(void);

#endif // REQUEST_CHARSET_H
