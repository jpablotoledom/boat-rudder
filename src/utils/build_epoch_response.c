#include "build_epoch_response.h"
#include "detect_epoch.h"
#include "generate_url_theme.h"
#include "read_file.h"
#include "template_utils.h"
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
// don't auto-follow a 302's Location header. Loaded from
// html/themes/<theme>/redirect/redirect_epoch<N>.html.
static char *build_redirect_body(const char *location, int epoch) {
    char *path = generate_url_theme("redirect/redirect_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    char *body = render_template(tpl, location, location);
    free(tpl);
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
