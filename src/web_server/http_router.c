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
#include "../db/cms_languages.h"
#include "../db/mongodb_manager.h"
#include "../db/session_manager.h"
#include "../html_builder/orchestrator.h"
#include "../modules/blog_list/blog_list.h"
#include "../modules/categories_admin/categories_admin.h"
#include "../modules/dashboard/dashboard.h"
#include "../modules/entry_page/entry_page.h"
#include "../modules/error/error.h"
#include "../modules/languages_admin/languages_admin.h"
#include "../modules/login/login.h"
#include "../utils/build_epoch_response.h"
#include "../utils/config_loader.h"
#include "../utils/detect_epoch.h"
#include "../utils/log.h"
#include "../utils/http_utils.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
// expected_type.
static void serve_cms_entry(void *ctx, const char *link, const char *expected_type,
                             const char *lang, const char *method, int epoch) {
    CmsEntry entry;
    if (mongodb_manager_is_ready() && cms_get_entry_by_link(link, lang, &entry)) {
        if (strcmp(entry.type, expected_type) != 0) {
            cms_entry_free(&entry);
            send_error_response(ctx, 404, "404 Not Found", epoch);
            return;
        }

        char *title   = strdup(entry.header_title);
        char *content = entry_page(&entry, epoch);
        cms_entry_free(&entry);

        char *body = NULL;
        if (content && title) {
            body = buildPageWebSite(epoch, title, content); // frees content
        } else {
            free(content);
        }
        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
        free(title);
        free(body);
        send_or_error(ctx, response, method, epoch);
    } else {
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
            char raw[256];
            size_t val_len = (size_t)(field_end - (eq + 1));
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            char decoded[256];
            url_decode(decoded, raw);
            strncpy(out, decoded, out_size - 1);
            out[out_size - 1] = '\0';
            return 1;
        }

        p = amp ? amp + 1 : end;
    }
    return 0;
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
                        char *content  = dashboard(epoch);
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
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        char *content  = languages_admin(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
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
                serve_cms_entry(ctx, decoded_url + 6, "page", content_lang, req.method, epoch);

            } else if (strcmp(decoded_url, "/blog") == 0) {
                int epoch = resolve_epoch(&req);

                char *content  = blog_list(epoch, content_lang);
                char *body     = buildPageWebSite(epoch, "Boat Rudder - Blog", content);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/blog/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                serve_cms_entry(ctx, decoded_url + 6, "blog", content_lang, req.method, epoch);

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
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    cms_delete_category(id);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/languages/add") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
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
