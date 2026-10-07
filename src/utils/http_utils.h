#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

// MIME type detection from file extension.
const char *get_mime_type(const char *path);

// URL percent-encoding / decoding.
void url_encode(char *dst, const char *src, size_t dst_size);
void url_decode(char *dst, const char *src);

// HTML entity encoding (< > & ").
void html_encode(char *dst, const char *src, size_t dst_size);

// Resolve url_path relative to root_directory and verify the result stays
// inside root_directory (prevents directory traversal attacks).
// Returns 1 on success (safe_path is populated), 0 on failure.
int sanitize_path(const char *url_path, char *safe_path, size_t size,
                  const char *root_directory);

// Returns 1 if peer_ip matches any entry in the trusted_proxies config value.
// Only trusted peers have X-Real-IP / X-Forwarded-For honored.
int is_trusted_proxy(const char *peer_ip);

#endif // HTTP_UTILS_H
