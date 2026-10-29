#include "wap_gateway.h"
#include "wbxml.h"
#include "../utils/config_loader.h"
#include "../utils/http_utils.h"
#include "../utils/log.h"
#include "../web_server/proxy_protocol.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOCKETS       16
#define MAX_DATAGRAM      4096
#define MAX_FETCH_BYTES   (1024 * 1024)
#define MAX_REDIRECTS     3
#define FETCH_TIMEOUT_SEC 10

// Page size: WAP_PAGE_BYTES (wbxml.h), the one limit every epoch -1 client
// gets for now, both framings included.

// WSP well-known content types (WINA registry).
#define CT_WMLC 0x14   // application/vnd.wap.wmlc
#define CT_WBMP 0x21   // image/vnd.wap.wbmp

typedef enum { FRAMING_ROVER, FRAMING_WSP } Framing;

typedef struct {
    int fd;
    Framing framing;
    char label[64];
} GatewaySocket;

static GatewaySocket sockets_[MAX_SOCKETS];
static int socket_count = 0;
static pthread_t gateway_thread;
static atomic_int gateway_running = 0;

// ---- Small byte buffer -----------------------------------------------------------

typedef struct {
    unsigned char *d;
    size_t len, cap;
    int ok;
} Bytes;

static void bytes_put(Bytes *b, const void *p, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (b->len + n + 1 > cap) cap *= 2;
        unsigned char *nd = realloc(b->d, cap);
        if (!nd) { b->ok = 0; return; }
        b->d = nd;
        b->cap = cap;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    b->d[b->len] = 0;
}

static void bytes_byte(Bytes *b, unsigned char c) { bytes_put(b, &c, 1); }

static void bytes_uintvar(Bytes *b, unsigned long v) {
    unsigned char tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    for (v >>= 7; v && n < 5; v >>= 7) tmp[n++] = 0x80 | (v & 0x7F);
    while (n) bytes_byte(b, tmp[--n]);
}

static int read_uintvar(const unsigned char *d, size_t n, size_t *pos, unsigned long *out) {
    unsigned long v = 0;
    for (int k = 0; k < 5; k++) {
        if (*pos >= n) return -1;
        unsigned char c = d[(*pos)++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) { *out = v; return 0; }
    }
    return -1;
}

// ---- Page fetch ----------------------------------------------------------------

typedef struct {
    int status;
    char content_type[128];
    char location[512];
    unsigned char *body;
    size_t body_len;
    unsigned char *raw;   // owns body
} HttpResponse;

static void header_value(const char *headers, const char *name, char *out, size_t out_size) {
    out[0] = '\0';
    size_t nlen = strlen(name);
    for (const char *line = headers; line && *line; ) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if (!strncasecmp(line, name, nlen) && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= out_size) vlen = out_size - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return;
        }
        line = eol + 2;
    }
}

// Where pages are fetched from: wap_upstream ("a.b.c.d:port") when set -
// a front that routes by Host to one of several sites, or any other HTTP
// server - else this same server's plain-HTTP listener (listen_address when
// it is bound to one, else loopback). Returns 0 and fills `addr`, or -1 for
// an unusable wap_upstream.
static int fetch_target(struct sockaddr_in *addr) {
    *addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)http_port) };
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (wap_upstream[0]) {
        char host[64];
        snprintf(host, sizeof(host), "%s", wap_upstream);
        char *colon = strrchr(host, ':');
        if (!colon) return -1;
        *colon = '\0';
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (*end || port < 1 || port > 65535 || inet_pton(AF_INET, host, &addr->sin_addr) != 1)
            return -1;
        addr->sin_port = htons((uint16_t)port);
        return 0;
    }
    if (listen_address[0] && inet_pton(AF_INET, listen_address, &addr->sin_addr) != 1)
        addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (addr->sin_addr.s_addr == htonl(INADDR_ANY)) addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return 0;
}

// Whether the fetch starts with a PROXY v1 header carrying the phone's
// address: on request for wap_upstream (wap_upstream_proxy_protocol - a
// front that requires one, e.g. HAProxy's accept-proxy); for this server's
// own listener when it reads them (proxy_protocol=1) and trusts the
// gateway's address - untrusted, the header would get the connection
// closed, so the request goes without one and is served as a direct
// loopback connection (see connection_thread.c).
static int fetch_sends_proxy_header(const char *server_ip) {
    if (wap_upstream[0]) return wap_upstream_proxy_protocol;
    return proxy_protocol && is_trusted_proxy(server_ip);
}

// A form a phone posts (WSP Post): its body as it came, the type the phone
// gave it, and the phone's own User-Agent when it named one.
typedef struct {
    char content_type[128];
    char user_agent[256];
    const unsigned char *body;
    size_t len;
} WapPost;

// GET `path` of `host` (the host the phone asked for; empty when it named
// none) from fetch_target() - or POST `post` there when it isn't NULL.
// `client_ip` rides along as X-Forwarded-For (honored only if the gateway's
// address is a trusted proxy) and, per fetch_sends_proxy_header(), as a
// PROXY header, so analytics and rate limiting see the phone rather than
// the gateway.
static int fetch_page(const char *host, const char *path, const char *client_ip, const WapPost *post,
                      HttpResponse *r) {
    memset(r, 0, sizeof(*r));
    struct sockaddr_in addr;
    if (fetch_target(&addr) != 0) {
        LOG_ERROR("wap_upstream '%s' is not <IPv4>:<port>", wap_upstream);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = FETCH_TIMEOUT_SEC };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }

    char server_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    inet_ntop(AF_INET, &addr.sin_addr, server_ip, sizeof(server_ip));

    char req[2048];
    size_t proxy_len = 0;
    if (fetch_sends_proxy_header(server_ip))
        proxy_len = proxy_protocol_format_v1(req, sizeof(req), client_ip, 0, server_ip,
                                             ntohs(addr.sin_port));
    char post_headers[512] = "";
    if (post)
        snprintf(post_headers, sizeof(post_headers), "Content-Type: %s\r\nContent-Length: %zu\r\n",
                 post->content_type, post->len);
    int n = snprintf(req + proxy_len, sizeof(req) - proxy_len,
                     "%s %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: text/vnd.wap.wml, image/vnd.wap.wbmp\r\n"
                     "X-Forwarded-For: %s\r\n"
                     "%s"
                     "Connection: close\r\n\r\n", post ? "POST" : "GET", path, host[0] ? host : "127.0.0.1",
                     post && post->user_agent[0] ? post->user_agent : "BoatRudder-WAP-Gateway", client_ip,
                     post_headers);
    if (n <= 0 || (size_t)n >= sizeof(req) - proxy_len) { close(fd); return -1; }
    n += (int)proxy_len;
    if (send(fd, req, (size_t)n, 0) != n ||
        (post && post->len && send(fd, post->body, post->len, 0) != (ssize_t)post->len)) {
        close(fd);
        return -1;
    }

    Bytes in = { .ok = 1 };
    char chunk[8192];
    ssize_t got;
    while ((got = recv(fd, chunk, sizeof(chunk), 0)) > 0 && in.len < MAX_FETCH_BYTES)
        bytes_put(&in, chunk, (size_t)got);
    close(fd);
    if (!in.ok || !in.d) { free(in.d); return -1; }

    char *head_end = strstr((char *)in.d, "\r\n\r\n");
    if (!head_end || sscanf((char *)in.d, "HTTP/%*s %d", &r->status) != 1) { free(in.d); return -1; }

    // The header block alone, every line (the last too) \r\n-terminated.
    size_t head_len = (size_t)(head_end - (char *)in.d) + 2;
    char *head = strndup((char *)in.d, head_len);
    if (head) {
        char *lines = strstr(head, "\r\n");   // skip the status line
        if (lines) {
            header_value(lines + 2, "Content-Type", r->content_type, sizeof(r->content_type));
            header_value(lines + 2, "Location", r->location, sizeof(r->location));
        }
        free(head);
    }
    r->raw = in.d;
    r->body = (unsigned char *)head_end + 4;
    r->body_len = in.len - (size_t)(r->body - in.d);
    return 0;
}

static int contains_ci(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncasecmp(haystack, needle, n)) return 1;
    return 0;
}

// The host (and port) of an absolute `uri`, lowercased: "http://Site.org:80/x"
// -> "site.org:80". Empty for a relative URI, or a host that isn't a plain
// DNS name or IPv4 address with an optional port - it goes out as the
// fetch's Host header, so nothing else may reach it.
static void uri_host(const char *uri, char *out, size_t out_size) {
    out[0] = '\0';
    const char *sep = strstr(uri, "://");
    if (!sep) return;
    const char *h = sep + 3;
    size_t len = strcspn(h, "/?#");
    const char *at = memchr(h, '@', len);          // drop any user:password@
    if (at) { len -= (size_t)(at + 1 - h); h = at + 1; }
    if (len == 0 || len > 253 || len >= out_size) return;

    const char *colon = memchr(h, ':', len);
    size_t name_len = colon ? (size_t)(colon - h) : len;
    if (name_len == 0) return;
    for (size_t i = 0; i < len; i++) {
        char c = h[i];
        int ok = i < name_len ? (isalnum((unsigned char)c) || c == '.' || c == '-')
                              : (i == name_len || isdigit((unsigned char)c));
        if (!ok) return;
    }
    if (colon && len - name_len < 2) return;        // "host:" with no port
    for (size_t i = 0; i < len; i++) out[i] = (char)tolower((unsigned char)h[i]);
    out[len] = '\0';
}

// Path and query of `uri` with its host thrown away ("http://anything/x?y"
// -> "/x?y"; nothing after the host -> "/"), and its fragment dropped.
static void local_path(const char *uri, char *out, size_t out_size) {
    const char *p = uri;
    const char *sep = strstr(uri, "://");
    if (sep) {
        p = strchr(sep + 3, '/');
        if (!p) p = "/";
    } else if (*p != '/') {
        p = "/";
    }
    snprintf(out, out_size, "%s", p);
    char *hash = strchr(out, '#');
    if (hash) *hash = '\0';
    if (!out[0]) snprintf(out, out_size, "/");
}

// ---- Per-IP rate limit ---------------------------------------------------------------

// UDP has no handshake, so a request with a spoofed source address gets its
// (up to ~20x larger) reply sent to that address - left unlimited, the
// gateway is a free traffic amplifier. Answering at most
// wap_gateway_rate_limit requests per minute per source IP caps that to a
// trickle while a real phone (a handful of requests a minute) never notices.
// Only touched by the gateway thread, so no locking.
#define RATE_TABLE_SIZE 256
#define RATE_WINDOW_SEC 60

typedef struct {
    uint32_t ip;          // network order; 0 = free slot
    time_t window_start;
    int count;
    int warned;           // logged the first drop of this window already
} RateEntry;

static RateEntry rate_table[RATE_TABLE_SIZE];

static int rate_allow(uint32_t ip, const char *client_ip) {
    if (wap_gateway_rate_limit <= 0) return 1;

    time_t now = time(NULL);
    RateEntry *e = NULL, *victim = &rate_table[0];
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        if (rate_table[i].ip == ip) { e = &rate_table[i]; break; }
        if (rate_table[i].window_start < victim->window_start) victim = &rate_table[i];
    }
    if (!e) {
        // Free slots have window_start 0, so they're picked first; once
        // full, the IP seen least recently gives up its slot.
        e = victim;
        *e = (RateEntry){ .ip = ip, .window_start = now };
    } else if (now - e->window_start >= RATE_WINDOW_SEC) {
        e->window_start = now;
        e->count = 0;
        e->warned = 0;
    }

    if (++e->count <= wap_gateway_rate_limit) return 1;
    if (!e->warned) {
        e->warned = 1;
        LOG_WARN("WAP gateway: %s over %d requests/min - dropping its requests for the rest of the minute",
                 client_ip, wap_gateway_rate_limit);
    }
    return 0;
}

// ---- Request handling ----------------------------------------------------------------

static void wsp_reply(Bytes *out, int content_type, const unsigned char *body, size_t len) {
    bytes_byte(out, 0x04);                    // Reply
    bytes_byte(out, 0x20);                    // status 200 OK (WSP code space)
    bytes_uintvar(out, 1);                    // headers length
    bytes_byte(out, 0x80 | content_type);     // Content-Type, short integer
    bytes_put(out, body, len);
}

static void message_reply(Bytes *out, const char *title, const char *text) {
    WbxmlBuf card;
    if (wbxml_message_card(title, text, &card) == 0) {
        wsp_reply(out, CT_WMLC, card.data, card.len);
        free(card.data);
    }
}

// The WSP Reply for the response `resp` to `uri` (its page `page`).
static void reply_page(Bytes *out, const HttpResponse *resp, const char *uri, int page, size_t page_bytes) {
    if (resp->status == 200 && !strncasecmp(resp->content_type, "text/vnd.wap.wml", 16)) {
        int utf8 = contains_ci(resp->content_type, "utf-8");
        WbxmlBuf page_buf;
        if (wbxml_compile_page((const char *)resp->body, resp->body_len, !utf8, uri, page,
                               page_bytes, &page_buf) == 0) {
            wsp_reply(out, CT_WMLC, page_buf.data, page_buf.len);
            free(page_buf.data);
        } else {
            message_reply(out, "Error", "This page could not be converted.");
        }
    } else if (resp->status == 200 && !strncasecmp(resp->content_type, "image/vnd.wap.wbmp", 18)) {
        wsp_reply(out, CT_WBMP, resp->body, resp->body_len);
    } else if (resp->status == 404) {
        message_reply(out, "Not found", "This page does not exist.");
    } else {
        message_reply(out, "Unavailable", "This content is not available on WAP.");
    }
}

// Fetches `path` of `host` as epoch -1, in one deck (wml_pages=all: this
// gateway does the paginating, into single-packet pages - see wbxml.h),
// posting `post` when it isn't NULL; follows redirects with GETs, leaving
// `host` and `path` (path_size bytes) at the page finally fetched. Returns
// fetch_page()'s result; on 0 the caller frees resp->raw.
static int fetch_following(char *host, size_t host_size, char *path, size_t path_size, const char *client_ip,
                           const WapPost *post, HttpResponse *resp) {
    int fetched = -1;
    memset(resp, 0, sizeof(*resp));
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        char req_path[1200];
        snprintf(req_path, sizeof(req_path), "%s%spreview_epoch=-1&wml_pages=all",
                 path, strchr(path, '?') ? "&" : "?");
        free(resp->raw);
        fetched = fetch_page(host, req_path, client_ip, hop == 0 ? post : NULL, resp);
        if (fetched != 0 || resp->status < 300 || resp->status >= 400 || !resp->location[0]) break;
        // An absolute Location names the site the rest of the chain is on.
        char next_host[256];
        uri_host(resp->location, next_host, sizeof(next_host));
        if (next_host[0]) snprintf(host, host_size, "%s", next_host);
        local_path(resp->location, path, path_size);
    }
    return fetched;
}

// The WSP Reply (everything after the framing) for a GET of `uri` - or, with
// `post`, for posting it there.
static void handle_request(Bytes *out, const char *uri, const WapPost *post, const char *client_ip,
                           size_t page_bytes, const char *label) {
    int page = post ? 0 : wbxml_page_param(uri);
    char page0[1100];
    wbxml_set_page_param(uri, 0, page0, sizeof(page0));

    char path[1100];
    local_path(page0, path, sizeof(path));
    char host[256];
    uri_host(page0, host, sizeof(host));

    HttpResponse resp;
    if (fetch_following(host, sizeof(host), path, sizeof(path), client_ip, post, &resp) != 0) {
        LOG_WARN("WAP %s %s: site unreachable", label, uri);
        message_reply(out, "Error", "The site is not responding. Try again later.");
        return;
    }
    // After a post, the page it led to (the entry, with a notice) is what the
    // phone shows: its Prev/Next links and relative links are that page's.
    char shown[1400];
    if (post && host[0]) snprintf(shown, sizeof(shown), "http://%s%s", host, path);
    else snprintf(shown, sizeof(shown), "%s", uri);
    reply_page(out, &resp, shown, page, page_bytes);
    LOG_INFO("WAP %s %s%s -> %d (%zu bytes)", label, post ? "POST " : "", uri, resp.status, out->len);
    free(resp.raw);
}

// ---- WSP Post --------------------------------------------------------------------

// Skips one WSP header value at *pos (WAP-230 8.4.1.2: short integer, a
// length-prefixed value, or a NUL-terminated text) - copying it to `text`
// (when not NULL) if it is a text. -1 when malformed.
static int wsp_value(const unsigned char *d, size_t end, size_t *pos, char *text, size_t text_size) {
    if (*pos >= end) return -1;
    unsigned char b = d[*pos];
    if (b >= 0x80) { (*pos)++; return 0; }
    if (b <= 30 || b == 31) {
        unsigned long len = b;
        (*pos)++;
        if (b == 31 && read_uintvar(d, end, pos, &len) != 0) return -1;
        if (len > end - *pos) return -1;
        *pos += len;
        return 0;
    }
    const unsigned char *nul = memchr(d + *pos, 0, end - *pos);
    if (!nul) return -1;
    size_t len = (size_t)(nul - (d + *pos));
    if (text && text_size) snprintf(text, text_size, "%.*s", (int)len, (const char *)d + *pos);
    *pos += len + 1;
    return 0;
}

// The content type and headers of a WSP Post (`d`..`end`): the type as text
// into post->content_type, and a User-Agent header into post->user_agent.
// 0, or -1 when malformed.
static int wsp_post_headers(const unsigned char *d, size_t end, WapPost *post) {
    static const struct { unsigned char code; const char *name; } TYPES[] = {
        { 0x12, "application/x-www-form-urlencoded" }, { 0x24, "multipart/form-data" },
        { 0x03, "text/plain" },
    };
    size_t pos = 0;
    if (end == 0) return -1;
    // Content-Type: a well-known code, a text, or a general form whose media
    // comes first (its parameters are skipped).
    unsigned char b = d[0];
    size_t media = 0, after = 0;
    if (b <= 31) {
        unsigned long len = b;
        pos = 1;
        if (b == 31 && read_uintvar(d, end, &pos, &len) != 0) return -1;
        if (len > end - pos) return -1;
        media = pos;
        after = pos + len;
    } else {
        media = 0;
        if (wsp_value(d, end, &pos, NULL, 0) != 0) return -1;
        after = pos;
    }
    post->content_type[0] = '\0';
    if (d[media] >= 0x80) {
        for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++)
            if ((d[media] & 0x7F) == TYPES[i].code) snprintf(post->content_type, sizeof(post->content_type), "%s", TYPES[i].name);
    } else {
        size_t m = media;
        if (wsp_value(d, after, &m, post->content_type, sizeof(post->content_type)) != 0) return -1;
    }
    if (!post->content_type[0]) snprintf(post->content_type, sizeof(post->content_type), "application/octet-stream");

    // The headers: well-known ones (a code >= 0x80) and application ones (a
    // text name); only User-Agent (0x29) is kept. A shift sequence ends the
    // reading - nothing after it is needed.
    for (pos = after; pos < end; ) {
        b = d[pos];
        if (b >= 0x80) {
            pos++;
            char *dst = (b & 0x7F) == 0x29 ? post->user_agent : NULL;
            if (wsp_value(d, end, &pos, dst, dst ? sizeof(post->user_agent) : 0) != 0) return -1;
        } else if (b >= 32 && b < 127) {
            if (wsp_value(d, end, &pos, NULL, 0) != 0 || wsp_value(d, end, &pos, NULL, 0) != 0) return -1;
        } else {
            break;
        }
    }
    // Printable ASCII only: it goes out as an HTTP header.
    for (char *p = post->user_agent; *p; p++) if (*p < 0x20 || *p > 0x7E) *p = '?';
    return 0;
}

// One datagram in, one datagram out. Anything that isn't a GET-family
// request or a Post in the expected framing is ignored (the client simply retries or
// times out, as with a lost packet).
static void handle_datagram(const GatewaySocket *gs, const unsigned char *d, size_t n,
                            const struct sockaddr_in *from) {
    char client_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &from->sin_addr, client_ip, sizeof(client_ip));

    size_t wsp_at;
    if (gs->framing == FRAMING_ROVER) {
        // WTP Invoke: 4-byte header, PDU type in bits 6-3 of byte 0.
        if (n < 6 || ((d[0] >> 3) & 0x0F) != 1) return;
        wsp_at = 4;
    } else {
        // Connectionless WSP: one TID byte, then the PDU.
        if (n < 3) return;
        wsp_at = 1;
    }

    unsigned char type = d[wsp_at];
    // Get, Options, Head, Delete, Trace - or Post (0x60), a form.
    if ((type < 0x40 || type > 0x44) && type != 0x60) return;

    size_t pos = wsp_at + 1;
    unsigned long uri_len, headers_len = 0;
    if (read_uintvar(d, n, &pos, &uri_len) != 0) return;
    if (type == 0x60 && read_uintvar(d, n, &pos, &headers_len) != 0) return;
    if (uri_len == 0 || uri_len > n - pos || uri_len > 1000) return;
    char uri[1024];
    memcpy(uri, d + pos, uri_len);
    uri[uri_len] = '\0';
    pos += uri_len;

    // A Post: its content type and headers, then the body - all in this one
    // datagram (a form too big for one isn't put back together here).
    WapPost post;
    memset(&post, 0, sizeof(post));
    if (type == 0x60) {
        if (headers_len > n - pos || wsp_post_headers(d + pos, headers_len, &post) != 0) {
            LOG_WARN("WAP %s: malformed Post from %s", gs->label, client_ip);
            return;
        }
        post.body = d + pos + headers_len;
        post.len = n - pos - headers_len;
    }

    if (!rate_allow(from->sin_addr.s_addr, client_ip)) return;

    Bytes reply = { .ok = 1 };
    if (gs->framing == FRAMING_ROVER) {
        // Rover only accepts a Result that mirrors the Invoke's first four
        // bytes (TID, the trailing byte, even the rid flag), with just the
        // PDU type switched to Result (2) - see neomar-wap-proxy's
        // docs/PROTOCOL.md §2.2.
        bytes_byte(&reply, (unsigned char)((2 << 3) | (d[0] & 0x07)));
        bytes_put(&reply, d + 1, 3);
    } else {
        bytes_byte(&reply, d[0]);              // echo the TID
    }

    Bytes body = { .ok = 1 };
    handle_request(&body, uri, type == 0x60 ? &post : NULL, client_ip, WAP_PAGE_BYTES, gs->label);
    if (body.ok && body.len) {
        bytes_put(&reply, body.d, body.len);
        if (reply.ok)
            sendto(gs->fd, reply.d, reply.len, 0, (const struct sockaddr *)from, sizeof(*from));
    }
    free(body.d);
    free(reply.d);
}

static void *gateway_loop(void *arg) {
    (void)arg;
    struct pollfd fds[MAX_SOCKETS];
    for (int i = 0; i < socket_count; i++) fds[i] = (struct pollfd){ .fd = sockets_[i].fd, .events = POLLIN };

    unsigned char buf[MAX_DATAGRAM];
    while (atomic_load(&gateway_running)) {
        int ready = poll(fds, (nfds_t)socket_count, 500);
        if (ready <= 0) continue;
        for (int i = 0; i < socket_count; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t got = recvfrom(fds[i].fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (got > 0) handle_datagram(&sockets_[i], buf, (size_t)got, &from);
        }
    }
    return NULL;
}

// ---- Setup -------------------------------------------------------------------------

static void open_socket(const char *ip, int port, Framing framing) {
    if (socket_count >= MAX_SOCKETS || port <= 0) return;

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        LOG_WARN("WAP gateway: invalid address '%s'", ip);
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("WAP gateway: cannot bind %s:%d (%s)%s", ip, port, strerror(errno),
                 errno == EADDRNOTAVAIL ? " - not an address of this host (check wap_gateway_ips)" : "");
        close(fd);
        return;
    }

    GatewaySocket *gs = &sockets_[socket_count++];
    gs->fd = fd;
    gs->framing = framing;
    snprintf(gs->label, sizeof(gs->label), "%s:%d", ip, port);
    LOG_INFO("WAP gateway listening on UDP %s (%s)", gs->label,
             framing == FRAMING_ROVER ? "Palm Rover WTP" : "connectionless WSP");
}

static void open_for_ip(const char *ip) {
    open_socket(ip, wap_gateway_rover_port, FRAMING_ROVER);
    open_socket(ip, wap_gateway_wsp_port, FRAMING_WSP);
}

int wap_gateway_start(void) {
    if (!wap_gateway_enabled) return 0;

    // Each listed address gets its own sockets (rather than one 0.0.0.0
    // bind), so on a multi-homed host replies leave from the very address
    // the client sent to.
    char list[sizeof(wap_gateway_ips)];
    snprintf(list, sizeof(list), "%s", wap_gateway_ips);
    int any_ip = 0;
    char *save = NULL;
    for (char *ip = strtok_r(list, ",", &save); ip; ip = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*ip)) ip++;
        char *end = ip + strlen(ip);
        while (end > ip && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!*ip) continue;
        any_ip = 1;
        open_for_ip(ip);
    }
    if (!any_ip) open_for_ip("0.0.0.0");

    if (socket_count == 0) {
        LOG_ERROR("WAP gateway enabled but no socket could be opened");
        return -1;
    }
    atomic_store(&gateway_running, 1);
    if (pthread_create(&gateway_thread, NULL, gateway_loop, NULL) != 0) {
        atomic_store(&gateway_running, 0);
        wap_gateway_stop();
        return -1;
    }
    return 0;
}

void wap_gateway_stop(void) {
    if (atomic_exchange(&gateway_running, 0)) pthread_join(gateway_thread, NULL);
    for (int i = 0; i < socket_count; i++) close(sockets_[i].fd);
    socket_count = 0;
}
