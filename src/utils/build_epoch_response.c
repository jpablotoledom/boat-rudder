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
    return build_epoch_response_status(body, extra_headers, epoch, "200 OK");
}

char *build_epoch_response_status(const char *body, const char *extra_headers,
                                   int epoch, const char *status_line) {
    const char *content_type = content_type_for_epoch(epoch);
    size_t body_len = strlen(body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, content_type, body_len, extra_headers);
    if (header_len < 0) return NULL;

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) return NULL;

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, content_type, body_len, extra_headers);

    memcpy(response + header_len, body, body_len + 1);

    return response;
}

// Tiny epoch-appropriate "click here to continue" body for clients that
// don't auto-follow a 302's Location header.
static char *build_redirect_body(const char *location, int epoch) {
    const char *fmt;
    switch (epoch) {
        case EPOCH_WML:
            fmt =
                "<?xml version=\"1.0\"?>\n"
                "<!DOCTYPE wml PUBLIC \"-//WAPFORUM//DTD WML 1.1//EN\" \"http://www.wapforum.org/DTD/wml_1.1.xml\">\n"
                "<wml>\n"
                "<card id=\"redirect\" title=\"Redirect\">\n"
                "<p>Redirecting to <a href=\"%s\">%s</a></p>\n"
                "</card>\n"
                "</wml>\n";
            break;
        case EPOCH_PRESTANDARD:
            fmt =
                "<html>\n<head></head>\n<body>\n"
                "<p>Redirecting to <a href=\"%s\">%s</a></p>\n"
                "</body>\n</html>\n";
            break;
        case EPOCH_EARLY:
            fmt =
                "<html>\n<head></head>\n<body bgcolor=\"#000000\" text=\"#FFFFFF\" link=\"#56E9FD\">\n"
                "<p>Redirecting to <a href=\"%s\">%s</a></p>\n"
                "</body>\n</html>\n";
            break;
        default:
            fmt =
                "<!DOCTYPE html>\n<html lang=\"en\">\n<head><meta charset=\"UTF-8\"></head>\n<body>\n"
                "<p>Redirecting to <a href=\"%s\">%s</a></p>\n"
                "</body>\n</html>\n";
            break;
    }

    int len = snprintf(NULL, 0, fmt, location, location);
    if (len < 0) return NULL;

    char *body = malloc((size_t)len + 1);
    if (!body) return NULL;

    snprintf(body, (size_t)len + 1, fmt, location, location);
    return body;
}

char *build_redirect_response(const char *location, const char *extra_headers, int epoch) {
    char *body = build_redirect_body(location, epoch);
    if (!body) return NULL;

    const char *content_type = content_type_for_epoch(epoch);
    size_t body_len = strlen(body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        location, content_type, body_len, extra_headers);
    if (header_len < 0) {
        free(body);
        return NULL;
    }

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) {
        free(body);
        return NULL;
    }

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        location, content_type, body_len, extra_headers);

    memcpy(response + header_len, body, body_len + 1);
    free(body);

    return response;
}
