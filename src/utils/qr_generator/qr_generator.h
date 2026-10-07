#ifndef QR_GENERATOR_H
#define QR_GENERATOR_H

#include <stddef.h>

// Extract YouTube video ID from any YouTube URL format.
// Writes into out (null-terminated). Returns 0 on success.
int extract_youtube_id(const char *url, char *out, size_t out_size);

// Generate a QR code PNG for a YouTube video URL.
// html_root: filesystem path to the html/ directory (e.g. "./html")
// Returns 0 on success, -1 on error.
int generate_youtube_qr(const char *youtube_url, const char *html_root);

// Build the web path for a YouTube QR image given the video ID.
// e.g. "/content/qr/youtube-dQw4w9WgXcQ.png"
void youtube_qr_web_path(const char *video_id, char *out, size_t out_size);

// Build the YouTube short URL from a video ID.
void youtube_short_url(const char *video_id, char *out, size_t out_size);

// Generate a WBMP QR code for a YouTube URL (for WAP/WML browsers).
// html_root: filesystem path to html/ directory.
int generate_youtube_qr_wbmp(const char *youtube_url, const char *html_root);

// Build the web path for a YouTube QR WBMP image.
void youtube_qr_wbmp_web_path(const char *video_id, char *out, size_t out_size);

// Generate a WBMP QR code for any text, cached at out_path on disk.
// web_path: output web-accessible path (e.g. /content/qr/gallery-ID.wbmp)
// fs_path:  filesystem path to write the file
// Returns 0 on success.
int generate_qr_wbmp(const char *text, const char *fs_path);

// Generate a QR code as Unicode half-block art for any text - two QR rows
// packed into one output row inside a <pre> block (cells that are neither
// top/bottom-dark as a plain space), so it comes out roughly square in a
// real terminal despite a monospace cell being taller than it is wide. Only
// for epoch 0: its realistic reader is a terminal browser in a UTF-8
// locale, unlike epoch 1/WML, which predate UTF-8 and get transcoded to
// Latin-1 (see build_epoch_response.c) - a rendering built from bytes above
// U+00FF would not survive that.
// Returns heap-allocated string (caller must free), NULL on error.
char *generate_qr_halfblock_text(const char *text);

// Same idea as generate_qr_halfblock_text() - two QR rows packed into one
// text row - but for EPOCH_PRESTANDARD's one real pre-Unicode reader, Cello
// (see utils/request_charset.h and detect_epoch.c's own note on why it's
// classified there despite that), which can't render the Unicode half-block
// glyphs any more than it can decode any other multi-byte UTF-8. Emits the
// UTF-8 encoding of Latin-1 code points U+00DB/U+00DC/U+00DF - not because
// the QR wants those actual letters (Û/Ü/ß), but because utf8_to_latin1()
// transcodes them down to exactly the byte values (0xDB/0xDC/0xDF) that
// render as real block-drawing glyphs (full/upper-half/lower-half) in
// "Terminal", the CP437 bitmap font Windows ships - wrapped in a plain
// <pre> (Cello has no notion of HTML's `face` attribute; each text role,
// including <pre>'s, is a separate local Configure > Fonts entry instead -
// point Monospace/Terminal at it to see these). A real block character, not
// an ASCII approximation (tried first: '#'/'*'/'n' - readable, but never
// aligned into a clean grid over Cello's own proportional default font). No
// quiet-zone margin, since a wide QR (e.g. the gallery's long URL) can
// otherwise outgrow the reader's window width even with <nobr> forcing
// horizontal scroll instead of wraparound. Followed, in the same <pre>
// block, by a plain-text reminder with a dash border exactly as wide as the
// grid above it and a note to configure the terminal font.
// Returns heap-allocated string (caller must free), NULL on error.
char *generate_qr_asciiblock_text(const char *text);

#endif
