// timegm() availability varies by platform.
// On Linux (glibc) it requires _GNU_SOURCE.
// On macOS / BSD it is a native BSD function exposed by default.
#if defined(__linux__) || defined(__GLIBC__)
#  define _GNU_SOURCE
#else
#  define _XOPEN_SOURCE 700
#endif

#include "static_file_server.h"
#include "../connection.h"
#include "../../utils/http_utils.h"
#include "../../utils/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#define SEND_BUFFER_SIZE 8192

// Security headers added to every response.
#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: SAMEORIGIN\r\n"

// Format a time_t as an HTTP-date string (RFC 7231).
static void format_http_date(char *buf, size_t size, time_t t) {
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
}

// Parse an HTTP-date string into time_t. Returns (time_t)-1 on failure.
static time_t parse_http_date(const char *s) {
    if (!s || !*s) return (time_t)-1;
    struct tm tm_val = {0};
    if (strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm_val) == NULL) return (time_t)-1;
    return timegm(&tm_val);
}

int serve_static_file(void *ctx,
                      const char *root_directory,
                      const char *decoded_url,
                      const char *if_modified_since) {
    char safe_path[PATH_MAX];
    if (!sanitize_path(decoded_url, safe_path, sizeof(safe_path), root_directory)) {
        LOG_WARN("Path traversal or invalid path: %s", decoded_url);
        return 404;
    }

    struct stat st;
    if (stat(safe_path, &st) != 0) {
        return 404;
    }

    // Directory → serve index.html inside it.
    if (S_ISDIR(st.st_mode)) {
        size_t base_len = strlen(safe_path);
        if (base_len + sizeof("/index.html") >= sizeof(safe_path)) {
            return 403;
        }
        memcpy(safe_path + base_len, "/index.html", sizeof("/index.html"));
        if (stat(safe_path, &st) != 0) {
            return 404;
        }
    }

    // Cache validation: If-Modified-Since
    char last_modified_str[64];
    format_http_date(last_modified_str, sizeof(last_modified_str), st.st_mtime);

    time_t ims = parse_http_date(if_modified_since);
    if (ims != (time_t)-1 && st.st_mtime <= ims) {
        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 304 Not Modified\r\n"
            "Last-Modified: %s\r\n"
            SECURITY_HEADERS
            "Connection: close\r\n"
            "\r\n",
            last_modified_str);
        connection_write(ctx, header, strlen(header));
        LOG_DEBUG("304 Not Modified: %s", safe_path);
        return 0;
    }

    int fd = open(safe_path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("open(%s) failed", safe_path);
        return 500;
    }

    const char *mime = get_mime_type(safe_path);

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Last-Modified: %s\r\n"
        "Cache-Control: public, max-age=3600\r\n"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        mime, (long)st.st_size, last_modified_str);
    connection_write(ctx, header, strlen(header));

    char buf[SEND_BUFFER_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (connection_write(ctx, buf, (size_t)n) <= 0) {
            LOG_WARN("Client disconnected during transfer: %s", safe_path);
            break;
        }
    }

    close(fd);
    LOG_DEBUG("200 OK: %s (%s, %ld bytes)", safe_path, mime, (long)st.st_size);
    return 0;
}
