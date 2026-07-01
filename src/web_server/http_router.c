#define _XOPEN_SOURCE 700

#include "http_router.h"
#include "http_constants.h"
#include "connection.h"
#include "http_request_parser.h"
#include "utils/url_parser.h"
#include "utils/static_file_server.h"
#include "../db/auth.h"
#include "../db/cms_categories.h"
#include "../db/cms_entries.h"
#include "../db/cms_entries_admin.h"
#include "../db/cms_languages.h"
#include "../db/cms_menu.h"
#include "../db/cms_users_admin.h"
#include "../db/mongodb_manager.h"
#include "../db/session_manager.h"
#include "../html_builder/orchestrator.h"
#include "../modules/blog_list/blog_list.h"
#include "../modules/category_menu/category_menu.h"
#include "../modules/categories_admin/categories_admin.h"
#include "../modules/dashboard/dashboard.h"
#include "../modules/entry_editor/entry_editor.h"
#include "../modules/entry_editor/entry_editor_blocks.h"
#include "../modules/entry_page/entry_page.h"
#include "../modules/error/error.h"
#include "../modules/languages_admin/languages_admin.h"
#include "../modules/login/login.h"
#include "../modules/media_admin/media_admin.h"
#include "../modules/menu_admin/menu_admin.h"
#include "../modules/users_admin/users_admin.h"
#include "../db/cms_media.h"
#include "../db/cms_media_galleries.h"
#include "utils/multipart_parser.h"
#include "../utils/build_epoch_response.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/config_loader.h"
#include "../utils/detect_epoch.h"
#include "../utils/json_utils.h"
#include "../utils/log.h"
#include "../utils/http_utils.h"
#include "../utils/template_utils.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *get_header_value(HttpRequest *req, const char *key) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].key, key) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

static void send_simple(void *ctx, const char *status, const char *body) {
    char header[512];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, body_len);
    connection_write(ctx, header, strlen(header));
    connection_write(ctx, body, body_len);
}

// Computes the browser "epoch" for `req`: the configured `force_epoch`
// override, if set, otherwise detect_epoch(User-Agent).
static int resolve_epoch(HttpRequest *req) {
    if (force_epoch >= EPOCH_WML && force_epoch <= EPOCH_MODERN) return force_epoch;
    return detect_epoch(get_header_value(req, "User-Agent"));
}

// Renders the centralized error_epoch<N>.html template via error_content() +
// buildPageWebSite(), and sends it with `status_line`. Falls back to
// send_simple() if the template could not be loaded/rendered.
static void send_error_response(void *ctx, int status_code, const char *status_line, int epoch) {
    char title[64];
    snprintf(title, sizeof(title), "Boat Rudder - Error %d", status_code);

    char *content  = error_content(epoch, status_code, NULL);
    char *body     = buildPageWebSite(epoch, title, content);
    char *response = body ? build_epoch_response_status(body, "", epoch, status_line) : NULL;
    free(body);

    if (!response) {
        // Last-resort fallback: template loading itself failed, so this must
        // not depend on the filesystem/template pipeline. Plain text only.
        send_simple(ctx, status_line, status_line);
        return;
    }

    connection_write(ctx, response, strlen(response));
    free(response);
}

// Sends a malloc'd full HTTP response, truncating to headers-only for HEAD
// requests, and frees it. If `response` is NULL (some step failed), sends an
// epoch-aware 500 error page instead.
static void send_or_error(void *ctx, char *response, const char *method, int epoch) {
    if (!response) {
        send_error_response(ctx, 500, "500 Internal Server Error", epoch);
        return;
    }

    size_t response_len = strlen(response);
    if (strcmp(method, "HEAD") == 0) {
        const char *header_end = strstr(response, "\r\n\r\n");
        response_len = header_end ? (size_t)(header_end - response) + 4 : response_len;
    }
    connection_write(ctx, response, response_len);
    free(response);
}

// Looks up db.entries.findOne({link, enabled: true}) and renders it via
// entry_page() + buildPageWebSite(), used by both /page/<link>
// (expected_type "page") and /blog/<link> (expected_type "blog"). Sends a
// 404 if the entry doesn't exist, mongodb is not ready, or entry.type !=
// expected_type. Takes ownership of `category_menu_html` (pass NULL for
// non-blog pages).
static void serve_cms_entry(void *ctx, const char *link, const char *expected_type,
                             const char *lang, const char *method, int epoch,
                             char *category_menu_html) {
    CmsEntry entry;
    if (mongodb_manager_is_ready() && cms_get_entry_by_link(link, lang, &entry)) {
        if (strcmp(entry.type, expected_type) != 0) {
            cms_entry_free(&entry);
            free(category_menu_html);
            send_error_response(ctx, 404, "404 Not Found", epoch);
            return;
        }

        // For blog entries highlight the "/blog" menu item (section match).
        // For pages use the full "/page/<link>" URL (menu items point to specific pages).
        char current_url[256];
        if (strcmp(expected_type, "blog") == 0)
            snprintf(current_url, sizeof(current_url), "/blog");
        else
            snprintf(current_url, sizeof(current_url), "/page/%s", link);

        char *title   = strdup(entry.header_title);
        char *content = entry_page(&entry, epoch);
        cms_entry_free(&entry);

        char *body = NULL;
        if (content && title) {
            body = buildEntryWebSiteAtUrl(epoch, title, content, current_url, category_menu_html);
            category_menu_html = NULL; // ownership transferred
        } else {
            free(content);
            free(category_menu_html);
        }
        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
        free(title);
        free(body);
        send_or_error(ctx, response, method, epoch);
    } else {
        free(category_menu_html);
        send_error_response(ctx, 404, "404 Not Found", epoch);
    }
}

// Shared by every /dashboard/* sub-route: sends a 503 and returns 0 if
// mongodb is not ready; sends a 302 to /login and returns 0 if `req` has no
// valid session cookie; otherwise writes the session's user id into
// user_id_out (>= USER_ID_HEX_BUF_SIZE bytes) and returns 1. Callers must not
// write to `ctx` if this returns 0.
static int require_dashboard_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    if (!mongodb_manager_is_ready()) {
        send_error_response(ctx, 503, "503 Service Unavailable", epoch);
        return 0;
    }

    const char *cookie = get_header_value(req, "Cookie");
    if (validate_session_cookie(cookie, user_id_out) != 1) {
        char *response = build_redirect_response("/login", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    return 1;
}

// require_dashboard_session() + cms_get_user_role(): role_out (>= USER_ROLE_BUF_SIZE
// bytes) is "admin" or "author" (a missing `role` field, or a lookup error, defaults
// to "admin" - same backward-compatible convention as cms_get_user_role()). Same
// 0/1 return + ctx-write contract as require_dashboard_session().
static int require_dashboard_session_role(void *ctx, HttpRequest *req, int epoch,
                                            char *user_id_out, char *role_out, size_t role_size) {
    if (!require_dashboard_session(ctx, req, epoch, user_id_out)) return 0;

    if (cms_get_user_role(user_id_out, role_out, role_size) != 0) {
        strncpy(role_out, "admin", role_size - 1);
        role_out[role_size - 1] = '\0';
    }

    return 1;
}

// require_dashboard_session_role() + role != "admin" -> 302 /dashboard, return 0.
// Used by routes reserved to Administrador (Users maintainer, entry deletion).
static int require_admin_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    char role[USER_ROLE_BUF_SIZE];
    if (!require_dashboard_session_role(ctx, req, epoch, user_id_out, role, sizeof(role))) return 0;

    if (strcmp(role, "admin") != 0) {
        char *response = build_redirect_response("/dashboard", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    return 1;
}

// 1 if `role` is "admin", or `role` is "author" and `entry` is a "blog" entry
// created by `user_id`. Gates the entry editor GET/POST routes - an Autor may
// only edit their own blog entries.
static int can_edit_entry(const char *role, const char *user_id, const CmsEntryEdit *entry) {
    if (strcmp(role, "admin") == 0) return 1;

    return strcmp(role, "author") == 0 &&
           strcmp(entry->type, "blog") == 0 &&
           strcmp(entry->created_by, user_id) == 0;
}

// Matches `decoded_url` against "<prefix>/<id><suffix>", where <id> is a
// single non-empty path segment (no '/'). On match, copies <id> into id_out
// (truncated to id_size - 1) and returns 1. `suffix` may be "" (the id is
// the last path component) or e.g. "/edit". Returns 0 on no match (id_out
// untouched).
static int match_id_route(const char *decoded_url, const char *prefix,
                           const char *suffix, char *id_out, size_t id_size) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(decoded_url, prefix, prefix_len) != 0 || decoded_url[prefix_len] != '/')
        return 0;

    const char *seg_start = decoded_url + prefix_len + 1;
    const char *slash = strchr(seg_start, '/');
    const char *seg_end;

    if (suffix[0] == '\0') {
        if (slash) return 0;
        seg_end = seg_start + strlen(seg_start);
    } else {
        if (!slash || strcmp(slash, suffix) != 0) return 0;
        seg_end = slash;
    }

    size_t seg_len = (size_t)(seg_end - seg_start);
    if (seg_len == 0 || seg_len >= id_size) return 0;

    memcpy(id_out, seg_start, seg_len);
    id_out[seg_len] = '\0';
    return 1;
}

// Matches "/dashboard/api/entries/<entry_id>/blocks/<block_id>/delete". On
// match, copies both ids (truncated to their buffer sizes) and returns 1; 0
// on no match (out buffers untouched).
static int match_block_delete_route(const char *decoded_url, char *entry_id_out,
                                     size_t entry_id_size, char *block_id_out,
                                     size_t block_id_size) {
    static const char prefix[] = "/dashboard/api/entries/";
    static const char mid[]    = "/blocks/";
    static const char suffix[] = "/delete";

    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    const char *entry_start = decoded_url + prefix_len;
    const char *mid_start = strstr(entry_start, mid);
    if (!mid_start) return 0;

    const char *block_start = mid_start + (sizeof(mid) - 1);
    size_t block_len = strlen(block_start);
    size_t suffix_len = sizeof(suffix) - 1;
    if (block_len <= suffix_len || strcmp(block_start + (block_len - suffix_len), suffix) != 0)
        return 0;
    block_len -= suffix_len;

    size_t entry_len = (size_t)(mid_start - entry_start);
    if (entry_len == 0 || entry_len >= entry_id_size) return 0;
    if (block_len == 0 || block_len >= block_id_size) return 0;

    memcpy(entry_id_out, entry_start, entry_len);
    entry_id_out[entry_len] = '\0';
    memcpy(block_id_out, block_start, block_len);
    block_id_out[block_len] = '\0';
    return 1;
}

// Returns the value of a query parameter by key, or "" if not found.
static const char *get_query_param(QueryParam *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) return params[i].value;
    }
    return "";
}

// Extracts the url-decoded value of `key` from an
// "application/x-www-form-urlencoded" body into `out` (NUL-terminated,
// truncated to out_size - 1). Returns 1 if `key` was found, 0 otherwise
// (out is left as an empty string).
static int parse_urlencoded_field(const char *body, int body_length, const char *key,
                                   char *out, size_t out_size) {
    if (out_size == 0) return 0;
    out[0] = '\0';
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) return 0;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            char raw[8192];
            size_t val_len = (size_t)(field_end - (eq + 1));
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            char decoded[8192];
            url_decode(decoded, raw);
            strncpy(out, decoded, out_size - 1);
            out[out_size - 1] = '\0';
            return 1;
        }

        p = amp ? amp + 1 : end;
    }
    return 0;
}

// Like parse_urlencoded_field(), but collects every value for `key` (not just
// the first), e.g. repeated "categories=..." fields from a
// <select multiple>. Each out_values[i] is malloc'd (caller must free).
// Returns the number of values found, up to max_count.
static size_t parse_urlencoded_multi(const char *body, int body_length, const char *key,
                                      char **out_values, size_t max_count) {
    size_t count = 0;
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end && count < max_count) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) break;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            char raw[8192];
            size_t val_len = (size_t)(field_end - (eq + 1));
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            char decoded[8192];
            url_decode(decoded, raw);
            out_values[count++] = strdup(decoded);
        }

        p = amp ? amp + 1 : end;
    }

    return count;
}

// Fills values[i] (caller-allocated, lang_count entries) with the url-decoded
// "name_<langs[i].code>" field from req's body, for
// cms_create_category()/cms_update_category(). Each values[i] is malloc'd
// and must be freed by the caller.
static void parse_category_form(HttpRequest *req, const CmsLanguageItem *langs,
                                 size_t lang_count, char **values) {
    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_link/out_order/out_enabled and values[i] (caller-allocated,
// lang_count entries) from req's body's "link"/"order"/"enabled"/
// "name_<langs[i].code>" fields, for cms_create_menu_item()/
// cms_update_menu_item(). Each values[i] is malloc'd and must be freed by
// the caller. An unchecked "enabled" checkbox sends no field at all, so its
// absence means false.
static void parse_menu_form(HttpRequest *req, char *out_link, size_t out_link_size,
                             int *out_order, bool *out_enabled,
                             const CmsLanguageItem *langs, size_t lang_count, char **values) {
    parse_urlencoded_field(req->body, req->body_length, "link", out_link, out_link_size);

    char order_str[16];
    parse_urlencoded_field(req->body, req->body_length, "order", order_str, sizeof(order_str));
    *out_order = atoi(order_str);

    char enabled_str[8];
    *out_enabled = parse_urlencoded_field(req->body, req->body_length, "enabled",
                                           enabled_str, sizeof(enabled_str)) != 0;

    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_email/out_password/out_role (caller-allocated buffers) from req's
// body's "email"/"password"/"role" fields, for cms_create_user()/
// cms_update_user(). An absent "role" field defaults to "author" (the safer
// default for a new account).
static void parse_user_form(HttpRequest *req, char *out_email, size_t out_email_size,
                             char *out_password, size_t out_password_size,
                             char *out_role, size_t out_role_size) {
    parse_urlencoded_field(req->body, req->body_length, "email", out_email, out_email_size);
    parse_urlencoded_field(req->body, req->body_length, "password", out_password, out_password_size);

    if (!parse_urlencoded_field(req->body, req->body_length, "role", out_role, out_role_size)) {
        strncpy(out_role, "author", out_role_size - 1);
        out_role[out_role_size - 1] = '\0';
    }
}

void http_route(read_func_t read_func, void *ctx, const char *root_directory) {
    LOG_DEBUG("http_route() called");

    if (!ctx) {
        LOG_ERROR("http_route: ctx is NULL");
        return;
    }

    char *raw_request = malloc(RAW_REQUEST_SIZE);
    if (!raw_request) {
        LOG_ERROR("http_route: malloc failed");
        connection_close(ctx);
        return;
    }

    HttpRequest req;
    memset(&req, 0, sizeof(req));

    // --- Read headers ---
    int bytes_read = 0;
    while (bytes_read < (int)RAW_REQUEST_SIZE - 1) {
        int ret = read_func(ctx, raw_request + bytes_read,
                            RAW_REQUEST_SIZE - 1 - bytes_read);
        if (ret > 0) {
            bytes_read += ret;
            if (strstr(raw_request, "\r\n\r\n")) break;
            if (bytes_read >= (int)RAW_REQUEST_SIZE - 1) {
                LOG_WARN("Headers exceed %d bytes", RAW_REQUEST_SIZE);
                send_error_response(ctx, 431, "431 Request Header Fields Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }
            continue;
        }
        if (ret == 0) {
            LOG_DEBUG("Peer closed connection while reading");
        } else {
            LOG_ERROR("read_func error: %d", ret);
        }
        goto cleanup;
    }

    raw_request[bytes_read] = '\0';
    if (bytes_read == 0) goto cleanup;

    // --- Read full body based on Content-Length ---
    {
        const char *hdr_end = strstr(raw_request, "\r\n\r\n");
        if (hdr_end) {
            int header_size = (int)(hdr_end + 4 - raw_request);
            int body_in_buffer = bytes_read - header_size;

            // Extract Content-Length from raw headers
            int content_length = 0;
            for (const char *h = raw_request; h < hdr_end; h++) {
                if (h[0] == '\r' && h[1] == '\n' &&
                    strncasecmp(h + 2, "Content-Length:", 15) == 0) {
                    const char *v = h + 17;
                    while (*v == ' ') v++;
                    content_length = atoi(v);
                    break;
                }
            }

            if (content_length > (int)MAX_BODY_SIZE) {
                LOG_WARN("Body too large: %d > %d", content_length, (int)MAX_BODY_SIZE);
                send_error_response(ctx, 413, "413 Payload Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }

            if (content_length > 0 && content_length > body_in_buffer) {
                int total_needed = header_size + content_length;
                char *big_buf = realloc(raw_request, total_needed + 1);
                if (!big_buf) {
                    LOG_ERROR("realloc failed for body (%d bytes)", total_needed);
                    send_error_response(ctx, 500, "500 Internal Server Error", EPOCH_PRESTANDARD);
                    goto cleanup;
                }
                raw_request = big_buf;

                int remaining = content_length - body_in_buffer;
                int offset = bytes_read;
                while (remaining > 0) {
                    int ret = read_func(ctx, raw_request + offset, remaining);
                    if (ret <= 0) {
                        LOG_ERROR("Connection closed while reading body (%d remaining)", remaining);
                        goto cleanup;
                    }
                    offset += ret;
                    remaining -= ret;
                }
                bytes_read = offset;
                raw_request[bytes_read] = '\0';
            }
        }
    }

    // --- Parse request line + headers ---
    if (parse_http_request(raw_request, bytes_read, &req) != 0) {
        LOG_WARN("Malformed HTTP request");
        send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
        goto cleanup;
    }

    {
        char m[16], u[2048], p[16];
        if (sscanf(raw_request, "%15s %2047s %15s", m, u, p) != 3 ||
            strncmp(p, "HTTP/", 5) != 0) {
            send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
            goto cleanup;
        }
    }

    LOG_INFO("%s %s %s", req.method, req.url, req.protocol);

    // --- Determine real client IP ---
    // Honor X-Real-IP / X-Forwarded-For only from trusted reverse proxies.
    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    {
        connection_ctx_t *conn = (connection_ctx_t *)ctx;
        struct sockaddr_storage peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        char peer_ip[INET6_ADDRSTRLEN] = "unknown";

        if (getpeername(conn->client_socket, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
            if (peer_addr.ss_family == AF_INET) {
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)&peer_addr)->sin_addr,
                          peer_ip, sizeof(peer_ip));
            } else if (peer_addr.ss_family == AF_INET6) {
                inet_ntop(AF_INET6,
                          &((struct sockaddr_in6 *)&peer_addr)->sin6_addr,
                          peer_ip, sizeof(peer_ip));
            }
        }

        if (is_trusted_proxy(peer_ip)) {
            const char *real_ip  = get_header_value(&req, "X-Real-IP");
            const char *fwd      = get_header_value(&req, "X-Forwarded-For");
            if (real_ip && real_ip[0]) {
                strncpy(client_ip, real_ip, sizeof(client_ip) - 1);
            } else if (fwd && fwd[0]) {
                strncpy(client_ip, fwd, sizeof(client_ip) - 1);
                char *comma = strchr(client_ip, ',');
                if (comma) *comma = '\0';
                // trim leading space
                char *p = client_ip;
                while (*p == ' ') p++;
                if (p != client_ip) memmove(client_ip, p, strlen(p) + 1);
            } else {
                strncpy(client_ip, peer_ip, sizeof(client_ip) - 1);
            }
        } else {
            strncpy(client_ip, peer_ip, sizeof(client_ip) - 1);
        }
        client_ip[sizeof(client_ip) - 1] = '\0';
    }

    {
        const char *ua = get_header_value(&req, "User-Agent");
        LOG_DEBUG("Client IP: %s | UA: %s", client_ip, ua ? ua : "(none)");
    }

    // --- Route ---
    {
        char route[2048];
        QueryParam params[MAX_PARAMS];
        int param_count = 0;
        char url_copy[2048];
        strncpy(url_copy, req.url, sizeof(url_copy) - 1);
        url_copy[sizeof(url_copy) - 1] = '\0';
        url_parse(url_copy, route, sizeof(route), params, &param_count);

        char decoded_url[2048];
        url_decode(decoded_url, route);

        char content_lang[16];
        cms_resolve_default_lang(content_lang, sizeof(content_lang));

        char id[32];
        char block_id[32];

        if (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0) {
            if (strcmp(decoded_url, "/") == 0) {
                int epoch = resolve_epoch(&req);

                char *body = buildHomeWebSite(epoch, content_lang);
                if (!body) LOG_ERROR("buildHomeWebSite failed for epoch %d", epoch);

                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/login") == 0) {
                int epoch = resolve_epoch(&req);

                char user_id[USER_ID_HEX_BUF_SIZE];
                const char *cookie = get_header_value(&req, "Cookie");

                if (mongodb_manager_is_ready() && validate_session_cookie(cookie, user_id) == 1) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = login(epoch, NULL);
                    char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strcmp(decoded_url, "/dashboard") == 0) {
                int epoch = resolve_epoch(&req);

                if (!mongodb_manager_is_ready()) {
                    send_error_response(ctx, 503, "503 Service Unavailable", epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    const char *cookie = get_header_value(&req, "Cookie");

                    if (validate_session_cookie(cookie, user_id) == 1) {
                        char role[USER_ROLE_BUF_SIZE];
                        if (cms_get_user_role(user_id, role, sizeof(role)) != 0) {
                            strncpy(role, "admin", sizeof(role) - 1);
                            role[sizeof(role) - 1] = '\0';
                        }

                        char *content  = dashboard(epoch, content_lang, user_id, role);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *response = build_redirect_response("/login", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = categories_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = categories_admin_form(epoch, "", langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        if (cms_get_category_name_values(id, langs, lang_count, values) == 0) {
                            char *content  = categories_admin_form(epoch, id, langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/languages") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = languages_admin(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = menu_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = menu_admin_form(epoch, "", "", 0, false, langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        char link[256];
                        int order;
                        bool enabled;
                        if (cms_get_menu_item_values(id, langs, lang_count, values,
                                                      link, sizeof(link), &order, &enabled) == 0) {
                            char *content  = menu_admin_form(epoch, id, link, order, enabled,
                                                              langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = users_admin_list(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = users_admin_form(epoch, "", "", "author", NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char email[256];
                        char role[USER_ROLE_BUF_SIZE];
                        if (cms_get_user_values(id, email, sizeof(email), role, sizeof(role)) == 0) {
                            char *content  = users_admin_form(epoch, id, email, role, NULL);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/entries", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        CmsEntryEdit entry;
                        if (cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                            if (!can_edit_entry(role, user_id, &entry)) {
                                char *response = build_redirect_response("/dashboard", "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                CmsCategoryItem *categories = NULL;
                                size_t category_count = 0;
                                cms_get_categories(content_lang, &categories, &category_count);

                                char *content  = entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count);
                                char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                                free(body);
                                send_or_error(ctx, response, req.method, epoch);

                                cms_categories_free(categories, category_count);
                            }
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_entry_edit_free(&entry, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/media") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(&dirs, &dir_count);

                        char *content  = media_admin_page(epoch, dirs, dir_count, NULL, 0);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Media", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/contents") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        const char *dir_id = get_query_param(params, param_count, "directory");
                        const char *start_str = get_query_param(params, param_count, "start");
                        const char *end_str = get_query_param(params, param_count, "end");
                        int skip = atoi(start_str);
                        int limit = atoi(end_str) - skip;
                        if (limit <= 0) limit = MEDIA_PAGE_SIZE;

                        CmsMediaItem *items = NULL;
                        size_t item_count = 0;
                        cms_get_media_items(dir_id[0] ? dir_id : NULL, skip, limit, &items, &item_count);

                        char *html = media_admin_render_items(items, item_count, epoch);
                        char *response = build_json_response(html ? html : "");
                        free(html);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);

                        cms_media_items_free(items, item_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/modal") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(&dirs, &dir_count);

                        char *html = media_admin_modal(epoch, dirs, dir_count, NULL, 0);
                        if (html) {
                            char header_buf[128];
                            snprintf(header_buf, sizeof(header_buf),
                                     "Content-Type: text/html; charset=UTF-8\r\n"
                                     "Content-Length: %zu\r\n", strlen(html));
                            char response_buf[256];
                            snprintf(response_buf, sizeof(response_buf),
                                     "HTTP/1.1 200 OK\r\n%s\r\n", header_buf);
                            connection_write(ctx, response_buf, strlen(response_buf));
                            connection_write(ctx, html, strlen(html));
                            free(html);
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "Error");
                        }
                        connection_close(ctx);

                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/logout") == 0) {
                int epoch = resolve_epoch(&req);

                char token[SESSION_TOKEN_BUF_SIZE];
                const char *cookie = get_header_value(&req, "Cookie");
                if (mongodb_manager_is_ready() && extract_session_token(cookie, token, sizeof(token))) {
                    destroy_session(token);
                }

                char clear_cookie[128];
                build_session_clear_cookie_header(clear_cookie, sizeof(clear_cookie));

                char *response = build_redirect_response("/", clear_cookie, epoch);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/page/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                serve_cms_entry(ctx, decoded_url + 6, "page", content_lang, req.method, epoch, NULL);

            } else if (strncmp(decoded_url, "/gallery/", 9) == 0 && decoded_url[9] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *gallery_id = decoded_url + 9;
                const char *img_str = get_query_param(params, param_count, "img");
                int img_index = img_str[0] ? atoi(img_str) : 0;

                if (!mongodb_manager_is_ready() || strlen(gallery_id) != 24) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    CmsMediaGallery gallery;
                    if (!cms_get_media_gallery(gallery_id, &gallery)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else if (epoch >= 3) {
                        char *item_tpl_path = generate_url_theme("elements/gallery/gallery-page-item_epoch%d.html", epoch);
                        char *item_tpl = item_tpl_path ? read_file_to_string(item_tpl_path) : NULL;
                        free(item_tpl_path);

                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!item_tpl || !page_tpl) {
                            free(item_tpl); free(page_tpl);
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *items_html = strdup("");
                            for (size_t i = 0; items_html && i < gallery.url_count; i++) {
                                char *full  = image_url_variant(gallery.urls[i], "_full");
                                char *small = image_url_variant(gallery.urls[i], "_small");
                                char *item  = (full && small) ? render_template(item_tpl, full, small) : NULL;
                                if (item) items_html = str_append(items_html, item);
                                free(item); free(full); free(small);
                            }
                            free(item_tpl);

                            char *page_html = items_html ? render_template(page_tpl, items_html) : NULL;
                            free(items_html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    } else {
                        // Epochs -1/0/1/2: paginated gallery
                        int count = (int)gallery.url_count;
                        int cur = (img_index >= 0 && img_index < count) ? img_index : 0;
                        int prev_idx = (cur - 1 + count) % count;
                        int next_idx = (cur + 1) % count;

                        char *back_tpl = generate_url_theme("elements/gallery/gallery-page-back_epoch%d.html", epoch);
                        char *back_str = back_tpl ? read_file_to_string(back_tpl) : NULL;
                        free(back_tpl);

                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!back_str || !page_tpl) {
                            free(back_str); free(page_tpl);
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *html = render_template(back_str);
                            free(back_str);

                            if (epoch <= 0) {
                                char *entry_tpl_path = generate_url_theme("elements/gallery/gallery-page-image-entry_epoch%d.html", epoch);
                                char *entry_tpl = entry_tpl_path ? read_file_to_string(entry_tpl_path) : NULL;
                                free(entry_tpl_path);
                                if (entry_tpl) {
                                    for (int i = 0; i < count && html; i++) {
                                        char *line = render_template(entry_tpl, i + 1);
                                        if (line) html = str_append(html, line);
                                        free(line);
                                    }
                                    free(entry_tpl);
                                }
                            } else {
                                char *main_tpl_path  = generate_url_theme("elements/gallery/gallery-page-main_epoch%d.html", epoch);
                                char *main_tpl       = main_tpl_path ? read_file_to_string(main_tpl_path) : NULL;
                                char *strip_s_path   = generate_url_theme("elements/gallery/gallery-page-thumbstrip-start_epoch%d.html", epoch);
                                char *strip_s_tpl    = strip_s_path ? read_file_to_string(strip_s_path) : NULL;
                                char *strip_e_path   = generate_url_theme("elements/gallery/gallery-page-thumbstrip-end_epoch%d.html", epoch);
                                char *strip_e_tpl    = strip_e_path ? read_file_to_string(strip_e_path) : NULL;
                                char *thumb_tpl_path = generate_url_theme("elements/gallery/gallery-page-thumb_epoch%d.html", epoch);
                                char *thumb_tpl      = thumb_tpl_path ? read_file_to_string(thumb_tpl_path) : NULL;
                                free(main_tpl_path); free(strip_s_path); free(strip_e_path); free(thumb_tpl_path);

                                if (main_tpl && strip_s_tpl && strip_e_tpl && thumb_tpl) {
                                    char *main_url = (epoch == 1)
                                        ? image_url_variant(gallery.urls[cur], "_medium")
                                        : image_url_variant(gallery.urls[cur], "_half");
                                    if (epoch == 1 && main_url) {
                                        char *dot = strrchr(main_url, '.'); if (dot) strcpy(dot, ".gif");
                                    }
                                    char *main_block = render_template(main_tpl,
                                        gallery_id, prev_idx, main_url ? main_url : "",
                                        gallery_id, next_idx);
                                    free(main_url);
                                    if (main_block) { html = str_append(html, main_block); free(main_block); }

                                    char *strip_s = render_template(strip_s_tpl);
                                    if (strip_s) { html = str_append(html, strip_s); free(strip_s); }

                                    for (int i = 0; i < count && html; i++) {
                                        char *t = (epoch == 1)
                                            ? image_url_variant(gallery.urls[i], "_micro")
                                            : image_url_variant(gallery.urls[i], "_small");
                                        if (epoch == 1 && t) {
                                            char *dot = strrchr(t, '.'); if (dot) strcpy(dot, ".gif");
                                        }
                                        int border = (i == cur) ? 3 : 1;
                                        char *thumb = render_template(thumb_tpl, gallery_id, i, t ? t : "", border);
                                        free(t);
                                        if (thumb) { html = str_append(html, thumb); free(thumb); }
                                    }

                                    char *strip_e = render_template(strip_e_tpl);
                                    if (strip_e) { html = str_append(html, strip_e); free(strip_e); }
                                }
                                free(main_tpl); free(strip_s_tpl); free(strip_e_tpl); free(thumb_tpl);
                            }

                            char *page_html = html ? render_template(page_tpl, html) : NULL;
                            free(html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    }
                    cms_media_gallery_free(&gallery);
                }

            } else if (strcmp(decoded_url, "/blog") == 0) {
                int epoch = resolve_epoch(&req);

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);
                char *cat_menu = category_menu_render(cats, cat_count, NULL, epoch);
                cms_categories_free(cats, cat_count);

                char *content  = blog_list(epoch, content_lang);
                char *body     = buildBlogListWebSiteAtUrl(epoch, "Boat Rudder - Blog", content, "/blog", cat_menu);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/blog/category/", 15) == 0 && decoded_url[15] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *cat_slug = decoded_url + 15;

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);

                char *cat_id   = NULL;
                char *cat_name = NULL;
                for (size_t i = 0; i < cat_count; i++) {
                    char *slug = slugify(cats[i].name);
                    if (slug && strcmp(slug, cat_slug) == 0) {
                        cat_id   = strdup(cats[i].id);
                        cat_name = strdup(cats[i].name);
                    }
                    free(slug);
                    if (cat_id) break;
                }

                if (!cat_id) {
                    cms_categories_free(cats, cat_count);
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    char *cat_menu = category_menu_render(cats, cat_count, cat_slug, epoch);
                    cms_categories_free(cats, cat_count);

                    char *content = blog_list_category(epoch, content_lang, cat_id);
                    free(cat_id);

                    char page_title[256];
                    snprintf(page_title, sizeof(page_title), "Blog - %s", cat_name ? cat_name : cat_slug);
                    free(cat_name);

                    char *body     = buildBlogListWebSiteAtUrl(epoch, page_title, content, "/blog", cat_menu);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strncmp(decoded_url, "/blog/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);
                char *cat_menu = category_menu_render(cats, cat_count, NULL, epoch);
                cms_categories_free(cats, cat_count);

                serve_cms_entry(ctx, decoded_url + 6, "blog", content_lang, req.method, epoch, cat_menu);

            } else {
                const char *ims = get_header_value(&req, "If-Modified-Since");
                int status = serve_static_file(ctx, root_directory, decoded_url, ims);
                if (status != 0) {
                    const char *status_line = status == 404 ? "404 Not Found"
                                             : status == 403 ? "403 Forbidden"
                                             : "500 Internal Server Error";
                    send_error_response(ctx, status, status_line, resolve_epoch(&req));
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/login") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *content  = login(epoch, NULL);
                char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (!mongodb_manager_is_ready()) {
                send_error_response(ctx, 503, "503 Service Unavailable", epoch);

            } else {
                char email[256];
                char password[256];
                parse_urlencoded_field(req.body, req.body_length, "user", email, sizeof(email));
                parse_urlencoded_field(req.body, req.body_length, "password", password, sizeof(password));

                char *user_id = auth_login_user(email, password);
                char *response = NULL;

                if (user_id) {
                    char *token = generate_session_token();
                    if (token && create_session(user_id, token, session_ttl_seconds) == 0) {
                        char cookie_header[256];
                        build_session_cookie_header(token, session_ttl_seconds, cookie_header, sizeof(cookie_header));
                        response = build_redirect_response("/dashboard", cookie_header, epoch);
                    }
                    free(token);
                    free(user_id);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = login(epoch, "Invalid email or password.");
                    char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                    response       = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/categories/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_create_category(langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_update_category(id, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_category(id);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/menu/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_create_menu_item(link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_update_menu_item(id, link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_menu_item(id);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/entries/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    const char *type = strcmp(role, "author") == 0 ? "blog" : "page";
                    char *new_id = cms_create_entry(user_id, type);
                    char *response;
                    if (new_id) {
                        char location[64];
                        snprintf(location, sizeof(location), "/dashboard/entries/%s/edit", new_id);
                        response = build_redirect_response(location, "", epoch);
                    } else {
                        response = build_redirect_response("/dashboard", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                    free(new_id);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/entries", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_entry(id);

                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/meta", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char link[256];
                        char type[16];
                        parse_urlencoded_field(req.body, req.body_length, "link", link, sizeof(link));
                        if (strcmp(role, "author") == 0) {
                            strcpy(type, "blog");
                        } else if (!parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type))) {
                            strcpy(type, "page");
                        }

                        char enabled_str[8];
                        bool enabled = parse_urlencoded_field(req.body, req.body_length, "enabled",
                                                               enabled_str, sizeof(enabled_str)) != 0;

                        char *category_ids[CATEGORY_LIST_LIMIT];
                        size_t category_count = parse_urlencoded_multi(req.body, req.body_length, "categories",
                                                                         category_ids, CATEGORY_LIST_LIMIT);

                        if (cms_update_entry_meta(id, link, type, enabled, category_ids, category_count) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < category_count; i++) free(category_ids[i]);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/header", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char image_url[1024];
                        char date[16];
                        parse_urlencoded_field(req.body, req.body_length, "image_url", image_url, sizeof(image_url));
                        parse_urlencoded_field(req.body, req.body_length, "date", date, sizeof(date));

                        char **title_values   = calloc(lang_count, sizeof(char *));
                        char **summary_values = calloc(lang_count, sizeof(char *));
                        char **author_values  = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) {
                            char field[40], value[1024];

                            snprintf(field, sizeof(field), "title_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            title_values[i] = strdup(value);

                            snprintf(field, sizeof(field), "summary_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            summary_values[i] = strdup(value);

                            snprintf(field, sizeof(field), "author_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            author_values[i] = strdup(value);
                        }

                        if (cms_update_entry_header(id, image_url, date, langs, lang_count,
                                                     title_values, summary_values, author_values) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < lang_count; i++) {
                            free(title_values[i]);
                            free(summary_values[i]);
                            free(author_values[i]);
                        }
                        free(title_values);
                        free(summary_values);
                        free(author_values);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/content", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char count_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "content_count", count_str, sizeof(count_str));
                        int block_count = atoi(count_str);
                        if (block_count < 0) block_count = 0;
                        if (block_count > 200) block_count = 200;

                        CmsContentBlockEdit *blocks = calloc((size_t)block_count, sizeof(CmsContentBlockEdit));
                        for (int i = 0; i < block_count; i++) {
                            char field[48], value[8192];

                            snprintf(field, sizeof(field), "content_%d_id", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].id = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_type", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].type = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_order", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].order = atoi(value);

                            snprintf(field, sizeof(field), "content_%d_extra_data", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].extra_data = strdup(value);

                            blocks[i].text_values = calloc(lang_count, sizeof(char *));
                            for (size_t j = 0; j < lang_count; j++) {
                                snprintf(field, sizeof(field), "content_%d_text_%s", i, langs[j].code);
                                parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                                blocks[i].text_values[j] = strdup(value);
                            }
                        }

                        if (cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count) == 0) {
                            for (int gi = 0; gi < block_count; gi++) {
                                if (blocks[gi].type && strcmp(blocks[gi].type, "gallery") == 0 &&
                                    blocks[gi].text_values && blocks[gi].text_values[0] &&
                                    blocks[gi].text_values[0][0]) {
                                    char gal_id[25];
                                    const char *existing = blocks[gi].extra_data;
                                    if (cms_upsert_media_gallery(
                                            (existing && strlen(existing) == 24) ? existing : NULL,
                                            id, blocks[gi].text_values[0], gal_id) == 0) {
                                        free(blocks[gi].extra_data);
                                        blocks[gi].extra_data = strdup(gal_id);
                                        cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count);
                                    }
                                }
                            }
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (int i = 0; i < block_count; i++) {
                            free(blocks[i].id);
                            free(blocks[i].type);
                            free(blocks[i].extra_data);
                            for (size_t j = 0; j < lang_count; j++) free(blocks[i].text_values[j]);
                            free(blocks[i].text_values);
                        }
                        free(blocks);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_block_delete_route(decoded_url, id, sizeof(id), block_id, sizeof(block_id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else if (cms_remove_entry_content_block(id, block_id) == 0) {
                        response = build_json_response("{\"ok\":true}");
                    } else {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"remove failed\"}", "400 Bad Request");
                    }
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/blocks", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response = NULL;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else {
                        char type[16];
                        char order_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type));
                        parse_urlencoded_field(req.body, req.body_length, "order", order_str, sizeof(order_str));
                        int order = atoi(order_str);

                        char new_block_id[32];
                        if (cms_add_entry_content_block(id, type, order, new_block_id, sizeof(new_block_id)) == 0) {
                            CmsContentBlockEdit block = {0};
                            block.id = new_block_id;
                            block.type = type;
                            block.order = order;
                            block.extra_data = (char *)"";
                            block.text_values = calloc(lang_count, sizeof(char *));
                            for (size_t i = 0; i < lang_count; i++) block.text_values[i] = (char *)"";

                            char *html = entry_editor_render_block(&block, langs, lang_count, epoch);
                            free(block.text_values);

                            char *html_json = html ? json_escape_alloc(html) : NULL;
                            free(html);

                            if (html_json) {
                                char *json_body = render_template(
                                    "{\"ok\":true,\"block_id\":\"%s\",\"html\":\"%s\"}", new_block_id, html_json);
                                response = json_body ? build_json_response(json_body) : NULL;
                                free(json_body);
                            }
                            free(html_json);
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"create failed\"}", "400 Bad Request");
                        }
                    }

                    if (!response)
                        response = build_json_response_status("{\"ok\":false,\"error\":\"internal error\"}", "500 Internal Server Error");
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/languages/add") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char code[16];
                    parse_urlencoded_field(req.body, req.body_length, "code", code, sizeof(code));

                    if (cms_add_language(code) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "Idioma desconocido o ya agregado.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/default", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_set_default_language(id);

                    char *response = build_redirect_response("/dashboard/languages", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/remove", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (cms_remove_language(id) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "No se puede eliminar el idioma predeterminado o el ultimo idioma.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/users/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char email[256], password[256], role[USER_ROLE_BUF_SIZE];
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));

                    char *response;
                    if (cms_create_user(email, password, role) == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        char *content  = users_admin_form(epoch, "", email, role,
                                              "No se pudo crear el usuario. Verifica el email y la contrasena.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char email[256], password[256], role[USER_ROLE_BUF_SIZE];
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));

                    char *response;
                    if (strcmp(id, user_id) == 0 && strcmp(role, "admin") != 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_form(epoch, id, email, role,
                                              "No puedes quitarte el unico rol de administrador.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_update_user(id, email, role, password) == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        char *content  = users_admin_form(epoch, id, email, role,
                                              "No se pudo actualizar el usuario. Verifica el email.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char target_role[USER_ROLE_BUF_SIZE];
                    char *response;

                    if (strcmp(id, user_id) == 0) {
                        char *content  = users_admin_list(epoch, "No puedes eliminar tu propia cuenta.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_get_user_role(id, target_role, sizeof(target_role)) == 0 &&
                               strcmp(target_role, "admin") == 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_list(epoch, "No se puede eliminar el ultimo administrador.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else {
                        cms_delete_user(id);
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        // ---- Media admin POST routes ----

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char name[256];
                    parse_urlencoded_field(req.body, req.body_length, "newpath", name, sizeof(name));

                    size_t nlen = strlen(name);
                    int valid = nlen >= 3 && nlen <= 60;
                    for (size_t i = 0; valid && i < nlen; i++) {
                        char c = name[i];
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_'))
                            valid = 0;
                    }
                    if (strstr(name, "..")) valid = 0;

                    if (!valid) {
                        send_simple(ctx, "400 Bad Request", "Invalid directory name (3-60 chars, a-z 0-9 _ -)");
                    } else {
                        char username[64] = {0};
                        cms_get_username_by_id(user_id, username, sizeof(username));

                        char dirpath[512];
                        snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, name);

                        struct stat st;
                        if (stat(dirpath, &st) == -1) {
                            char parent_path[512];
                            snprintf(parent_path, sizeof(parent_path), "./html/content/posts/%s", username);
                            mkdir(parent_path, 0775);
                            mkdir(dirpath, 0775);
                        }

                        char new_id[25];
                        if (cms_create_media_directory(name, "posts", user_id, new_id) == 0) {
                            CmsMediaDirectory dir;
                            if (cms_get_media_directory_by_id(new_id, &dir)) {
                                strncpy(dir.author_name, username, sizeof(dir.author_name) - 1);
                                char *html = media_admin_render_directory_item(&dir, epoch);
                                if (html) {
                                    char hdr[128];
                                    snprintf(hdr, sizeof(hdr),
                                             "Content-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\n",
                                             strlen(html));
                                    char resp[256];
                                    snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\n%s\r\n", hdr);
                                    connection_write(ctx, resp, strlen(resp));
                                    connection_write(ctx, html, strlen(html));
                                    free(html);
                                } else {
                                    send_simple(ctx, "500 Internal Server Error", "Render error");
                                }
                            } else {
                                send_simple(ctx, "500 Internal Server Error", "DB lookup error");
                            }
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "DB insert error");
                        }
                        connection_close(ctx);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/rename") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char dir_id[32], oldname[256], newname[256];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));
                    parse_urlencoded_field(req.body, req.body_length, "oldname", oldname, sizeof(oldname));
                    parse_urlencoded_field(req.body, req.body_length, "newname", newname, sizeof(newname));

                    char username[64] = {0};
                    cms_get_username_by_id(user_id, username, sizeof(username));

                    char old_path[512], new_path[512];
                    snprintf(old_path, sizeof(old_path), "./html/content/posts/%s/%s", username, oldname);
                    snprintf(new_path, sizeof(new_path), "./html/content/posts/%s/%s", username, newname);

                    if (rename(old_path, new_path) == 0 || errno == ENOENT) {
                        cms_rename_media_directory(dir_id, newname);

                        CmsMediaDirectory dir;
                        if (cms_get_media_directory_by_id(dir_id, &dir)) {
                            strncpy(dir.author_name, username, sizeof(dir.author_name) - 1);
                            char *html = media_admin_render_directory_item(&dir, epoch);
                            if (html) {
                                char hdr[128];
                                snprintf(hdr, sizeof(hdr),
                                         "Content-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\n",
                                         strlen(html));
                                char resp[256];
                                snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\n%s\r\n", hdr);
                                connection_write(ctx, resp, strlen(resp));
                                connection_write(ctx, html, strlen(html));
                                free(html);
                            }
                        }
                    } else {
                        send_simple(ctx, "500 Internal Server Error", "Could not rename directory");
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char dir_id[32], dirname[256];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));
                    parse_urlencoded_field(req.body, req.body_length, "dirname", dirname, sizeof(dirname));

                    char username[64] = {0};
                    cms_get_username_by_id(user_id, username, sizeof(username));

                    char dirpath[512];
                    snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, dirname);

                    rmdir(dirpath);
                    cms_delete_media_directory(dir_id);
                    send_simple(ctx, "200 OK", "Deleted");
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    const char *ct = get_header_value(&req, "Content-Type");
                    MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                    if (!mp) {
                        send_simple(ctx, "400 Bad Request", "Invalid multipart body");
                    } else {
                        const MultipartPart *dir_part = multipart_find(mp, "media-directory-selected");
                        const MultipartPart *file_part = multipart_find(mp, "file");

                        if (!dir_part || !file_part || file_part->filename[0] == '\0') {
                            send_simple(ctx, "400 Bad Request", "Missing directory or file");
                            free_multipart(mp);
                        } else {
                            char dir_id_buf[32] = {0};
                            size_t dlen = dir_part->data_len < sizeof(dir_id_buf) - 1 ? dir_part->data_len : sizeof(dir_id_buf) - 1;
                            memcpy(dir_id_buf, dir_part->data, dlen);

                            CmsMediaDirectory dir;
                            if (!cms_get_media_directory_by_id(dir_id_buf, &dir)) {
                                send_simple(ctx, "404 Not Found", "Directory not found");
                            } else {
                                char username[64] = {0};
                                cms_get_username_by_id(user_id, username, sizeof(username));

                                // Sanitize filename
                                char sanitized[256] = {0};
                                int j = 0;
                                for (int k = 0; file_part->filename[k] && j < (int)sizeof(sanitized) - 1; k++) {
                                    char c = file_part->filename[k];
                                    if (c == ' ') sanitized[j++] = '-';
                                    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                             (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
                                        sanitized[j++] = c;
                                }
                                sanitized[j] = '\0';

                                // Create dir on disk
                                char dirpath[512];
                                snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, dir.name);
                                mkdir(dirpath, 0775);

                                // Write file
                                char filepath[1024];
                                snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, sanitized);
                                FILE *f = fopen(filepath, "wb");
                                int write_ok = 0;
                                if (f) {
                                    write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                                    fclose(f);
                                }

                                if (!write_ok) {
                                    send_simple(ctx, "500 Internal Server Error", "Could not write file");
                                } else {
                                    LOG_INFO("Media file saved: %s (%zu bytes)", filepath, file_part->data_len);

                                    // Run image optimizer
                                    char command[4096];
                                    snprintf(command, sizeof(command),
                                             "./scripts/image-optimizer.sh '%s' '%s' '%s'",
                                             dirpath, dirpath, filepath);
                                    LOG_INFO("Running optimizer: %s", command);

                                    char optimized_path[1024] = {0};
                                    FILE *pipe = popen(command, "r");
                                    if (pipe) {
                                        if (fgets(optimized_path, sizeof(optimized_path), pipe))
                                            optimized_path[strcspn(optimized_path, "\n")] = '\0';
                                        pclose(pipe);
                                    }

                                    if (optimized_path[0] == '\0')
                                        strncpy(optimized_path, filepath, sizeof(optimized_path) - 1);

                                    // Build filename for DB: strip path, remove _full suffix
                                    char *basename = strrchr(optimized_path, '/');
                                    basename = basename ? basename + 1 : optimized_path;
                                    char db_name[512];
                                    strncpy(db_name, basename, sizeof(db_name) - 1);
                                    db_name[sizeof(db_name) - 1] = '\0';
                                    char *dot = strrchr(db_name, '.');
                                    if (dot) {
                                        char *full_tag = strstr(db_name, "_full");
                                        if (full_tag && full_tag < dot)
                                            memmove(full_tag, full_tag + 5, strlen(full_tag + 5) + 1);
                                    }

                                    char media_id[25];
                                    cms_insert_media(db_name, user_id, dir_id_buf, media_id);

                                    char json_response[2048];
                                    snprintf(json_response, sizeof(json_response),
                                             "{\"ok\":true,\"filename\":\"%s\"}", optimized_path);
                                    char *response = build_json_response(json_response);
                                    connection_write(ctx, response, strlen(response));
                                    free(response);
                                }
                            }
                            free_multipart(mp);
                            connection_close(ctx);
                        }
                    }
                }
            }

        } else if (strcmp(req.method, "OPTIONS") == 0) {
            const char *opts =
                "HTTP/1.1 204 No Content\r\n"
                "Allow: GET, HEAD, OPTIONS, POST\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            connection_write(ctx, opts, strlen(opts));

        } else {
            send_error_response(ctx, 405, "405 Method Not Allowed", resolve_epoch(&req));
        }
    }

cleanup:
    free(raw_request);
    if (req.body) { free(req.body); req.body = NULL; }
    connection_close(ctx);
}
