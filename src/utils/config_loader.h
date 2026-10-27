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

// IPv4 address both listeners bind to; empty means every local address.
// An instance behind a front (multi-site hosting) binds 127.0.0.1.
extern char listen_address[64];

// Expect a PROXY protocol header (v1 or v2) at the start of every connection
// from a trusted_proxies peer, and take the client's address from it - see
// connection_thread.c and proxy_protocol.h. A loopback connection without one
// (the WAP gateway, the updater's health check) is served as direct; any
// other connection without one is closed.
extern bool proxy_protocol;

// Comma-separated list of trusted reverse-proxy IPs.
// Only connections from these IPs have X-Real-IP / X-Forwarded-For honored.
// Example: "127.0.0.1,10.0.0.1"
extern char trusted_proxies[512];

// Last-resort theme fallback: a request's theme is ?theme= -> `theme` cookie
// -> site_settings.active_theme -> this value (see utils/request_theme.h).
extern char theme[64];

// Content language fallback ("Eng"/"Esp"), used only when MongoDB is down or
// no `languages` document is the default (see db/cms_languages.h).
extern char lang[16];

// Forces the browser "epoch" used for every "/" request, bypassing
// User-Agent detection. Valid values: -1 (WML) .. 3 (MODERN), see
// detect_epoch.h. Any other value (default -2) means "auto-detect".
extern int force_epoch;

// Public base URL of the site, no trailing slash. Prefixes the absolute URLs
// encoded in QR codes (gallery/image QR pages in http_router.c); when empty
// they are built from the request's Host header.
extern char public_url[256];

// Behind a front that serves several sites on one address (multi-br), a
// browser that sends no Host header names no site, so the front routes it
// by a `?site=<site_key>` query parameter instead. When this is set, every
// page served to such a request carries `site=<site_key>` on its in-site
// links, images and redirects, so the visitor stays on this site - see
// utils/request_site.h. Empty (the default): a site on its own, nothing added.
extern char site_key[64];

// The sites a Host-less visitor can choose from, "key|Title;key|Title;..."
// (a key is the other site's own site_key). When set together with
// site_key, a Host-less request for "/" with no ?site= gets this list
// instead of the home page. Empty: no list, the home page as always.
extern char site_directory[1024];

// MongoDB connection URI and database name (login/sessions, epoch3 only).
extern char mongodb_uri[256];
extern char mongodb_db[64];

// Session cookie lifetime, in seconds.
extern int session_ttl_seconds;

// Anti-DDoS tunables (see server_listener.c): a global cap on concurrent
// connections, plus a per-IP rate window/threshold with a temporary ban
// (2x the window) once exceeded. ddos_max_ips sizes the per-IP tracking
// table (malloc'd once at server_start() from this value); once full,
// distinct new IPs go untracked until the cleanup thread frees stale
// entries (ddos_cleanup_interval_secs / ddos_ip_stale_secs).
extern int ddos_max_connections;
extern int ddos_rate_window_secs;
extern int ddos_rate_limit;
extern int ddos_max_ips;
extern int ddos_cleanup_interval_secs;
extern int ddos_ip_stale_secs;

// Per-connection socket read/write timeout, in seconds (SO_RCVTIMEO/
// SO_SNDTIMEO in connection_thread.c) - the slow-loris defense: a client
// that stalls mid-request/response longer than this gets its connection
// closed. Applies per read/write call, not to total transfer time, so a
// slow-but-steady large upload isn't affected by a low value here.
extern int connection_io_timeout_secs;

// WAP 1.x gateway (src/wap_gateway/wap_gateway.h): off unless enabled.
// wap_gateway_ips is a comma-separated list of local addresses to answer on;
// empty means every local address. A port of 0 disables that framing.
extern bool wap_gateway_enabled;
extern char wap_gateway_ips[256];
extern int  wap_gateway_rover_port;
extern int  wap_gateway_wsp_port;
// Max requests per minute answered per source IP (0 = unlimited) - see
// wap_gateway.c's rate_allow().
extern int  wap_gateway_rate_limit;
// Where the gateway fetches pages: "<IPv4>:<port>" of an HTTP server - a
// front routing by Host, so one gateway serves several sites - or empty for
// this server's own listener. wap_upstream_proxy_protocol sends that server
// a PROXY v1 header with the phone's address first.
extern char wap_upstream[64];
extern bool wap_upstream_proxy_protocol;

// GitHub repository ("owner/name") whose releases /dashboard/settings/updates
// lists - the same one scripts/boat-rudder-update installs from.
extern char update_repo[128];

#endif // CONFIG_LOADER_H
