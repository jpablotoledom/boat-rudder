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

#endif // BUILD_EPOCH_RESPONSE_H
