#ifndef BUILD_EPOCH_RESPONSE_H
#define BUILD_EPOCH_RESPONSE_H

// Builds a complete HTTP/1.1 200 OK response (status line, headers and body)
// for `body`, choosing Content-Type based on `epoch`:
//   EPOCH_WML                  -> text/vnd.wap.wml
//   EPOCH_PRESTANDARD/EARLY     -> text/html
//   EPOCH_MIDDLE/MODERN         -> text/html; charset=UTF-8
//
// `extra_headers` (may be "") is inserted verbatim before the final blank
// line, each line including its own "\r\n".
//
// Returns a malloc'd, NUL-terminated buffer, or NULL on allocation failure.
// The caller must free() the returned buffer.
char *build_epoch_response(const char *body, const char *extra_headers, int epoch);

// Same as build_epoch_response(), but with an explicit HTTP status line
// (e.g. "404 Not Found", "401 Unauthorized") instead of the implicit
// "200 OK". Used by the centralized error templates (src/modules/error).
char *build_epoch_response_status(const char *body, const char *extra_headers,
                                   int epoch, const char *status_line);

// Builds a "302 Found" response redirecting to `location`, with Content-Type
// chosen per `epoch` and a tiny epoch-appropriate body containing a link to
// `location` (for clients/old WAP phones that do not auto-follow redirects).
// `extra_headers` (may be "") is inserted verbatim, e.g. "Set-Cookie: ...\r\n".
//
// Returns a malloc'd, NUL-terminated buffer, or NULL on allocation failure.
// The caller must free() the returned buffer.
char *build_redirect_response(const char *location, const char *extra_headers, int epoch);

// Builds a complete HTTP/1.1 response with `status_line` (e.g. "200 OK",
// "400 Bad Request"), Content-Type: application/json; charset=UTF-8, and
// `json_body` as the body. Used by the /dashboard/api/* AJAX routes (epoch3
// editor only - no per-epoch Content-Type).
//
// Returns a malloc'd, NUL-terminated buffer, or NULL on allocation failure.
// The caller must free() the returned buffer.
char *build_json_response_status(const char *json_body, const char *status_line);

// Same as build_json_response_status(), with status line "200 OK".
char *build_json_response(const char *json_body);

#endif // BUILD_EPOCH_RESPONSE_H
