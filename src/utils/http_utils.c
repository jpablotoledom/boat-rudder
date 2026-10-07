#define _XOPEN_SOURCE 700

#include "http_utils.h"
#include "config_loader.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=UTF-8";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".xml")  == 0) return "application/xml";
    if (strcmp(dot, ".txt")  == 0) return "text/plain; charset=UTF-8";
    if (strcmp(dot, ".jpg")  == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".bmp")  == 0) return "image/bmp";
    if (strcmp(dot, ".pcx")  == 0) return "image/vnd.zbrush.pcx";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".webp") == 0) return "image/webp";
    if (strcmp(dot, ".mp4")  == 0) return "video/mp4";
    if (strcmp(dot, ".webm") == 0) return "video/webm";
    if (strcmp(dot, ".mp3")  == 0) return "audio/mpeg";
    if (strcmp(dot, ".ogg")  == 0) return "audio/ogg";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".woff2")== 0) return "font/woff2";
    if (strcmp(dot, ".ttf")  == 0) return "font/ttf";
    if (strcmp(dot, ".pdf")  == 0) return "application/pdf";
    if (strcmp(dot, ".wbmp") == 0) return "image/vnd.wap.wbmp";
    return "application/octet-stream";
}

void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            a = tolower(a);
            b = tolower(b);
            a = (a >= 'a') ? a - 'a' + 10 : a - '0';
            b = (b >= 'a') ? b - 'a' + 10 : b - '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void url_encode(char *dst, const char *src, size_t dst_size) {
    char *end = dst + dst_size - 1;
    while (*src && dst < end) {
        if (isalnum((unsigned char)*src) || strchr("-_.~/", *src)) {
            *dst++ = *src;
        } else {
            if (dst + 3 >= end) break;
            snprintf(dst, end - dst, "%%%02X", (unsigned char)*src);
            dst += 3;
        }
        src++;
    }
    *dst = '\0';
}

void html_encode(char *dst, const char *src, size_t dst_size) {
    char *end = dst + dst_size - 1;
    while (*src && dst < end) {
        if (*src == '&') {
            if (dst + 5 >= end) break;
            memcpy(dst, "&amp;", 5); dst += 5;
        } else if (*src == '<') {
            if (dst + 4 >= end) break;
            memcpy(dst, "&lt;", 4); dst += 4;
        } else if (*src == '>') {
            if (dst + 4 >= end) break;
            memcpy(dst, "&gt;", 4); dst += 4;
        } else if (*src == '"') {
            if (dst + 6 >= end) break;
            memcpy(dst, "&quot;", 6); dst += 6;
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

int sanitize_path(const char *url_path, char *safe_path, size_t size,
                  const char *root_directory) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s%s", root_directory, url_path) >= (int)sizeof(path))
        return 0;

    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL)
        return 0;

    size_t root_len = strlen(root_directory);
    if (strncmp(resolved, root_directory, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0'))
        return 0;

    size_t resolved_len = strlen(resolved);
    if (resolved_len >= size)
        return 0;

    memcpy(safe_path, resolved, resolved_len + 1);
    return 1;
}

int is_trusted_proxy(const char *peer_ip) {
    if (!peer_ip || trusted_proxies[0] == '\0') return 0;

    char buf[sizeof(trusted_proxies)];
    strncpy(buf, trusted_proxies, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token) {
        while (*token == ' ') token++;
        if (strcmp(token, peer_ip) == 0) return 1;
        token = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}
