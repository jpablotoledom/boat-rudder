#include "request_theme.h"
#include "config_loader.h"
#include "../db/cms_site_settings.h"
#include <stdlib.h>
#include <string.h>

// Per-thread, per-request. See request_theme.h for why this is not a parameter.
static __thread char current_theme[64] = "";

void request_theme_set(void) {
    char *key = cms_get_active_theme_key(); // never NULL - see its own doc
    strncpy(current_theme, key, sizeof(current_theme) - 1);
    current_theme[sizeof(current_theme) - 1] = '\0';
    free(key);
}

const char *request_theme(void) {
    return current_theme[0] ? current_theme : theme;
}
