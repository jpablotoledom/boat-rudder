#ifndef LOGIN_H
#define LOGIN_H

// Builds the /login page content fragment for `epoch`.
//
// epoch == EPOCH_MODERN: loads login_epoch3.html and fills its single %s
//   placeholder with an error message block built from `error_message`
//   (NULL or "" -> no error block).
// epoch != EPOCH_MODERN: loads login_epoch<N>.html and returns it verbatim
//   (login is restricted to EPOCH_MODERN; `error_message` is ignored, since
//   there is no form to show an error on).
//
// Returns a malloc'd HTML/WML fragment, or NULL on failure (missing template
// or allocation failure). The caller must free() the returned buffer.
char *login(int epoch, const char *error_message);

#endif // LOGIN_H
