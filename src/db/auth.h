#ifndef AUTH_H
#define AUTH_H

// Verifies `email`/`password` against the `users` collection (field
// "password" holds a libsodium crypto_pwhash_str() Argon2id hash).
//
// On success, returns a malloc'd 24-char ObjectId hex string (+ NUL) -
// the caller must free() it. Returns NULL if the email is unknown, the
// password does not match, or on a DB error - all three cases are
// indistinguishable to the caller, to avoid user enumeration.
char *auth_login_user(const char *email, const char *password);

#endif // AUTH_H
