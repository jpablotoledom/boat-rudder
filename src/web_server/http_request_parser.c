#include "http_request_parser.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static char *trim(char *str) {
    while (*str == ' ' || *str == '\t') str++;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return str;
}

// Absolute-form request target ("GET http://host/path HTTP/1.1", RFC 9112
// 3.2.2): what a client sends to a proxy, and what a phone whose connection
// profile names one - UC Browser on Java ME, confirmed live - sends on to
// the server too. Reduces `url` in place to its origin form ("/path", "/"
// when the authority has no path after it) and copies the authority into
// `authority` ("" when `url` is already origin-form).
static void split_absolute_form(char *url, char *authority, size_t authority_size) {
    authority[0] = '\0';
    size_t scheme = strncasecmp(url, "http://", 7) == 0  ? 7
                  : strncasecmp(url, "https://", 8) == 0 ? 8 : 0;
    if (!scheme) return;

    const char *host = url + scheme;
    size_t host_len = strcspn(host, "/?#");
    snprintf(authority, authority_size, "%.*s", (int)host_len, host);

    const char *rest = host + host_len;
    if (*rest == '/') {
        memmove(url, rest, strlen(rest) + 1);
    } else {
        size_t len = strlen(rest);
        memmove(url + 1, rest, len + 1);
        url[0] = '/';
    }
}

int parse_http_request(const char *raw_request, int raw_len, HttpRequest *req) {
    if (!raw_request || !req) return -1;

    memset(req, 0, sizeof(HttpRequest));

    char *buffer = malloc(raw_len + 1);
    if (!buffer) return -1;
    memcpy(buffer, raw_request, raw_len);
    buffer[raw_len] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\r\n", &saveptr);
    if (!line) { free(buffer); return -1; }

    // A real HTTP/0.9 "simple request" is just "GET /path" - no version
    // token, and (per spec) no headers ever follow it either. req->protocol
    // stays "" (the struct was zeroed above), which callers use to detect
    // this case and skip anything that assumes a header block exists.
    int fields = sscanf(line, "%15s %1023s %15s", req->method, req->url, req->protocol);
    if (fields != 3 && fields != 2) {
        free(buffer);
        return -1;
    }

    char authority[MAX_HEADER_VALUE];
    split_absolute_form(req->url, authority, sizeof(authority));

    req->header_count = 0;
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL && strlen(line) > 0) {
        if (req->header_count >= MAX_HEADERS) break;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        strncpy(req->headers[req->header_count].key,   trim(line),       MAX_HEADER_KEY   - 1);
        strncpy(req->headers[req->header_count].value, trim(colon + 1),  MAX_HEADER_VALUE - 1);
        req->header_count++;
    }

    // With an absolute-form target, its authority is the host asked for -
    // any Host header is ignored in its favor (RFC 9112 3.2.2), and one is
    // added when the client sent none.
    if (authority[0]) {
        int h = 0;
        while (h < req->header_count && strcasecmp(req->headers[h].key, "Host") != 0) h++;
        if (h == req->header_count && h < MAX_HEADERS) {
            strncpy(req->headers[h].key, "Host", MAX_HEADER_KEY - 1);
            req->header_count++;
        }
        if (h < req->header_count)
            snprintf(req->headers[h].value, MAX_HEADER_VALUE, "%s", authority);
    }

    const char *body_start = strstr(raw_request, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int body_len = raw_len - (int)(body_start - raw_request);
        if (body_len > 0) {
            req->body = malloc(body_len + 1);
            if (req->body) {
                memcpy(req->body, body_start, body_len);
                req->body[body_len] = '\0';
                req->body_length = body_len;
            }
        }
    }

    free(buffer);
    return 0;
}
