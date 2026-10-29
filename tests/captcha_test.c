// The comment form's challenge (src/utils/captcha.c): a token accepts its
// own answer only - in any case, once the minimum time has passed and before
// it expires - and nothing else; the answer comes from the token under the
// key; a token always draws the same image; the PNG and GIF are well formed.
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

static unsigned int le16(const unsigned char *p) { return (unsigned int)p[0] | (unsigned int)p[1] << 8; }

int main(void) {
    if (sodium_init() < 0) return 1;
    unsigned char key[32], other_key[32];
    randombytes_buf(key, sizeof(key));
    randombytes_buf(other_key, sizeof(other_key));
    time_t now = time(NULL);
    char nonce[33], answer[CAPTCHA_LENGTH + 1], wrong[CAPTCHA_LENGTH + 1];
    Captcha c;

    expect_int("issue", captcha_issue(key, now - 10, 1, &c), 0);
    expect_true("data uri", c.image_data_uri && strncmp(c.image_data_uri, "data:image/png;base64,", 22) == 0);
    expect_int("answer from the token", captcha_answer(key, c.token, answer), 0);
    expect_int("no answer under another key", captcha_answer(other_key, c.token, wrong), -1);
    expect_int("answer length", (int)strlen(answer), CAPTCHA_LENGTH);
    expect_true("answer not in the token", strstr(c.token, answer) == NULL);

    char lower[16];
    snprintf(lower, sizeof(lower), " %c%c%c %c%c ", answer[0] | 0x20, answer[1] | 0x20, answer[2] | 0x20,
             answer[3] | 0x20, answer[4] | 0x20);
    memcpy(wrong, answer, sizeof(wrong));
    wrong[4] = wrong[4] == 'A' ? 'C' : 'A';

    expect_int("right answer", captcha_check(key, c.token, answer, now, nonce), CAPTCHA_OK);
    expect_true("nonce returned", strlen(nonce) == 32 && strncmp(nonce, c.token, 32) == 0);
    expect_int("lower case, spaces", captcha_check(key, c.token, lower, now, nonce), CAPTCHA_OK);
    expect_int("wrong answer", captcha_check(key, c.token, wrong, now, nonce), CAPTCHA_WRONG);
    expect_int("empty answer", captcha_check(key, c.token, "", now, nonce), CAPTCHA_WRONG);
    expect_int("other key", captcha_check(other_key, c.token, answer, now, nonce), CAPTCHA_WRONG);
    expect_int("too fast", captcha_check(key, c.token, answer, now - 10 + CAPTCHA_MIN_SECS - 1, nonce), CAPTCHA_TOO_FAST);
    expect_int("expired", captcha_check(key, c.token, answer, now - 10 + CAPTCHA_TTL_SECS + 1, nonce), CAPTCHA_EXPIRED);

    // Moving the issue time forward to dodge the expiry breaks the mac.
    char tampered[CAPTCHA_TOKEN_MAX];
    snprintf(tampered, sizeof(tampered), "%.32s.%lld.%s", c.token, (long long)now, strrchr(c.token, '.') + 1);
    expect_int("tampered time", captcha_check(key, tampered, answer, now + 5, nonce), CAPTCHA_WRONG);
    expect_int("malformed", captcha_check(key, "abc.def", answer, now, nonce), CAPTCHA_MALFORMED);
    expect_int("null token", captcha_check(key, NULL, answer, now, nonce), CAPTCHA_MALFORMED);

    // The image by URL: the token's own, the same every time, only while valid.
    size_t g1_len = 0, g2_len = 0, n = 0;
    unsigned char *g1 = captcha_token_gif(key, c.token, now, &g1_len);
    unsigned char *g2 = captcha_token_gif(key, c.token, now, &g2_len);
    expect_true("token gif", g1 && g2);
    expect_true("same token, same image", g1 && g2 && g1_len == g2_len && memcmp(g1, g2, g1_len) == 0);
    expect_true("expired token, no image", captcha_token_gif(key, c.token, now - 10 + CAPTCHA_TTL_SECS + 1, &n) == NULL);
    expect_true("other key, no image", captcha_token_gif(other_key, c.token, now, &n) == NULL);
    expect_true("tampered token, no image", captcha_token_gif(key, tampered, now, &n) == NULL);
    expect_true("malformed token, no image", captcha_token_gif(key, "abc.def", now, &n) == NULL);
    if (g1) {
        expect_true("GIF87a", memcmp(g1, "GIF87a", 6) == 0);
        expect_int("gif width", (int)le16(g1 + 6), CAPTCHA_WIDTH);
        expect_int("gif height", (int)le16(g1 + 8), CAPTCHA_HEIGHT);
        expect_int("16-color table", g1[10], 0xB3);
        expect_int("image descriptor", g1[13 + 48], 0x2C);
        expect_int("trailer", g1[g1_len - 1], 0x3B);
        expect_true("small enough", g1_len < 12000);
        FILE *f = fopen("captcha_test.gif", "wb");
        if (f) { fwrite(g1, 1, g1_len, f); fclose(f); }
    }
    free(g1);
    free(g2);

    // The text art for epoch 0: the same token's, the same every time.
    char *t1 = captcha_token_text(key, c.token, now);
    char *t2 = captcha_token_text(key, c.token, now);
    expect_true("token text", t1 && t2);
    expect_true("same token, same text", t1 && t2 && strcmp(t1, t2) == 0);
    expect_true("expired token, no text", captcha_token_text(key, c.token, now - 10 + CAPTCHA_TTL_SECS + 1) == NULL);
    expect_true("other key, no text", captcha_token_text(other_key, c.token, now) == NULL);
    if (t1) {
        int lines = 0, widest = 0, col = 0, ink = 0;
        for (const char *p = t1; *p; p++) {
            if (*p == '\n') { lines++; if (col > widest) widest = col; col = 0; continue; }
            col++;
            if (strchr("#%@$MWX8", *p)) ink++;
        }
        expect_int("text lines", lines, 9);
        expect_true("fits 80 columns", widest <= 80);
        expect_true("has ink", ink > 50);
        expect_true("nothing to escape", strpbrk(t1, "<>&\"") == NULL);
    }
    free(t1);
    free(t2);
    captcha_free(&c);

    // Without inline_png, only the token; fresh challenges differ.
    Captcha a, b;
    captcha_new(key, 0, &a);
    captcha_new(key, 0, &b);
    expect_true("no data uri unless asked", a.image_data_uri == NULL);
    expect_true("tokens differ", strcmp(a.token, b.token) != 0);
    size_t ga_len = 0, gb_len = 0;
    unsigned char *ga = captcha_token_gif(key, a.token, now, &ga_len);
    unsigned char *gb = captcha_token_gif(key, b.token, now, &gb_len);
    expect_true("different tokens, different images", ga && gb && (ga_len != gb_len || memcmp(ga, gb, ga_len) != 0));
    free(ga);
    free(gb);
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
