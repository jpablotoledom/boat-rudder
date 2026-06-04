#ifndef HTTP_ROUTER_H
#define HTTP_ROUTER_H

#include <sys/types.h>

typedef ssize_t (*read_func_t)(void *ctx, char *buf, size_t count);

void http_route(read_func_t read_func, void *ctx, const char *root_directory);

#endif // HTTP_ROUTER_H
