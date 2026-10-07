#ifndef MULTIPART_PARSER_H
#define MULTIPART_PARSER_H

#include <stddef.h>

typedef struct {
    char name[128];
    char filename[256];
    char content_type[64];
    const char *data;
    size_t data_len;
} MultipartPart;

typedef struct {
    MultipartPart *parts;
    size_t count;
} MultipartResult;

MultipartResult *parse_multipart(const char *body, size_t body_len,
                                 const char *content_type);
void free_multipart(MultipartResult *result);
const MultipartPart *multipart_find(const MultipartResult *result, const char *name);

#endif
