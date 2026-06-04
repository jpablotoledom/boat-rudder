#ifndef URL_PARSER_H
#define URL_PARSER_H

#include <stddef.h>
#include "../http_constants.h"

// A single key=value pair from a URL query string.
typedef struct {
    char key[MAX_PARAM_LENGTH];
    char value[MAX_PARAM_LENGTH];
} QueryParam;

// Split a URL into its path and query parameters.
// route must be at least MAX_PATH_LENGTH bytes.
// Returns 0 on success, -1 on invalid arguments.
int url_parse(const char *url, char *route, size_t route_size,
              QueryParam *params, int *param_count);

#endif // URL_PARSER_H
