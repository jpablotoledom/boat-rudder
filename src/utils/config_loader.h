#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stdbool.h>

// Load key=value config from filename. Returns 0 on success, -1 on error.
// Accepts optional -c <path> flag via argc/argv (handled in main).
int load_config(const char *filename);

extern int  verbose_level;
extern int  http_port;
extern int  https_port;
extern bool ssl_enabled;
extern char ssl_cert[256];
extern char ssl_key[256];

// Comma-separated list of trusted reverse-proxy IPs.
// Only connections from these IPs have X-Real-IP / X-Forwarded-For honored.
// Example: "127.0.0.1,10.0.0.1"
extern char trusted_proxies[512];

// Active theme (selects the html/themes/<theme>/ directory).
extern char theme[64];

// Default content language (reserved for a future language selector).
extern char lang[16];

// Forces the browser "epoch" used for every "/" request, bypassing
// User-Agent detection. Valid values: -1 (WML) .. 3 (MODERN), see
// detect_epoch.h. Any other value (default -2) means "auto-detect".
extern int force_epoch;

// Public base URL of the site (reserved for future SEO/canonical links).
extern char public_url[256];

#endif // CONFIG_LOADER_H
