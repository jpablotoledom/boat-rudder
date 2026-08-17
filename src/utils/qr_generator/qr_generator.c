#include "qr_generator.h"
#include "qrencode_minimal.h"
#include "../../utils/log.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- YouTube ID / URL helpers ---- */

int extract_youtube_id(const char *url, char *out, size_t out_size) {
    if (!url || !out || out_size < 2) return -1;
    out[0] = '\0';
    const char *p = NULL;
    if      ((p = strstr(url, "/embed/"))   != NULL) p += 7;
    else if ((p = strstr(url, "youtu.be/")) != NULL) p += 9;
    else if ((p = strstr(url, "v="))        != NULL) p += 2;
    else return -1;
    // YouTube ids are [A-Za-z0-9_-]. Stop at anything else: the id is
    // interpolated into filesystem paths, so a stray '/' or '.' would let a
    // crafted URL escape the qr/ directory.
    size_t i = 0;
    while (p[i] && i < out_size - 1) {
        char c = p[i];
        int allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) break;
        out[i] = c;
        i++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

void youtube_qr_web_path(const char *video_id, char *out, size_t out_size) {
    snprintf(out, out_size, "/content/qr/youtube-%s.gif", video_id);
}

void youtube_short_url(const char *video_id, char *out, size_t out_size) {
    snprintf(out, out_size, "https://youtu.be/%s", video_id);
}

/* ================================================================
   Minimal GIF89a LZW encoder — supports up to 4096-color tables.
   Here we use min_code_size=2 for a 2-color (B&W) image.
   ================================================================ */

#define LZW_MAX_CODES 4096
#define LZW_HASH_SIZE 5003  /* prime > LZW_MAX_CODES */

/* The dictionary is per-encode state, not shared: connection threads render
   pages concurrently and each may be encoding its own QR. A file-scope table
   would let two encoders interleave writes and emit a corrupt stream, so the
   table is allocated per write_gif() call and threaded through explicitly. */
typedef struct { int prefix, suffix, code; } LZWEntry;

static void lzw_hash_clear(LZWEntry *hash) {
    for (int i = 0; i < LZW_HASH_SIZE; i++) hash[i].code = -1;
}

static int lzw_hash_find(const LZWEntry *hash, int prefix, int suffix) {
    int h = (int)(((unsigned)(prefix * 31 + suffix * 97)) % LZW_HASH_SIZE);
    for (;;) {
        if (hash[h].code == -1) return -1;
        if (hash[h].prefix == prefix && hash[h].suffix == suffix)
            return hash[h].code;
        h = (h + 1) % LZW_HASH_SIZE;
    }
}

static void lzw_hash_add(LZWEntry *hash, int prefix, int suffix, int code) {
    int h = (int)(((unsigned)(prefix * 31 + suffix * 97)) % LZW_HASH_SIZE);
    while (hash[h].code != -1) h = (h + 1) % LZW_HASH_SIZE;
    hash[h].prefix = prefix;
    hash[h].suffix = suffix;
    hash[h].code   = code;
}

/* Bit-level writer (LSB-first, packed into 255-byte GIF sub-blocks) */
typedef struct {
    FILE *fp;
    unsigned char sub[256];
    int sub_len;
    unsigned int bit_buf;
    int bit_pos;
} GIFBits;

static void gif_flush_sub(GIFBits *g) {
    if (g->sub_len > 0) {
        fputc(g->sub_len, g->fp);
        fwrite(g->sub, 1, g->sub_len, g->fp);
        g->sub_len = 0;
    }
}

static void gif_put_byte(GIFBits *g, unsigned char b) {
    g->sub[g->sub_len++] = b;
    if (g->sub_len == 255) gif_flush_sub(g);
}

static void gif_put_code(GIFBits *g, int code, int bits) {
    g->bit_buf |= (unsigned int)code << g->bit_pos;
    g->bit_pos += bits;
    while (g->bit_pos >= 8) {
        gif_put_byte(g, g->bit_buf & 0xFF);
        g->bit_buf >>= 8;
        g->bit_pos -= 8;
    }
}

static void gif_flush_bits(GIFBits *g) {
    while (g->bit_pos > 0) {
        gif_put_byte(g, g->bit_buf & 0xFF);
        g->bit_buf >>= 8;
        g->bit_pos -= 8;
    }
    gif_flush_sub(g);
    fputc(0, g->fp); /* block terminator */
}

/* Write a GIF89a file with 2-color palette.
   colors[0] = background, colors[1] = foreground (each 3 bytes RGB) */
static int write_gif(const char *path,
                     const unsigned char *pixels, int width, int height,
                     unsigned char bg[3], unsigned char fg[3]) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    /* --- GIF Header --- */
    fwrite("GIF87a", 1, 6, fp);

    /* Logical Screen Descriptor */
    unsigned char lsd[7];
    lsd[0] = width  & 0xFF; lsd[1] = (width  >> 8) & 0xFF;
    lsd[2] = height & 0xFF; lsd[3] = (height >> 8) & 0xFF;
    lsd[4] = 0xA0;  /* GCT flag=1, color res=2, sort=0, GCT size=0 (2 colors) */
    lsd[5] = 0;     /* background color index */
    lsd[6] = 0;     /* pixel aspect ratio */
    fwrite(lsd, 1, 7, fp);

    /* Global Color Table: 2 entries (BG + FG), padded to 2^1=2 entries */
    fwrite(bg, 1, 3, fp);
    fwrite(fg, 1, 3, fp);

    /* --- Image Descriptor --- */
    fputc(0x2C, fp);  /* Image Separator */
    unsigned char id[9] = {0,0, 0,0,
                            width&0xFF, (width>>8)&0xFF,
                            height&0xFF, (height>>8)&0xFF,
                            0x00 /* no local CT, no interlace */};
    fwrite(id, 1, 9, fp);

    /* --- LZW Image Data --- */
    const int min_code = 2;       /* minimum LZW code size */
    const int clear_c  = 1 << min_code;   /* 4  */
    const int eoi_c    = clear_c + 1;     /* 5  */

    fputc(min_code, fp);          /* LZW minimum code size byte */

    GIFBits g = {fp, {0}, 0, 0, 0};

    /* Initialize LZW */
    LZWEntry *hash = malloc(sizeof(LZWEntry) * LZW_HASH_SIZE);
    if (!hash) { fclose(fp); return -1; }
    lzw_hash_clear(hash);
    int code_size  = min_code + 1;  /* starts at 3 bits */
    int next_code  = eoi_c + 1;     /* 6 */
    int code_limit = 1 << code_size; /* 8 */

    gif_put_code(&g, clear_c, code_size);

    int prefix = -1;
    int total  = width * height;

    for (int i = 0; i < total; i++) {
        int px = pixels[i] & 1;  /* 0 or 1 */

        if (prefix == -1) {
            prefix = px;
            continue;
        }

        int found = lzw_hash_find(hash, prefix, px);
        if (found != -1) {
            prefix = found;
        } else {
            gif_put_code(&g, prefix, code_size);

            if (next_code < LZW_MAX_CODES) {
                lzw_hash_add(hash, prefix, px, next_code++);
                if (next_code > code_limit && code_size < 12) {
                    code_size++;
                    code_limit <<= 1;
                }
            } else {
                /* Table full: emit clear code and reset */
                gif_put_code(&g, clear_c, code_size);
                lzw_hash_clear(hash);
                code_size  = min_code + 1;
                next_code  = eoi_c + 1;
                code_limit = 1 << code_size;
            }
            prefix = px;
        }
    }

    if (prefix != -1) gif_put_code(&g, prefix, code_size);
    gif_put_code(&g, eoi_c, code_size);
    gif_flush_bits(&g);   /* also flushes the last sub-block + block terminator */

    fputc(0x3B, fp); /* GIF Trailer */

    free(hash);
    int ok = (ferror(fp) == 0);
    fclose(fp);
    return ok ? 0 : -1;
}

/* ================================================================
   Atomic asset publishing

   Several connection threads can render the same page at once and each will
   try to generate the same missing QR asset. Writing straight to the final
   path lets one thread serve a half-written file another thread is still
   filling in, so every writer builds a private temp file and rename()s it
   into place - rename is atomic within a filesystem, so readers see either
   the old state or the complete file, never a partial one.
   ================================================================ */

static void temp_path_for(const char *final_path, char *out, size_t out_size) {
    static _Atomic unsigned counter = 0;
    unsigned n = counter++;
    snprintf(out, out_size, "%s.tmp.%d.%u", final_path, (int)getpid(), n);
}

static int finish_atomic(const char *tmp_path, const char *final_path, int write_rc) {
    if (write_rc != 0 || rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

/* ================================================================
   Public API
   ================================================================ */

int generate_youtube_qr(const char *youtube_url, const char *html_root) {
    char video_id[32] = {0};
    if (extract_youtube_id(youtube_url, video_id, sizeof(video_id)) != 0) {
        LOG_ERROR("QR: could not extract video ID from: %s", youtube_url);
        return -1;
    }

    /* Build filesystem output path */
    char qr_dir[512];
    snprintf(qr_dir, sizeof(qr_dir), "%s/content/qr", html_root);
    mkdir(qr_dir, 0755);

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/content/qr/youtube-%s.gif",
             html_root, video_id);

    /* Skip if already exists */
    struct stat st;
    if (stat(out_path, &st) == 0) {
        LOG_DEBUG("QR already exists: %s", out_path);
        return 0;
    }

    /* Build short URL as QR payload */
    char short_url[128];
    youtube_short_url(video_id, short_url, sizeof(short_url));

    /* Generate QR matrix via libqrencode */
    QRcode *qr = QRcode_encodeString(short_url, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) {
        LOG_ERROR("QR: QRcode_encodeString failed for %s", short_url);
        return -1;
    }

    /* Build the palette-index buffer: 0 = light (background), 1 = dark */
    const int scale  = 4;
    const int margin = 2;
    int img_size = (qr->width + margin * 2) * scale;

    unsigned char *pixels = malloc((size_t)img_size * img_size);
    if (!pixels) { QRcode_free(qr); return -1; }

    for (int y = 0; y < img_size; y++) {
        int qr_y = y / scale - margin;
        for (int x = 0; x < img_size; x++) {
            int qr_x = x / scale - margin;
            int dark  = 0;
            if (qr_x >= 0 && qr_x < qr->width &&
                qr_y >= 0 && qr_y < qr->width)
                dark = qr->data[qr_y * qr->width + qr_x] & 0x01;
            pixels[y * img_size + x] = dark ? 1 : 0;
        }
    }
    QRcode_free(qr);

    /* Encode with the built-in GIF87a writer. The original shelled out to
       ImageMagick, which made the binary depend on `magick` being installed
       and ran a shell command per render; write_gif() is pure C and needs
       neither. */
    unsigned char bg[3] = {255, 255, 255};
    unsigned char fg[3] = {0, 0, 0};
    char tmp_path[600];
    temp_path_for(out_path, tmp_path, sizeof(tmp_path));
    int ret = finish_atomic(tmp_path, out_path,
                            write_gif(tmp_path, pixels, img_size, img_size, bg, fg));
    free(pixels);

    if (ret == 0) { LOG_DEBUG("QR GIF generated: %s", out_path); }
    else          { LOG_ERROR("QR GIF write failed: %s", out_path); }
    return ret;
}

char *generate_youtube_qr_text(const char *youtube_url) {
    char video_id[32] = {0};
    if (extract_youtube_id(youtube_url, video_id, sizeof(video_id)) != 0)
        return NULL;

    char short_url[128];
    youtube_short_url(video_id, short_url, sizeof(short_url));

    QRcode *qr = QRcode_encodeString(short_url, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) return NULL;

    const int margin = 2;
    int size = qr->width;
    int grid = size + margin * 2;       /* total modules including margin */
    int term_rows = (grid + 1) / 2;     /* 2 QR rows per terminal line */

    /* worst case: 3 bytes per module (UTF-8 half-block) + newline + NUL */
    char *buf = malloc((size_t)term_rows * (grid * 3 + 2) + 1);
    if (!buf) { QRcode_free(qr); return NULL; }

    char *p = buf;
    for (int tr = 0; tr < term_rows; tr++) {
        int qr_top = tr * 2 - margin;
        int qr_bot = tr * 2 + 1 - margin;
        for (int c = 0; c < grid; c++) {
            int col = c - margin;
            int top = (qr_top >= 0 && qr_top < size && col >= 0 && col < size)
                      ? (qr->data[qr_top * size + col] & 0x01) : 0;
            int bot = (qr_bot >= 0 && qr_bot < size && col >= 0 && col < size)
                      ? (qr->data[qr_bot * size + col] & 0x01) : 0;
            if (top && bot) {
                /* U+2588 FULL BLOCK */
                *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x88;
            } else if (top) {
                /* U+2580 UPPER HALF BLOCK */
                *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x80;
            } else if (bot) {
                /* U+2584 LOWER HALF BLOCK */
                *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x84;
            } else {
                *p++ = ' ';
            }
        }
        *p++ = '\n';
    }
    *p = '\0';

    QRcode_free(qr);
    return buf;
}

/* WBMP type 0: two header bytes, then width/height as multi-byte integers
   (7 bits per byte, high bit = "more follows"), then a 1bpp bitmap padded to
   whole bytes per row, where 1 = white and 0 = black. */
static int write_wbmp(const char *path, const unsigned char *pixels,
                      int width, int height) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    fputc(0x00, fp);  /* type: B&W, no compression */
    fputc(0x00, fp);  /* fixed header */

    for (int dim = 0; dim < 2; dim++) {
        int v = dim ? height : width;
        unsigned char mb[5];
        int n = 0;
        do { mb[n++] = v & 0x7F; v >>= 7; } while (v > 0);
        while (n > 0) {
            n--;
            fputc(mb[n] | (n > 0 ? 0x80 : 0x00), fp);
        }
    }

    int row_bytes = (width + 7) / 8;
    for (int y = 0; y < height; y++) {
        for (int b = 0; b < row_bytes; b++) {
            unsigned char byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int x = b * 8 + bit;
                /* pad beyond the row with white so the quiet zone stays clean */
                int dark = (x < width) ? (pixels[y * width + x] & 1) : 0;
                if (!dark) byte |= (unsigned char)(0x80 >> bit);
            }
            fputc(byte, fp);
        }
    }

    int ok = (ferror(fp) == 0);
    fclose(fp);
    return ok ? 0 : -1;
}

int generate_qr_wbmp(const char *text, const char *fs_path) {
    struct stat st;
    if (stat(fs_path, &st) == 0) {
        LOG_DEBUG("QR WBMP cached: %s", fs_path);
        return 0;
    }

    QRcode *qr = QRcode_encodeString(text, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) return -1;

    const int scale = 4, margin = 2;
    int size = qr->width;
    int img_size = (size + margin * 2) * scale;
    unsigned char *pixels = malloc((size_t)img_size * img_size);
    if (!pixels) { QRcode_free(qr); return -1; }

    for (int y = 0; y < img_size; y++) {
        int qr_y = y / scale - margin;
        for (int x = 0; x < img_size; x++) {
            int qr_x = x / scale - margin;
            int dark = 0;
            if (qr_x >= 0 && qr_x < size && qr_y >= 0 && qr_y < size)
                dark = qr->data[qr_y * size + qr_x] & 0x01;
            pixels[y * img_size + x] = dark ? 1 : 0;
        }
    }
    QRcode_free(qr);

    /* The original shelled out to ImageMagick via a PGM temp file, which made
       the binary depend on `magick` and ran a shell command per render.
       write_wbmp() needs neither. */
    char tmp_path[600];
    temp_path_for(fs_path, tmp_path, sizeof(tmp_path));
    int ret = finish_atomic(tmp_path, fs_path,
                            write_wbmp(tmp_path, pixels, img_size, img_size));
    free(pixels);

    if (ret == 0) { LOG_DEBUG("QR WBMP generated: %s", fs_path); }
    else          { LOG_ERROR("QR WBMP write failed: %s", fs_path); }
    return ret;
}

char *generate_qr_halfblock_text(const char *text) {
    QRcode *qr = QRcode_encodeString(text, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) return NULL;

    const int margin = 2;
    int size = qr->width;
    int grid = size + margin * 2;
    int term_rows = (grid + 1) / 2;

    char *buf = malloc((size_t)term_rows * (grid * 3 + 2) + 1);
    if (!buf) { QRcode_free(qr); return NULL; }

    char *p = buf;
    for (int tr = 0; tr < term_rows; tr++) {
        int qr_top = tr * 2 - margin;
        int qr_bot = tr * 2 + 1 - margin;
        for (int c = 0; c < grid; c++) {
            int col = c - margin;
            int top = (qr_top >= 0 && qr_top < size && col >= 0 && col < size)
                      ? (qr->data[qr_top * size + col] & 0x01) : 0;
            int bot = (qr_bot >= 0 && qr_bot < size && col >= 0 && col < size)
                      ? (qr->data[qr_bot * size + col] & 0x01) : 0;
            // UTF-8 half blocks: full, upper, lower. Written as unsigned char
            // so the high bytes don't overflow a signed char.
            if      (top && bot) { *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x88; }
            else if (top)        { *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x80; }
            else if (bot)        { *p++ = (char)0xE2; *p++ = (char)0x96; *p++ = (char)0x84; }
            else                 { *p++ = ' '; }
        }
        *p++ = '\n';
    }
    *p = '\0';
    QRcode_free(qr);
    return buf;
}

void youtube_qr_wbmp_web_path(const char *video_id, char *out, size_t out_size) {
    snprintf(out, out_size, "/content/qr/youtube-%s.wbmp", video_id);
}

int generate_youtube_qr_wbmp(const char *youtube_url, const char *html_root) {
    char video_id[32] = {0};
    if (extract_youtube_id(youtube_url, video_id, sizeof(video_id)) != 0)
        return -1;

    char qr_dir[512];
    snprintf(qr_dir, sizeof(qr_dir), "%s/content/qr", html_root);
    mkdir(qr_dir, 0755);

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/content/qr/youtube-%s.wbmp",
             html_root, video_id);

    /* Encode straight from the URL. The original generated the GIF and asked
       ImageMagick to convert it, which meant reading a file another thread
       could still be writing. generate_qr_wbmp() handles caching and the
       atomic write itself. */
    char short_url[128];
    youtube_short_url(video_id, short_url, sizeof(short_url));
    return generate_qr_wbmp(short_url, out_path);
}
