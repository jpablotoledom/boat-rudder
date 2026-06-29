#include "multipart_parser.h"
#include "memmem_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_PARTS 64

static void extract_header_value(const char *headers, size_t headers_len,
                                 const char *key, char *out, size_t out_size) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *pos = memmem(headers, headers_len, key, klen);
    if (!pos) return;
    pos += klen;

    if (klen > 0 && key[klen - 1] == '"') {
        const char *end = memchr(pos, '"', headers + headers_len - pos);
        if (!end) return;
        size_t len = (size_t)(end - pos);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, pos, len);
        out[len] = '\0';
    } else {
        const char *end = pos;
        while (end < headers + headers_len && *end != ';' && *end != '\r' && *end != ' ')
            end++;
        size_t len = (size_t)(end - pos);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, pos, len);
        out[len] = '\0';
    }
}

MultipartResult *parse_multipart(const char *body, size_t body_len,
                                 const char *content_type) {
    if (!body || !content_type) return NULL;

    const char *bp = strstr(content_type, "boundary=");
    if (!bp) return NULL;
    bp += 9;

    if (*bp == '"') {
        bp++;
        const char *end = strchr(bp, '"');
        if (!end) return NULL;
    }

    char boundary[256];
    snprintf(boundary, sizeof(boundary), "--%s", bp);
    char *nl = strchr(boundary, '\r');
    if (nl) *nl = '\0';
    nl = strchr(boundary, '\n');
    if (nl) *nl = '\0';
    nl = strchr(boundary, '"');
    if (nl) *nl = '\0';

    size_t blen = strlen(boundary);

    MultipartResult *result = calloc(1, sizeof(*result));
    if (!result) return NULL;
    result->parts = calloc(MAX_PARTS, sizeof(MultipartPart));
    if (!result->parts) { free(result); return NULL; }

    const char *part = memmem(body, body_len, boundary, blen);
    if (!part) { free(result->parts); free(result); return NULL; }

    while (part && part < body + body_len && result->count < MAX_PARTS) {
        const char *next = memmem(part + blen, body + body_len - (part + blen), boundary, blen);
        if (!next) next = body + body_len;

        const char *header_start = part + blen;
        if (header_start + 2 <= body + body_len && header_start[0] == '\r' && header_start[1] == '\n')
            header_start += 2;

        if (header_start >= body + body_len || (header_start[0] == '-' && header_start[1] == '-'))
            break;

        const char *header_end = memmem(header_start, next - header_start, "\r\n\r\n", 4);
        if (!header_end) { part = next; continue; }

        const char *data_start = header_end + 4;
        size_t data_len = next - data_start;
        if (data_len >= 2 && data_start[data_len - 2] == '\r' && data_start[data_len - 1] == '\n')
            data_len -= 2;

        size_t headers_len = (size_t)(header_end - header_start);
        MultipartPart *p = &result->parts[result->count];

        extract_header_value(header_start, headers_len, "name=\"", p->name, sizeof(p->name));
        extract_header_value(header_start, headers_len, "filename=\"", p->filename, sizeof(p->filename));

        const char *ct = memmem(header_start, headers_len, "Content-Type: ", 14);
        if (ct) {
            ct += 14;
            const char *ct_end = memmem(ct, header_end - ct, "\r\n", 2);
            if (!ct_end) ct_end = header_end;
            size_t ct_len = (size_t)(ct_end - ct);
            if (ct_len >= sizeof(p->content_type)) ct_len = sizeof(p->content_type) - 1;
            memcpy(p->content_type, ct, ct_len);
            p->content_type[ct_len] = '\0';
        }

        p->data = data_start;
        p->data_len = data_len;

        if (p->name[0] != '\0')
            result->count++;

        part = next;
    }

    return result;
}

void free_multipart(MultipartResult *result) {
    if (!result) return;
    free(result->parts);
    free(result);
}

const MultipartPart *multipart_find(const MultipartResult *result, const char *name) {
    if (!result || !name) return NULL;
    for (size_t i = 0; i < result->count; i++) {
        if (strcmp(result->parts[i].name, name) == 0)
            return &result->parts[i];
    }
    return NULL;
}
