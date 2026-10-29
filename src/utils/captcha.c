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

// The drawing's randomness: a ChaCha20 keystream from a 32-byte seed, so a
// token (whose seed is derived from it, see draw_seed()) always gives the
// same image - redrawing it shows a bot nothing new.
typedef struct {
    unsigned char key[32];
    unsigned char buf[64];
    uint64_t block;
    int pos;
} Rng;

static void rng_init(Rng *r, const unsigned char seed[32]) {
    memcpy(r->key, seed, 32);
    r->block = 0;
    r->pos = 64;
}

// Uniform in [lo, hi] (the modulo bias over 32 bits is far below anything
// visible in a drawing).
static int rnd(Rng *r, int lo, int hi) {
    static const unsigned char zero_nonce[8];
    if (r->pos + 4 > 64) {
        static const unsigned char zeros[64];
        crypto_stream_chacha20_xor_ic(r->buf, zeros, 64, zero_nonce, r->block++, r->key);
        r->pos = 0;
    }
    uint32_t v = (uint32_t)r->buf[r->pos] | (uint32_t)r->buf[r->pos + 1] << 8 |
                 (uint32_t)r->buf[r->pos + 2] << 16 | (uint32_t)r->buf[r->pos + 3] << 24;
    r->pos += 4;
    return lo + (int)(v % (uint32_t)(hi - lo + 1));
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

// Draws `text` into a W x H grayscale buffer (0 black .. 255 white), every
// random choice taken from `seed`.
static void draw(const char *text, const unsigned char seed[32], uint8_t *out) {
    Rng rng;
    rng_init(&rng, seed);
    uint8_t *img = malloc((size_t)W * H);
    if (!img) { memset(out, 255, (size_t)W * H); return; }
    for (int i = 0; i < W * H; i++) img[i] = (uint8_t)rnd(&rng, 215, 255);

    int len = (int)strlen(text);
    int step = (W - 20) / (len > 0 ? len : 1);
    for (int i = 0; i < len; i++) {
        const uint8_t *g = glyph(text[i]);
        if (!g) continue;
        int sx = rnd(&rng, 4, 5), sy = rnd(&rng, 6, 7);
        double shear = rnd(&rng, -20, 20) / 100.0;
        int x0 = 10 + i * step + rnd(&rng, -3, 3);
        int y0 = (H - 7 * sy) / 2 + rnd(&rng, -6, 6);
        uint8_t ink = (uint8_t)rnd(&rng, 10, 60);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                int bx = x0 + c * sx + (int)lround(shear * (3 - r) * sy);
                int by = y0 + r * sy;
                for (int yy = 0; yy < sy; yy++)
                    for (int xx = 0; xx < sx; xx++)
                        if (rnd(&rng, 0, 29) > 0) put(img, bx + xx, by + yy, ink);   // a few holes
            }
    }

    for (int i = 0; i < 3; i++)
        line(img, rnd(&rng, 0, W / 4), rnd(&rng, 0, H - 1), rnd(&rng, 3 * W / 4, W - 1), rnd(&rng, 0, H - 1), (uint8_t)rnd(&rng, 90, 160));
    for (int i = 0; i < 250; i++) put(img, rnd(&rng, 0, W - 1), rnd(&rng, 0, H - 1), (uint8_t)rnd(&rng, 60, 180));

    // One sine wave across the image, so glyphs don't sit on straight lines.
    double amp = rnd(&rng, 15, 30) / 10.0, period = rnd(&rng, 70, 110), phase = rnd(&rng, 0, 628) / 100.0;
    for (int x = 0; x < W; x++) {
        int shift = (int)lround(amp * sin(2 * M_PI * x / period + phase));
        for (int y = 0; y < H; y++) {
            int src = y + shift;
            out[y * W + x] = (src >= 0 && src < H) ? img[src * W + x] : (uint8_t)rnd(&rng, 215, 255);
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

// ---- GIF ------------------------------------------------------------------------

// GIF87a, 16 grays, for the browsers that don't show PNG (or data: URIs).
// The LZW stream is "uncompressed": a clear code every few pixels keeps the
// code table small and every code 5 bits wide, so no dictionary is needed.
// 200x70 comes to about 9 KB.
#define GIF_CLEAR       16
#define GIF_END         17
#define GIF_CODE_BITS   5
#define GIF_RUN         12      // pixels between clear codes: the table never reaches 32

typedef struct {
    uint8_t *out;
    size_t   len;
    uint8_t  block[255];
    int      block_len;
    uint32_t bits;
    int      nbits;
} GifWriter;

static void gif_byte(GifWriter *g, uint8_t b) {
    g->block[g->block_len++] = b;
    if (g->block_len == 255) {
        g->out[g->len++] = 255;
        memcpy(g->out + g->len, g->block, 255);
        g->len += 255;
        g->block_len = 0;
    }
}

static void gif_code(GifWriter *g, unsigned code) {
    g->bits |= (uint32_t)code << g->nbits;
    g->nbits += GIF_CODE_BITS;
    while (g->nbits >= 8) {
        gif_byte(g, (uint8_t)g->bits);
        g->bits >>= 8;
        g->nbits -= 8;
    }
}

static unsigned char *encode_gif(const uint8_t *gray, size_t *len) {
    size_t pixels = (size_t)W * H;
    size_t codes = pixels + pixels / GIF_RUN + 3;
    size_t data = codes * GIF_CODE_BITS / 8 + 2;
    size_t cap = 13 + 48 + 10 + 1 + data + data / 255 + 2 + 2;
    GifWriter g = { .out = malloc(cap) };
    if (!g.out) return NULL;

    uint8_t *p = g.out;
    memcpy(p, "GIF87a", 6); p += 6;
    *p++ = (uint8_t)W; *p++ = (uint8_t)(W >> 8);
    *p++ = (uint8_t)H; *p++ = (uint8_t)(H >> 8);
    *p++ = 0xB3;        // global color table, 16 entries
    *p++ = 0; *p++ = 0;
    for (int i = 0; i < 16; i++) { uint8_t v = (uint8_t)(i * 17); *p++ = v; *p++ = v; *p++ = v; }
    *p++ = 0x2C;        // image descriptor: 0,0 W x H, no local table
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = (uint8_t)W; *p++ = (uint8_t)(W >> 8);
    *p++ = (uint8_t)H; *p++ = (uint8_t)(H >> 8);
    *p++ = 0;
    *p++ = 4;           // LZW minimum code size: 16 colors
    g.len = (size_t)(p - g.out);

    for (size_t i = 0; i < pixels; i++) {
        if (i % GIF_RUN == 0) gif_code(&g, GIF_CLEAR);
        gif_code(&g, gray[i] >> 4);
    }
    gif_code(&g, GIF_END);
    if (g.nbits > 0) gif_byte(&g, (uint8_t)g.bits);
    if (g.block_len) {
        g.out[g.len++] = (uint8_t)g.block_len;
        memcpy(g.out + g.len, g.block, (size_t)g.block_len);
        g.len += (size_t)g.block_len;
    }
    g.out[g.len++] = 0;     // end of image data
    g.out[g.len++] = 0x3B;  // trailer
    *len = g.len;
    return g.out;
}

// ---- Text art -------------------------------------------------------------------

// The challenge for browsers without images (epoch 0: Lynx, w3m...): the
// same 5x7 glyphs as characters, for a <pre>. Each glyph pixel is two
// characters wide (a terminal cell is about twice as tall as wide), in an
// ink of its own; glyphs are slanted, shifted up or down, spaced unevenly,
// with a few holes, among scattered punctuation. Fits 80 columns. Only
// characters that need no HTML escaping.
#define ART_ROWS  9
#define ART_COLS  80

static char *draw_text_art(const char *text, const unsigned char seed[32]) {
    static const char INKS[] = "#%@$MWX8";
    static const char NOISE[] = ".,'`:-";
    Rng rng;
    rng_init(&rng, seed);
    char grid[ART_ROWS][ART_COLS];
    memset(grid, ' ', sizeof(grid));

    int x = rnd(&rng, 1, 3);
    for (const char *p = text; *p; p++) {
        const uint8_t *g = glyph(*p);
        if (!g) continue;
        char ink = INKS[rnd(&rng, 0, (int)sizeof(INKS) - 2)];
        int y0 = rnd(&rng, 0, ART_ROWS - 7);
        int slant = rnd(&rng, -1, 1);   // the top rows lean one cell left, right, or not
        for (int r = 0; r < 7; r++) {
            int dx = r < 3 ? slant : 0;
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int k = 0; k < 2; k++) {
                    int col = x + dx + c * 2 + k;
                    if (col >= 0 && col < ART_COLS && rnd(&rng, 0, 24) > 0) grid[y0 + r][col] = ink;
                }
            }
        }
        x += 10 + rnd(&rng, 3, 5);
    }
    for (int r = 0; r < ART_ROWS; r++)
        for (int c = 0; c < x && c < ART_COLS; c++)
            if (grid[r][c] == ' ' && rnd(&rng, 0, 13) == 0)
                grid[r][c] = NOISE[rnd(&rng, 0, (int)sizeof(NOISE) - 2)];

    char *out = malloc(ART_ROWS * (ART_COLS + 1) + 1);
    if (!out) return NULL;
    char *w = out;
    for (int r = 0; r < ART_ROWS; r++) {
        int n = ART_COLS;
        while (n > 0 && grid[r][n - 1] == ' ') n--;
        memcpy(w, grid[r], (size_t)n);
        w += n;
        *w++ = '\n';
    }
    *w = '\0';
    return out;
}

// ---- WBMP (WAP) ----------------------------------------------------------------

// A phone's screen is 96-160 pixels wide and black and white: the glyphs at
// twice their size, each shifted up or down, a few holes, dots and a dotted
// line - less than the GIF, as a 1-bit image this small can't take more and
// stay readable.
static unsigned char *draw_wbmp(const char *text, const unsigned char seed[32], size_t *len) {
    enum { WW = CAPTCHA_WBMP_WIDTH, WH = CAPTCHA_WBMP_HEIGHT };
    Rng rng;
    rng_init(&rng, seed);
    uint8_t ink[WH][WW];
    memset(ink, 0, sizeof(ink));

    int x = rnd(&rng, 2, 6);
    for (const char *p = text; *p; p++) {
        const uint8_t *g = glyph(*p);
        if (!g) continue;
        int y0 = (WH - 14) / 2 + rnd(&rng, -4, 4);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int k = 0; k < 4; k++) {
                    int px = x + c * 2 + (k & 1), py = y0 + r * 2 + (k >> 1);
                    if (px >= 0 && px < WW && py < WH && rnd(&rng, 0, 19) > 0) ink[py][px] = 1;
                }
            }
        x += 10 + rnd(&rng, 4, 7);
    }
    for (int i = 0; i < 25; i++) ink[rnd(&rng, 0, WH - 1)][rnd(&rng, 0, WW - 1)] = 1;
    int y = rnd(&rng, 8, WH - 9), dy = rnd(&rng, -1, 1);
    for (int lx = 0; lx < WW; lx++) {
        if (lx % 24 == 23) y += dy;
        if (y >= 0 && y < WH && lx % 3 == 0) ink[y][lx] = 1;
    }

    // WBMP type 0: type, fixed header, width and height (one byte each below
    // 128), then rows of bits, most significant first, 1 white.
    int row_bytes = (WW + 7) / 8;
    *len = 4 + (size_t)row_bytes * WH;
    unsigned char *out = malloc(*len);
    if (!out) return NULL;
    out[0] = 0; out[1] = 0; out[2] = WW; out[3] = WH;
    unsigned char *w = out + 4;
    for (int r = 0; r < WH; r++)
        for (int b = 0; b < row_bytes; b++) {
            unsigned char byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int px = b * 8 + bit;
                if (px >= WW || !ink[r][px]) byte |= (unsigned char)(0x80 >> bit);
            }
            *w++ = byte;
        }
    return out;
}

// ---- Images of a text (tests) ---------------------------------------------------

static unsigned char *image_of(const char *text, const unsigned char seed[32], int gif, size_t *len) {
    uint8_t *gray = malloc((size_t)W * H);
    if (!gray) return NULL;
    draw(text, seed, gray);
    unsigned char *img = gif ? encode_gif(gray, len) : encode_png(gray, len);
    free(gray);
    return img;
}

unsigned char *captcha_png(const char *text, size_t *len) {
    static const unsigned char seed[32];
    return image_of(text, seed, 0, len);
}

unsigned char *captcha_gif(const char *text, size_t *len) {
    static const unsigned char seed[32];
    return image_of(text, seed, 1, len);
}

char *captcha_text_art(const char *text) {
    static const unsigned char seed[32];
    return draw_text_art(text, seed);
}

unsigned char *captcha_wbmp(const char *text, size_t *len) {
    static const unsigned char seed[32];
    return draw_wbmp(text, seed, len);
}

// ---- Token ----------------------------------------------------------------------

static void hmac(const unsigned char key[32], const char *msg, unsigned char out[32]) {
    crypto_auth_hmacsha256(out, (const unsigned char *)msg, strlen(msg), key);
}

// The answer of nonce/issued: drawn from an HMAC under the site's key, so
// it is in no token and no record, yet the server can always recompute it -
// to check an answer, or to draw the image again from the token alone.
static void derive_text(const unsigned char key[32], const char *nonce, long long issued,
                        char out[CAPTCHA_LENGTH + 1]) {
    char msg[96];
    unsigned char h[32];
    snprintf(msg, sizeof(msg), "captcha-text|%s|%lld", nonce, issued);
    hmac(key, msg, h);
    for (int i = 0; i < CAPTCHA_LENGTH; i++) out[i] = ALPHABET[h[i] % (sizeof(ALPHABET) - 1)];
    out[CAPTCHA_LENGTH] = '\0';
    sodium_memzero(h, sizeof(h));
}

// The drawing's seed of nonce/issued (Rng).
static void draw_seed(const unsigned char key[32], const char *nonce, long long issued, unsigned char out[32]) {
    char msg[96];
    snprintf(msg, sizeof(msg), "captcha-draw|%s|%lld", nonce, issued);
    hmac(key, msg, out);
}

static void mac_hex(const unsigned char key[32], const char *nonce, long long issued, const char *answer,
                    char out[65]) {
    char msg[128];
    unsigned char mac[32];
    snprintf(msg, sizeof(msg), "captcha|%s|%lld|%s", nonce, issued, answer);
    hmac(key, msg, mac);
    sodium_bin2hex(out, 65, mac, sizeof(mac));
}

// Splits "nonce.issued.mac". Returns 1 on the right shape.
static int parse_token(const char *token, char nonce[33], long long *issued, const char **mac) {
    if (!token) return 0;
    const char *d1 = strchr(token, '.');
    const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || d1 - token != 32 || strlen(d2 + 1) != 64) return 0;
    memcpy(nonce, token, 32);
    nonce[32] = '\0';
    for (int i = 0; i < 32; i++) if (!isxdigit((unsigned char)nonce[i])) return 0;
    char *end = NULL;
    *issued = strtoll(d1 + 1, &end, 10);
    if (end != d2 || *issued <= 0) return 0;
    *mac = d2 + 1;
    return 1;
}

// 1 if `token` is the site's own (its mac matches its derived answer); fills
// nonce, issued and the answer.
static int token_genuine(const unsigned char key[32], const char *token, char nonce[33], long long *issued,
                         char text[CAPTCHA_LENGTH + 1]) {
    const char *mac;
    if (!parse_token(token, nonce, issued, &mac)) return 0;
    char expected[65];
    derive_text(key, nonce, *issued, text);
    mac_hex(key, nonce, *issued, text, expected);
    return sodium_memcmp(expected, mac, 64) == 0;
}

int captcha_issue(const unsigned char key[32], time_t issued, int inline_png, Captcha *out) {
    memset(out, 0, sizeof(*out));
    unsigned char raw[16];
    char nonce[33], mac[65], text[CAPTCHA_LENGTH + 1];
    randombytes_buf(raw, sizeof(raw));
    sodium_bin2hex(nonce, sizeof(nonce), raw, sizeof(raw));
    derive_text(key, nonce, (long long)issued, text);
    mac_hex(key, nonce, (long long)issued, text, mac);
    snprintf(out->token, sizeof(out->token), "%s.%lld.%s", nonce, (long long)issued, mac);
    if (!inline_png) { sodium_memzero(text, sizeof(text)); return 0; }

    unsigned char seed[32];
    draw_seed(key, nonce, (long long)issued, seed);
    size_t png_len = 0;
    unsigned char *png = image_of(text, seed, 0, &png_len);
    sodium_memzero(text, sizeof(text));
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

int captcha_new(const unsigned char key[32], int inline_png, Captcha *out) {
    return captcha_issue(key, time(NULL), inline_png, out);
}

void captcha_free(Captcha *c) {
    if (!c) return;
    free(c->image_data_uri);
    c->image_data_uri = NULL;
}

int captcha_answer(const unsigned char key[32], const char *token, char out[CAPTCHA_LENGTH + 1]) {
    char nonce[33];
    long long issued;
    if (token_genuine(key, token, nonce, &issued, out)) return 0;
    sodium_memzero(out, CAPTCHA_LENGTH + 1);
    return -1;
}

// The answer and drawing seed of `token`: 1 when it is the key's own and not
// expired at `now`.
static int token_drawing(const unsigned char key[32], const char *token, time_t now,
                         char text[CAPTCHA_LENGTH + 1], unsigned char seed[32]) {
    char nonce[33];
    long long issued;
    if (!token_genuine(key, token, nonce, &issued, text) ||
        (long long)now - issued > CAPTCHA_TTL_SECS || issued > (long long)now + 60) {
        sodium_memzero(text, CAPTCHA_LENGTH + 1);
        return 0;
    }
    draw_seed(key, nonce, issued, seed);
    return 1;
}

unsigned char *captcha_token_gif(const unsigned char key[32], const char *token, time_t now, size_t *len) {
    char text[CAPTCHA_LENGTH + 1];
    unsigned char seed[32];
    if (!token_drawing(key, token, now, text, seed)) return NULL;
    unsigned char *gif = image_of(text, seed, 1, len);
    sodium_memzero(text, sizeof(text));
    sodium_memzero(seed, sizeof(seed));
    return gif;
}

unsigned char *captcha_token_wbmp(const unsigned char key[32], const char *token, time_t now, size_t *len) {
    char text[CAPTCHA_LENGTH + 1];
    unsigned char seed[32];
    if (!token_drawing(key, token, now, text, seed)) return NULL;
    unsigned char *wbmp = draw_wbmp(text, seed, len);
    sodium_memzero(text, sizeof(text));
    sodium_memzero(seed, sizeof(seed));
    return wbmp;
}

char *captcha_token_text(const unsigned char key[32], const char *token, time_t now) {
    char text[CAPTCHA_LENGTH + 1];
    unsigned char seed[32];
    if (!token_drawing(key, token, now, text, seed)) return NULL;
    char *art = draw_text_art(text, seed);
    sodium_memzero(text, sizeof(text));
    sodium_memzero(seed, sizeof(seed));
    return art;
}

CaptchaResult captcha_check(const unsigned char key[32], const char *token, const char *answer,
                            time_t now, char nonce_out[33]) {
    char nonce[33];
    long long issued;
    const char *mac;
    if (!answer || !parse_token(token, nonce, &issued, &mac)) return CAPTCHA_MALFORMED;

    char norm[16];
    size_t k = 0;
    for (const char *p = answer; *p && k + 1 < sizeof(norm); p++)
        if (!isspace((unsigned char)*p)) norm[k++] = (char)toupper((unsigned char)*p);
    norm[k] = '\0';

    char expected[65];
    mac_hex(key, nonce, issued, norm, expected);
    int match = sodium_memcmp(expected, mac, 64) == 0;

    // The time checks need the mac to be trusted first: an attacker can't
    // move `issued` without breaking it.
    if (!match) return CAPTCHA_WRONG;
    if ((long long)now - issued > CAPTCHA_TTL_SECS) return CAPTCHA_EXPIRED;
    if ((long long)now - issued < CAPTCHA_MIN_SECS) return CAPTCHA_TOO_FAST;
    memcpy(nonce_out, nonce, 33);
    return CAPTCHA_OK;
}
