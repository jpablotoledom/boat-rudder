#ifndef JSON_UTILS_H
#define JSON_UTILS_H

// Escapes `src` for embedding as a JSON string literal's contents (escapes
// '"', '\\', and control characters; everything else - including UTF-8
// multi-byte sequences - is passed through unchanged). Returns a malloc'd,
// NUL-terminated buffer the caller must free(), or NULL on allocation failure.
char *json_escape_alloc(const char *src);

#endif // JSON_UTILS_H
