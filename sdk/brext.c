#define _GNU_SOURCE

#include "brext.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_HEAD_BYTES  16384
#define IO_TIMEOUT_SECS 15

static volatile sig_atomic_t g_stop;

// ---- Environment ----------------------------------------------------------------

static const char *env_or_empty(const char *key) {
    const char *v = getenv(key);
    return v ? v : "";
}

const char *brext_name(void)        { return env_or_empty("BR_EXTENSION"); }
const char *brext_dir(void)         { return env_or_empty("BR_EXTENSION_DIR"); }
const char *brext_mongodb_uri(void) { return env_or_empty("BR_MONGODB_URI"); }
const char *brext_mongodb_db(void)  { return env_or_empty("BR_MONGODB_DB"); }
int         brext_log_level(void)   { const char *v = getenv("BR_LOG_LEVEL"); return v ? atoi(v) : 3; }

void brext_log(int level, const char *fmt, ...) {
    if (level > brext_log_level()) return;
    static const char *const names[] = { "", "ERROR", "WARN", "INFO", "DEBUG" };
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s %s\n", brext_name()[0] ? brext_name() : "extension",
            names[level >= 1 && level <= 4 ? level : 3], msg);
    fflush(stderr);
}

// ---- Request --------------------------------------------------------------------

const char *brext_header(const BrextRequest *req, const char *name) {
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) return req->headers[i].value;
    return "";
}

const char *brext_user_id(const BrextRequest *req)   { return brext_header(req, "X-BR-User-Id"); }
const char *brext_user_name(const BrextRequest *req) { return brext_header(req, "X-BR-User-Name"); }
const char *brext_role(const BrextRequest *req)      { return brext_header(req, "X-BR-Role"); }
const char *brext_lang(const BrextRequest *req)      { return brext_header(req, "X-BR-Lang"); }
int         brext_epoch(const BrextRequest *req)     { return atoi(brext_header(req, "X-BR-Epoch")); }
const char *brext_mount(const BrextRequest *req)     { return brext_header(req, "X-BR-Mount"); }

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes [s, s + n) ('+' is a space, %XX a byte) into out.
static void url_decode_n(const char *s, size_t n, char *out, size_t out_size) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < out_size; i++) {
        if (s[i] == '+') {
            out[k++] = ' ';
        } else if (s[i] == '%' && i + 2 < n && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out[k++] = (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else {
            out[k++] = s[i];
        }
    }
    out[k] = '\0';
}

static int field(const char *data, size_t len, const char *key, char *out, size_t out_size) {
    const char *p = data, *end = data + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp) amp = end;
        const char *eq = memchr(p, '=', (size_t)(amp - p));
        const char *kend = eq ? eq : amp;
        char k[128];
        url_decode_n(p, (size_t)(kend - p), k, sizeof(k));
        if (strcmp(k, key) == 0) {
            if (eq) url_decode_n(eq + 1, (size_t)(amp - eq - 1), out, out_size);
            else if (out_size) out[0] = '\0';
            return 1;
        }
        p = amp + 1;
    }
    if (out_size) out[0] = '\0';
    return 0;
}

int brext_query(const BrextRequest *req, const char *key, char *out, size_t out_size) {
    return field(req->query, strlen(req->query), key, out, out_size);
}

int brext_form(const BrextRequest *req, const char *key, char *out, size_t out_size) {
    if (strncasecmp(brext_header(req, "Content-Type"), "application/x-www-form-urlencoded", 33) != 0 ||
        !req->body) {
        if (out_size) out[0] = '\0';
        return 0;
    }
    return field(req->body, req->body_len, key, out, out_size);
}

// Length of `s` without trailing slashes, keeping "/" itself.
static size_t trimmed_len(const char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') n--;
    return n;
}

int brext_route(const BrextRequest *req, const char *method, const char *pattern,
                char *segment, size_t segment_size) {
    if (strcmp(req->method, method) != 0 &&
        !(strcmp(method, "GET") == 0 && strcmp(req->method, "HEAD") == 0))
        return 0;

    const char *p = req->path, *q = pattern;
    const char *pend = p + trimmed_len(p), *qend = q + trimmed_len(q);
    while (p < pend && q < qend) {
        if (*q == '*') {
            const char *seg_end = memchr(p, '/', (size_t)(pend - p));
            if (!seg_end) seg_end = pend;
            if (seg_end == p) return 0;
            if (segment) url_decode_n(p, (size_t)(seg_end - p), segment, segment_size);
            p = seg_end;
            q++;
        } else if (*p == *q) {
            p++;
            q++;
        } else {
            return 0;
        }
    }
    return p == pend && q == qend;
}

// ---- Response -------------------------------------------------------------------

void brext_append(BrextResponse *res, const char *text) {
    if (res->failed || !text) return;
    size_t n = strlen(text);
    if (res->len + n + 1 > res->cap) {
        size_t cap = res->cap ? res->cap : 4096;
        while (res->len + n + 1 > cap) cap *= 2;
        char *nb = realloc(res->body, cap);
        if (!nb) { res->failed = 1; return; }
        res->body = nb;
        res->cap = cap;
    }
    memcpy(res->body + res->len, text, n + 1);
    res->len += n;
}

void brext_appendf(BrextResponse *res, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) { res->failed = 1; return; }
    brext_append(res, s);
    free(s);
}

void brext_append_escaped(BrextResponse *res, const char *text) {
    char one[2] = { 0, 0 };
    for (const char *c = text ? text : ""; *c && !res->failed; c++) {
        switch (*c) {
        case '&':  brext_append(res, "&amp;");  break;
        case '<':  brext_append(res, "&lt;");   break;
        case '>':  brext_append(res, "&gt;");   break;
        case '"':  brext_append(res, "&quot;"); break;
        case '\'': brext_append(res, "&#39;");  break;
        default:   one[0] = *c; brext_append(res, one);
        }
    }
}

void brext_status(BrextResponse *res, int status) { res->status = status; }

void brext_html(BrextResponse *res, const char *title) {
    snprintf(res->content_type, sizeof(res->content_type), "text/html; charset=utf-8");
    snprintf(res->title, sizeof(res->title), "%s", title ? title : "");
}

void brext_json(BrextResponse *res) {
    snprintf(res->content_type, sizeof(res->content_type), "application/json");
}

void brext_redirect(BrextResponse *res, const char *path) {
    res->status = 302;
    snprintf(res->location, sizeof(res->location), "%s", path);
}

void brext_not_found(BrextResponse *res) {
    res->status = 404;
    brext_html(res, "Not found");
    res->len = 0;
    if (res->body) res->body[0] = '\0';
    brext_append(res, "<section class=\"boat-rudder-dashboard\"><h1>Not found</h1>"
                      "<p>This page doesn't exist.</p></section>");
}

// ---- Server ---------------------------------------------------------------------

typedef struct {
    int          fd;
    BrextHandler handler;
    void        *userdata;
} Connection;

static int read_some(int fd, char *buf, size_t size) {
    for (;;) {
        ssize_t r = recv(fd, buf, size, 0);
        if (r < 0 && errno == EINTR) continue;
        return (int)r;
    }
}

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// A header value Boat Rudder will read can't carry a line break.
static void clean_header(char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x20) *s = ' ';
}

static void send_response(int fd, BrextResponse *res, int head_only) {
    if (res->failed) {
        static const char oom[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(fd, oom, sizeof(oom) - 1);
        return;
    }
    if (!res->status) res->status = 200;
    if (!res->content_type[0]) snprintf(res->content_type, sizeof(res->content_type), "text/plain");
    clean_header(res->content_type);
    clean_header(res->title);
    clean_header(res->location);

    char head[1024];
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s%s%s%s%s%sConnection: close\r\n\r\n",
        res->status, res->content_type, res->len,
        res->title[0] ? "X-BR-Title: " : "", res->title, res->title[0] ? "\r\n" : "",
        res->location[0] ? "Location: " : "", res->location, res->location[0] ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(head)) return;
    if (write_all(fd, head, (size_t)n) == 0 && !head_only && res->len)
        write_all(fd, res->body, res->len);
}

// Parses one request from fd into req (headers point into `head`). Returns 0,
// or -1 when the connection should just be closed.
static int read_request(int fd, char *head, BrextRequest *req) {
    size_t len = 0;
    char *end = NULL;
    while (!end) {
        if (len >= MAX_HEAD_BYTES) return -1;
        int r = read_some(fd, head + len, MAX_HEAD_BYTES - len);
        if (r <= 0) return -1;
        len += (size_t)r;
        head[len] = '\0';
        end = strstr(head, "\r\n\r\n");
    }
    char *body_start = end + 4;
    size_t already = len - (size_t)(body_start - head);
    *end = '\0';

    char *save = NULL;
    char *line = strtok_r(head, "\r\n", &save);
    char target[3072];
    if (!line || sscanf(line, "%15s %3071s", req->method, target) != 2) return -1;
    char *qm = strchr(target, '?');
    if (qm) *qm = '\0';
    if (strlen(target) >= sizeof(req->path) || (qm && strlen(qm + 1) >= sizeof(req->query))) return -1;
    memcpy(req->path, target, strlen(target) + 1);
    if (!req->path[0]) snprintf(req->path, sizeof(req->path), "/");
    if (qm) memcpy(req->query, qm + 1, strlen(qm + 1) + 1);

    while ((line = strtok_r(NULL, "\r\n", &save)) && req->header_count < BREXT_MAX_HEADERS) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        req->headers[req->header_count].name = line;
        req->headers[req->header_count].value = v;
        req->header_count++;
    }

    long content_length = atol(brext_header(req, "Content-Length"));
    if (content_length < 0 || content_length > BREXT_MAX_BODY) return -1;
    req->body = malloc((size_t)content_length + 1);
    if (!req->body) return -1;
    size_t have = already < (size_t)content_length ? already : (size_t)content_length;
    memcpy(req->body, body_start, have);
    while (have < (size_t)content_length) {
        int r = read_some(fd, req->body + have, (size_t)content_length - have);
        if (r <= 0) return -1;
        have += (size_t)r;
    }
    req->body[have] = '\0';
    req->body_len = have;
    return 0;
}

static void *serve_connection(void *arg) {
    Connection conn = *(Connection *)arg;
    free(arg);

    // SIGTERM is for the accept loop in brext_serve(), not for this thread.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    struct timeval tv = { .tv_sec = IO_TIMEOUT_SECS };
    setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(conn.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char *head = malloc(MAX_HEAD_BYTES + 1);
    BrextRequest *req = calloc(1, sizeof(*req));
    BrextResponse res = { 0 };
    if (head && req && read_request(conn.fd, head, req) == 0) {
        conn.handler(req, &res, conn.userdata);
        send_response(conn.fd, &res, strcmp(req->method, "HEAD") == 0);
    }
    if (req) free(req->body);
    free(req);
    free(head);
    free(res.body);
    close(conn.fd);
    return NULL;
}

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

int brext_serve(BrextHandler handler, void *userdata) {
    const char *path = getenv("BR_SOCKET");
    if (!path || !path[0]) {
        brext_log(1, "BR_SOCKET is not set - this program is started by Boat Rudder");
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_stop };   // no SA_RESTART: accept() returns EINTR
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(path) >= sizeof(addr.sun_path)) {
        brext_log(1, "socket: %s", fd < 0 ? strerror(errno) : "path too long");
        if (fd >= 0) close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || chmod(path, 0600) != 0 ||
        listen(fd, 64) != 0) {
        brext_log(1, "%s: %s", path, strerror(errno));
        close(fd);
        return 1;
    }
    brext_log(3, "listening on %s", path);

    while (!g_stop) {
        int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno != EINTR) brext_log(2, "accept: %s", strerror(errno));
            continue;
        }
        Connection *conn = malloc(sizeof(*conn));
        pthread_t t;
        if (!conn) { close(c); continue; }
        *conn = (Connection){ c, handler, userdata };
        if (pthread_create(&t, NULL, serve_connection, conn) != 0) {
            free(conn);
            close(c);
            continue;
        }
        pthread_detach(t);
    }

    close(fd);
    unlink(path);
    brext_log(3, "stopped");
    return 0;
}
