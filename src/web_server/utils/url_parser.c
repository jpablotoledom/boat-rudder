#include "url_parser.h"
#include <stdlib.h>
#include <string.h>

int url_parse(const char *url, char *route, size_t route_size,
              QueryParam *params, int *param_count) {
    if (!url || !route || !params || !param_count) return -1;

    // A "#fragment" belongs to the browser, but some early ones (pre-HTML
    // 2.0) send it along: it ends the URL, never reaching a path or a value.
    const char *end = strchr(url, '#');
    if (!end) end = url + strlen(url);

    const char *query_start = memchr(url, '?', (size_t)(end - url));
    size_t route_len = (size_t)((query_start ? query_start : end) - url);

    if (route_len >= route_size) route_len = route_size - 1;
    memcpy(route, url, route_len);
    route[route_len] = '\0';

    *param_count = 0;
    if (!query_start) return 0;

    const char *param = query_start + 1;
    while (param && param < end && *param_count < MAX_PARAMS) {
        const char *eq   = memchr(param, '=', (size_t)(end - param));
        const char *next = memchr(param, '&', (size_t)(end - param));

        if (!eq) break;
        if (next && next < eq) { param = next + 1; continue; } // a key with no value

        size_t key_len = eq - param;
        if (key_len >= MAX_PARAM_LENGTH) key_len = MAX_PARAM_LENGTH - 1;
        memcpy(params[*param_count].key, param, key_len);
        params[*param_count].key[key_len] = '\0';

        eq++;
        size_t val_len = (size_t)((next ? next : end) - eq);
        if (val_len >= MAX_PARAM_LENGTH) val_len = MAX_PARAM_LENGTH - 1;
        memcpy(params[*param_count].value, eq, val_len);
        params[*param_count].value[val_len] = '\0';

        (*param_count)++;
        param = next ? next + 1 : NULL;
        // Every href is written with "&amp;" (well-formed HTML, and required
        // by WML), but a browser older than HTML 2.0 - Cello - sends it as
        // it is: "&amp;" separates params just as "&" does.
        if (param && end - param >= 4 && strncmp(param, "amp;", 4) == 0) param += 4;
    }

    return 0;
}
