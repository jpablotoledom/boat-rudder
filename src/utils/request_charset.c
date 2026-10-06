#include "request_charset.h"
#include <string.h>

// Per-thread, per-request. See request_charset.h for why this is not a
// parameter.
static __thread int needs_legacy_charset = 0;

void request_charset_set(const char *user_agent) {
    // Cello is the only browser detect_epoch.c carves out this way today -
    // see that file's own note by name. Matched independently of epoch
    // resolution (force_epoch/preview_epoch never touch this), straight off
    // the request's real header.
    needs_legacy_charset = (user_agent && strstr(user_agent, "Cello") != NULL);
}

int request_needs_legacy_charset(void) {
    return needs_legacy_charset;
}
