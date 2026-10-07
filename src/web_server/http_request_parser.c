#include "http_request_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char *trim(char *str) {
    while (*str == ' ' || *str == '\t') str++;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return str;
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
