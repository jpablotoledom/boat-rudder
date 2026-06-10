#include "build_epoch_response.h"
#include "detect_epoch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: SAMEORIGIN\r\n"

static const char *content_type_for_epoch(int epoch) {
    switch (epoch) {
        case EPOCH_WML:
            return "text/vnd.wap.wml";
        case EPOCH_PRESTANDARD:
        case EPOCH_EARLY:
            return "text/html";
        default:
            return "text/html; charset=UTF-8";
    }
}

char *build_epoch_response(const char *body, const char *extra_headers, int epoch) {
    const char *content_type = content_type_for_epoch(epoch);
    size_t body_len = strlen(body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        content_type, body_len, extra_headers);
    if (header_len < 0) return NULL;

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) return NULL;

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        content_type, body_len, extra_headers);

    memcpy(response + header_len, body, body_len + 1);

    return response;
}
