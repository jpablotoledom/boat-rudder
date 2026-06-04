#ifndef HTTP_REQUEST_PARSER_H
#define HTTP_REQUEST_PARSER_H

#include "connection.h"

#define MAX_HEADERS      64
#define MAX_HEADER_KEY   64
#define MAX_HEADER_VALUE 512

typedef struct {
    char key[MAX_HEADER_KEY];
    char value[MAX_HEADER_VALUE];
} HttpHeader;

typedef struct {
    char       method[16];
    char       url[2048];       // 2 KiB - matches practical client URL length limits
    char       protocol[16];
    HttpHeader headers[MAX_HEADERS];
    int        header_count;
    char      *body;
    int        body_length;
} HttpRequest;

int parse_http_request(const char *raw_request, int raw_len, HttpRequest *req);

#endif // HTTP_REQUEST_PARSER_H
