// The comment form's challenge (src/utils/captcha.c): a token accepts its
// own answer only - in any case, once the minimum time has passed and before
// it expires - and nothing else; the image is a well-formed PNG.
//
// Run with: ctest --test-dir build/debug (or the binary directly).

#include "../src/utils/captcha.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect_int(const char *what, int got, int expected) {
    checks++;
    if (got != expected) {
        printf("FAIL %s: expected %d, got %d\n", what, expected, got);
        failures++;
    }
}

static void expect_true(const char *what, int condition) {
    checks++;
    if (!condition) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static unsigned int be32(const unsigned char *p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
}

int main(void) {
    if (sodium_init() < 0) return 1;
    unsigned char key[32], other_key[32];
    randombytes_buf(key, sizeof(key));
    randombytes_buf(other_key, sizeof(other_key));
    time_t now = time(NULL);
    char nonce[33];
    Captcha c;

    expect_int("issue", captcha_issue(key, "AC3FK", now - 10, &c), 0);
    expect_true("data uri", c.image_data_uri && strncmp(c.image_data_uri, "data:image/png;base64,", 22) == 0);
    expect_int("right answer", captcha_check(key, c.token, "AC3FK", now, nonce), CAPTCHA_OK);
    expect_true("nonce returned", strlen(nonce) == 32 && strncmp(nonce, c.token, 32) == 0);
    expect_int("lower case, spaces", captcha_check(key, c.token, " ac3 fk ", now, nonce), CAPTCHA_OK);
    expect_int("wrong answer", captcha_check(key, c.token, "AC3FX", now, nonce), CAPTCHA_WRONG);
    expect_int("empty answer", captcha_check(key, c.token, "", now, nonce), CAPTCHA_WRONG);
    expect_int("other key", captcha_check(other_key, c.token, "AC3FK", now, nonce), CAPTCHA_WRONG);
    expect_int("too fast", captcha_check(key, c.token, "AC3FK", now - 10 + CAPTCHA_MIN_SECS - 1, nonce), CAPTCHA_TOO_FAST);
    expect_int("expired", captcha_check(key, c.token, "AC3FK", now - 10 + CAPTCHA_TTL_SECS + 1, nonce), CAPTCHA_EXPIRED);

    // Moving the issue time forward to dodge the expiry breaks the mac.
    char tampered[CAPTCHA_TOKEN_MAX];
    snprintf(tampered, sizeof(tampered), "%.32s.%lld.%s", c.token, (long long)now, strrchr(c.token, '.') + 1);
    expect_int("tampered time", captcha_check(key, tampered, "AC3FK", now + 5, nonce), CAPTCHA_WRONG);
    expect_int("malformed", captcha_check(key, "abc.def", "AC3FK", now, nonce), CAPTCHA_MALFORMED);
    expect_int("null token", captcha_check(key, NULL, "AC3FK", now, nonce), CAPTCHA_MALFORMED);
    captcha_free(&c);

    // A fresh random challenge has a distinct token each time.
    Captcha a, b;
    captcha_new(key, &a);
    captcha_new(key, &b);
    expect_true("tokens differ", strcmp(a.token, b.token) != 0);
    captcha_free(&a);
    captcha_free(&b);

    size_t len = 0;
    unsigned char *png = captcha_png("XY479", &len);
    expect_true("png", png && len > 33);
    if (png) {
        expect_true("png signature", memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);
        expect_true("IHDR", memcmp(png + 12, "IHDR", 4) == 0);
        expect_int("width", (int)be32(png + 16), CAPTCHA_WIDTH);
        expect_int("height", (int)be32(png + 20), CAPTCHA_HEIGHT);
        expect_true("IEND last", memcmp(png + len - 8, "IEND", 4) == 0);
        FILE *f = fopen("captcha_test.png", "wb");
        if (f) { fwrite(png, 1, len, f); fclose(f); }
    }
    free(png);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
