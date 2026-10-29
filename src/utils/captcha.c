#include "captcha.h"
#include <ctype.h>
#include <math.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// No look-alikes: no 0/O/Q/D-ish pairs beyond D, no 1/I/L, 2/Z, 5/S, 8/B, G/6.
static const char ALPHABET[] = "ACDEFHJKMNPRTUVWXY34679";

// 5x7 glyphs, one row per byte, bit 4 = leftmost column.
static const struct { char c; uint8_t rows[7]; } FONT[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
};

static const uint8_t *glyph(char c) {
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
        if (FONT[i].c == c) return FONT[i].rows;
    return NULL;
}

// Uniform in [lo, hi].
static int rnd(int lo, int hi) {
    return lo + (int)randombytes_uniform((uint32_t)(hi - lo + 1));
}

// ---- Drawing --------------------------------------------------------------------

#define W CAPTCHA_WIDTH
#define H CAPTCHA_HEIGHT

static void put(uint8_t *img, int x, int y, uint8_t v) {
    if (x >= 0 && x < W && y >= 0 && y < H) img[y * W + x] = v;
}

static void line(uint8_t *img, int x0, int y0, int x1, int y1, uint8_t v) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(img, x0, y0, v);
        put(img, x0, y0 + 1, v);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draws `text` into a W x H grayscale buffer (0 black .. 255 white).
static void draw(const char *text, uint8_t *out) {
    uint8_t *img = malloc((size_t)W * H);
    if (!img) { memset(out, 255, (size_t)W * H); return; }
    for (int i = 0; i < W * H; i++) img[i] = (uint8_t)rnd(215, 255);

    int len = (int)strlen(text);
    int step = (W - 20) / (len > 0 ? len : 1);
    for (int i = 0; i < len; i++) {
        const uint8_t *g = glyph(text[i]);
        if (!g) continue;
        int sx = rnd(4, 5), sy = rnd(6, 7);
        double shear = rnd(-20, 20) / 100.0;
        int x0 = 10 + i * step + rnd(-3, 3);
        int y0 = (H - 7 * sy) / 2 + rnd(-6, 6);
        uint8_t ink = (uint8_t)rnd(10, 60);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                int bx = x0 + c * sx + (int)lround(shear * (3 - r) * sy);
                int by = y0 + r * sy;
                for (int yy = 0; yy < sy; yy++)
                    for (int xx = 0; xx < sx; xx++)
                        if (rnd(0, 29) > 0) put(img, bx + xx, by + yy, ink);   // a few holes
            }
    }

    for (int i = 0; i < 3; i++)
        line(img, rnd(0, W / 4), rnd(0, H - 1), rnd(3 * W / 4, W - 1), rnd(0, H - 1), (uint8_t)rnd(90, 160));
    for (int i = 0; i < 250; i++) put(img, rnd(0, W - 1), rnd(0, H - 1), (uint8_t)rnd(60, 180));

    // One sine wave across the image, so glyphs don't sit on straight lines.
    double amp = rnd(15, 30) / 10.0, period = rnd(70, 110), phase = rnd(0, 628) / 100.0;
    for (int x = 0; x < W; x++) {
        int shift = (int)lround(amp * sin(2 * M_PI * x / period + phase));
        for (int y = 0; y < H; y++) {
            int src = y + shift;
            out[y * W + x] = (src >= 0 && src < H) ? img[src * W + x] : (uint8_t)rnd(215, 255);
        }
    }
    free(img);
}

// ---- PNG ------------------------------------------------------------------------

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// Appends a chunk (length, type, data, CRC over type + data).
static uint8_t *chunk(uint8_t *w, const char *type, const uint8_t *data, uint32_t n) {
    be32(w, n);
    memcpy(w + 4, type, 4);
    if (n) memcpy(w + 8, data, n);
    be32(w + 8 + n, crc32_of(w + 4, n + 4, 0));
    return w + 12 + n;
}

// 8-bit grayscale PNG, zlib stream of stored (uncompressed) deflate blocks:
// a 14 KB image needs no compressor.
static unsigned char *encode_png(const uint8_t *gray, size_t *len) {
    size_t raw_len = (size_t)H * (W + 1);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return NULL;
    for (int y = 0; y < H; y++) {
        raw[y * (W + 1)] = 0;   // filter: none
        memcpy(raw + y * (W + 1) + 1, gray + y * W, W);
    }

    size_t blocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len);
    size_t png_cap = 8 + 25 + 12 + z_len + 12;
    uint8_t *png = z ? malloc(png_cap) : NULL;
    if (!png) { free(raw); free(z); return NULL; }

    uint8_t *w = z;
    *w++ = 0x78; *w++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; ) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *w++ = off + n == raw_len ? 1 : 0;
        *w++ = (uint8_t)n; *w++ = (uint8_t)(n >> 8);
        *w++ = (uint8_t)~n; *w++ = (uint8_t)(~n >> 8);
        memcpy(w, raw + off, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
        w += n;
        off += n;
    }
    be32(w, (b << 16) | a);
    w += 4;
    size_t z_used = (size_t)(w - z);

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t ihdr[13];
    be32(ihdr, W);
    be32(ihdr + 4, H);
    ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    uint8_t *p = png;
    memcpy(p, sig, 8);
    p += 8;
    p = chunk(p, "IHDR", ihdr, 13);
    p = chunk(p, "IDAT", z, (uint32_t)z_used);
    p = chunk(p, "IEND", NULL, 0);
    *len = (size_t)(p - png);

    free(raw);
    free(z);
    return png;
}

unsigned char *captcha_png(const char *text, size_t *len) {
    uint8_t *gray = malloc((size_t)W * H);
    if (!gray) return NULL;
    draw(text, gray);
    unsigned char *png = encode_png(gray, len);
    free(gray);
    return png;
}

// ---- Token ----------------------------------------------------------------------

static void mac_hex(const unsigned char key[32], const char *nonce, long long issued, const char *answer,
                    char out[65]) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "captcha|%s|%lld|%s", nonce, issued, answer);
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char *)msg, (unsigned long long)n, key);
    sodium_bin2hex(out, 65, mac, sizeof(mac));
}

int captcha_issue(const unsigned char key[32], const char *text, time_t issued, Captcha *out) {
    memset(out, 0, sizeof(*out));
    unsigned char raw[16];
    char nonce[33], mac[65];
    randombytes_buf(raw, sizeof(raw));
    sodium_bin2hex(nonce, sizeof(nonce), raw, sizeof(raw));
    mac_hex(key, nonce, (long long)issued, text, mac);
    snprintf(out->token, sizeof(out->token), "%s.%lld.%s", nonce, (long long)issued, mac);

    size_t png_len = 0;
    unsigned char *png = captcha_png(text, &png_len);
    if (!png) return -1;
    size_t b64_len = sodium_base64_encoded_len(png_len, sodium_base64_VARIANT_ORIGINAL);
    out->image_data_uri = malloc(b64_len + 32);
    if (out->image_data_uri) {
        strcpy(out->image_data_uri, "data:image/png;base64,");
        sodium_bin2base64(out->image_data_uri + strlen(out->image_data_uri), b64_len, png, png_len,
                          sodium_base64_VARIANT_ORIGINAL);
    }
    free(png);
    return out->image_data_uri ? 0 : -1;
}

int captcha_new(const unsigned char key[32], Captcha *out) {
    char text[CAPTCHA_LENGTH + 1];
    for (int i = 0; i < CAPTCHA_LENGTH; i++)
        text[i] = ALPHABET[randombytes_uniform((uint32_t)(sizeof(ALPHABET) - 1))];
    text[CAPTCHA_LENGTH] = '\0';
    int rc = captcha_issue(key, text, time(NULL), out);
    sodium_memzero(text, sizeof(text));
    return rc;
}

void captcha_free(Captcha *c) {
    if (!c) return;
    free(c->image_data_uri);
    c->image_data_uri = NULL;
}

CaptchaResult captcha_check(const unsigned char key[32], const char *token, const char *answer,
                            time_t now, char nonce_out[33]) {
    if (!token || !answer) return CAPTCHA_MALFORMED;

    // nonce.issued.mac
    const char *d1 = strchr(token, '.');
    const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || d1 - token != 32 || strlen(d2 + 1) != 64) return CAPTCHA_MALFORMED;
    char nonce[33];
    memcpy(nonce, token, 32);
    nonce[32] = '\0';
    for (int i = 0; i < 32; i++) if (!isxdigit((unsigned char)nonce[i])) return CAPTCHA_MALFORMED;
    char *end = NULL;
    long long issued = strtoll(d1 + 1, &end, 10);
    if (end != d2 || issued <= 0) return CAPTCHA_MALFORMED;

    char norm[16];
    size_t k = 0;
    for (const char *p = answer; *p && k + 1 < sizeof(norm); p++)
        if (!isspace((unsigned char)*p)) norm[k++] = (char)toupper((unsigned char)*p);
    norm[k] = '\0';

    char expected[65];
    mac_hex(key, nonce, issued, norm, expected);
    int match = sodium_memcmp(expected, d2 + 1, 64) == 0;

    // The time checks need the mac to be trusted first: an attacker can't
    // move `issued` without breaking it.
    if (!match) return CAPTCHA_WRONG;
    if ((long long)now - issued > CAPTCHA_TTL_SECS) return CAPTCHA_EXPIRED;
    if ((long long)now - issued < CAPTCHA_MIN_SECS) return CAPTCHA_TOO_FAST;
    memcpy(nonce_out, nonce, 33);
    return CAPTCHA_OK;
}
