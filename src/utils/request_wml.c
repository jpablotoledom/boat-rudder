#include "request_wml.h"
#include <stdio.h>
#include <string.h>

static __thread char target[1024] = "/";
static __thread int unpaged = 0;

void request_wml_set(const char *raw_target, const char *wml_pages) {
    snprintf(target, sizeof(target), "%s", raw_target && raw_target[0] ? raw_target : "/");
    unpaged = wml_pages && strcmp(wml_pages, "all") == 0;
}

const char *request_wml_target(void) {
    return target;
}

int request_wml_unpaged(void) {
    return unpaged;
}
