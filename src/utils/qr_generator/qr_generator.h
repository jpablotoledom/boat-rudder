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

// Generate a QR code as ASCII art (<pre> block) for a YouTube URL.
// Returns heap-allocated string (caller must free), NULL on error.
char *generate_youtube_qr_text(const char *youtube_url);

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

// Generate a QR code as Unicode half-block <pre> art for any text.
// Returns heap-allocated string (caller must free), NULL on error.
char *generate_qr_halfblock_text(const char *text);

#endif
