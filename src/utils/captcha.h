#ifndef CAPTCHA_H
#define CAPTCHA_H

#include <stddef.h>
#include <time.h>

// Boat Rudder's own challenge for the comment form: 5 characters drawn
// here (a built-in 5x7 font, each glyph scaled, slanted and shifted at
// random, the whole image waved, crossed by lines and sprinkled with dots)
// and encoded as a PNG with no image library.
//
// Stateless: the form carries a token "nonce.issued.mac", where mac is
// HMAC-SHA256(key, "captcha|nonce|issued|ANSWER"). Nothing is stored until
// an answer comes back; making each token single-use (by its nonce) is the
// caller's job - see cms_comments.h.

#define CAPTCHA_LENGTH     5
#define CAPTCHA_TTL_SECS   900   // a token answered later than this is expired
#define CAPTCHA_MIN_SECS   3     // and sooner than this, too fast for a person
#define CAPTCHA_TOKEN_MAX  128
#define CAPTCHA_WIDTH      200
#define CAPTCHA_HEIGHT     70

typedef struct {
    char  token[CAPTCHA_TOKEN_MAX];
    char *image_data_uri;          // "data:image/png;base64,..." (malloc'd)
} Captcha;

typedef enum {
    CAPTCHA_OK = 0,
    CAPTCHA_WRONG,
    CAPTCHA_EXPIRED,
    CAPTCHA_TOO_FAST,
    CAPTCHA_MALFORMED,
} CaptchaResult;

// A fresh challenge. Returns 0, -1 on allocation failure. captcha_free() it.
int captcha_new(const unsigned char key[32], Captcha *out);

// The same for a given text and time - what captcha_new() uses; exposed for
// tests. `text` must be CAPTCHA_LENGTH characters of the challenge alphabet.
int captcha_issue(const unsigned char key[32], const char *text, time_t issued, Captcha *out);

void captcha_free(Captcha *c);

// Checks `answer` (case and spaces don't matter) against `token` at time
// `now`. On CAPTCHA_OK the token's nonce (32 hex + NUL) is copied to
// `nonce_out`, for the caller to mark it used.
CaptchaResult captcha_check(const unsigned char key[32], const char *token, const char *answer,
                            time_t now, char nonce_out[33]);

// The PNG alone (malloc'd, *len bytes), for tests.
unsigned char *captcha_png(const char *text, size_t *len);

#endif // CAPTCHA_H
