#ifndef ERROR_H
#define ERROR_H

// Renders error/error_epoch<N>.html with `status_code` and `message`
// substituted into its two %s placeholders (in that order).
//
// If `message` is NULL, a default message for `status_code` is used (see
// the static table in error.c); unrecognized codes fall back to "Error".
//
// Returns a malloc'd HTML/WML fragment (to be wrapped via
// buildPageWebSite()), or NULL on failure (missing template or allocation
// failure) - the caller should fall back to a hardcoded minimal response.
char *error_content(int epoch, int status_code, const char *message);

#endif // ERROR_H
