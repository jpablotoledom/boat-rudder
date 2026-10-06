#include "request_code_lines.h"
#include <string.h>

static __thread int hidden = 0;

void request_code_lines_set(const char *query_value) {
    hidden = (query_value && strcmp(query_value, "off") == 0);
}

int request_code_lines_hidden(void) {
    return hidden;
}
