#include "url_parser.h"
#include <stdlib.h>
#include <string.h>

int url_parse(const char *url, char *route, size_t route_size,
              QueryParam *params, int *param_count) {
    if (!url || !route || !params || !param_count) return -1;

    const char *query_start = strchr(url, '?');
    size_t route_len = query_start ? (size_t)(query_start - url) : strlen(url);

    if (route_len >= route_size) route_len = route_size - 1;
    strncpy(route, url, route_len);
    route[route_len] = '\0';

    *param_count = 0;
    if (!query_start) return 0;

    const char *param = query_start + 1;
    while (param && *param && *param_count < MAX_PARAMS) {
        const char *eq   = strchr(param, '=');
        const char *next = strchr(param, '&');

        if (!eq) break;

        size_t key_len = eq - param;
        if (key_len >= MAX_PARAM_LENGTH) key_len = MAX_PARAM_LENGTH - 1;
        strncpy(params[*param_count].key, param, key_len);
        params[*param_count].key[key_len] = '\0';

        eq++;
        size_t val_len = next ? (size_t)(next - eq) : strlen(eq);
        if (val_len >= MAX_PARAM_LENGTH) val_len = MAX_PARAM_LENGTH - 1;
        strncpy(params[*param_count].value, eq, val_len);
        params[*param_count].value[val_len] = '\0';

        (*param_count)++;
        param = next ? next + 1 : NULL;
    }

    return 0;
}
