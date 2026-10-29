#ifndef CAPTCHA_H
#define CAPTCHA_H

#include <stddef.h>
#include <time.h>

// Boat Rudder's own challenge for the comment form: 5 characters drawn
// here (a built-in 5x7 font, each glyph scaled, slanted and shifted, the
// whole image waved, crossed by lines and sprinkled with dots) and encoded
// as a PNG (inline, epoch 3), a GIF (served by URL, epochs 1-2) or text art
// (inline in a <pre>, epoch 0's text browsers) with no image library.
//
// Stateless: the form carries a token "nonce.issued.mac". The answer is
// derived from it under the key - HMAC-SHA256(key, "captcha-text|nonce|
// issued") mapped onto the alphabet - and mac is HMAC-SHA256(key,
// "captcha|nonce|issued|ANSWER"). So the answer is in no token and no
// record, yet the image can be drawn again from the token alone; its
// randomness is seeded from the token too, so a token always gives the same
// image. Nothing is stored until an answer comes back; making each token
// single-use (by its nonce) is the caller's job - see cms_comments.h.

#define CAPTCHA_LENGTH     5
#define CAPTCHA_TTL_SECS   900   // a token answered later than this is expired
#define CAPTCHA_MIN_SECS   3     // and sooner than this, too fast for a person
#define CAPTCHA_TOKEN_MAX  128
#define CAPTCHA_WIDTH      200
#define CAPTCHA_HEIGHT     70

typedef struct {
    char  token[CAPTCHA_TOKEN_MAX];
    char *image_data_uri;          // "data:image/png;base64,..." (malloc'd), NULL unless asked for
} Captcha;

typedef enum {
    CAPTCHA_OK = 0,
    CAPTCHA_WRONG,
    CAPTCHA_EXPIRED,
    CAPTCHA_TOO_FAST,
    CAPTCHA_MALFORMED,
} CaptchaResult;

// A fresh challenge; with `inline_png`, its image as a data: URI too (else
// the image is served by captcha_token_gif()). Returns 0, -1 on allocation
// failure. captcha_free() it.
int captcha_new(const unsigned char key[32], int inline_png, Captcha *out);

// The same, issued at a given time - what captcha_new() uses; for tests.
int captcha_issue(const unsigned char key[32], time_t issued, int inline_png, Captcha *out);

void captcha_free(Captcha *c);

// Checks `answer` (case and spaces don't matter) against `token` at time
// `now`. On CAPTCHA_OK the token's nonce (32 hex + NUL) is copied to
// `nonce_out`, for the caller to mark it used.
CaptchaResult captcha_check(const unsigned char key[32], const char *token, const char *answer,
                            time_t now, char nonce_out[33]);

// The image of `token` as a GIF (malloc'd, *len bytes): NULL unless the
// token is the key's own and not expired at `now`.
unsigned char *captcha_token_gif(const unsigned char key[32], const char *token, time_t now, size_t *len);

// The image of `token` as text art for a <pre> (malloc'd, 9 lines of at
// most 80 columns, no character that needs HTML escaping), on the same
// terms as captcha_token_gif(): NULL unless genuine and not expired.
char *captcha_token_text(const unsigned char key[32], const char *token, time_t now);

// The answer behind `token` (CAPTCHA_LENGTH + NUL) if it is the key's own:
// 0, else -1. For tests - the server never needs to read it.
int captcha_answer(const unsigned char key[32], const char *token, char out[CAPTCHA_LENGTH + 1]);

// The PNG / GIF of a text alone (malloc'd, *len bytes), for tests.
unsigned char *captcha_png(const char *text, size_t *len);
unsigned char *captcha_gif(const char *text, size_t *len);
char *captcha_text_art(const char *text);

#endif // CAPTCHA_H
