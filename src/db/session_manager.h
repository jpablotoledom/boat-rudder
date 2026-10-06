#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <stddef.h>

// Cookie name used for the session token.
#define SESSION_COOKIE_NAME "session"

// 32 random bytes, hex-encoded: 64 chars + NUL.
#define SESSION_TOKEN_BUF_SIZE 65

// bson ObjectId as hex string: 24 chars + NUL.
#define USER_ID_HEX_BUF_SIZE 25

// CSRF token: 32-byte keyed BLAKE2b of the session token, hex-encoded.
#define CSRF_TOKEN_BUF_SIZE 65

// Name of the request header (AJAX) and form field (HTML forms, urlencoded
// or multipart) that carry the CSRF token on dashboard POSTs.
#define CSRF_HEADER_NAME "X-CSRF-Token"
#define CSRF_FIELD_NAME  "csrf_token"

// Generates a new session token: 32 random bytes (libsodium CSPRNG),
// hex-encoded into a malloc'd, NUL-terminated string of
// SESSION_TOKEN_BUF_SIZE bytes. Caller must free(). Returns NULL on
// allocation failure.
char *generate_session_token(void);

// Inserts {user_id: ObjectId(user_id_hex), token, created_at,
// expires_at = now + ttl_seconds} into the `sessions` collection.
// Returns 0 on success, -1 on error (invalid user_id, no DB, ...).
int create_session(const char *user_id_hex, const char *token, int ttl_seconds);

// Looks up `token` in `sessions`. If found and not expired, writes the
// 24-char hex user_id into user_id_out (>= USER_ID_HEX_BUF_SIZE bytes) and
// returns 1. Returns 0 if not found/expired, -1 on DB error.
int validate_session(const char *token, char *user_id_out);

// Extracts the SESSION_COOKIE_NAME value from a raw "Cookie:" header value
// into token_out (>= SESSION_TOKEN_BUF_SIZE bytes). Returns 1 if found
// (token_out is NUL-terminated), 0 otherwise (cookie_header may be NULL).
int extract_session_token(const char *cookie_header, char *token_out, size_t size);

// extract_session_token() + validate_session(). Returns 1 if `cookie_header`
// contains a valid, non-expired session (user_id_out filled), 0 if missing/
// invalid/expired, -1 on DB error.
int validate_session_cookie(const char *cookie_header, char *user_id_out);

// Deletes the session document matching `token` (used by /logout). Returns
// 0 on success (including "not found"), -1 on DB error.
int destroy_session(const char *token);

// Builds a "Set-Cookie: ..." header line (including trailing "\r\n") that
// sets the session cookie to `token` with the given TTL. Adds "; Secure"
// when ssl_enabled is set in configs/settings.conf.
void build_session_cookie_header(const char *token, int ttl_seconds, char *header_out, size_t size);

// Builds a "Set-Cookie: ..." header line (including trailing "\r\n") that
// clears the session cookie (Max-Age=0).
void build_session_clear_cookie_header(char *header_out, size_t size);

// Derives the CSRF token bound to `session_token` into token_out (>=
// CSRF_TOKEN_BUF_SIZE bytes): a keyed BLAKE2b hash, so it is stable for the
// session's lifetime, needs no storage, and cannot be computed without the
// (HttpOnly) session cookie. Returns 1 on success, 0 if session_token is
// NULL/empty.
int derive_csrf_token(const char *session_token, char *token_out);

// Constant-time check that `supplied` is the CSRF token of the session in
// `cookie_header`. Returns 1 on match, 0 otherwise (missing cookie, missing
// or malformed token, mismatch).
int verify_csrf_token(const char *cookie_header, const char *supplied);

// Creates the `sessions` indexes the code relies on - a unique index on
// `token` and a TTL index on `expires_at` (expireAfterSeconds: 0) so MongoDB
// purges expired sessions on its own - and deletes the sessions that are
// already expired. Idempotent; failures are logged, not fatal. Call once at
// startup after mongodb_manager_init().
void session_manager_ensure_indexes(void);

#endif // SESSION_MANAGER_H
