#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <stddef.h>

// Cookie name used for the session token.
#define SESSION_COOKIE_NAME "session"

// 32 random bytes, hex-encoded: 64 chars + NUL.
#define SESSION_TOKEN_BUF_SIZE 65

// bson ObjectId as hex string: 24 chars + NUL.
#define USER_ID_HEX_BUF_SIZE 25

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

#endif // SESSION_MANAGER_H
