#include "wap_gateway.h"
#include "wbxml.h"
#include "../utils/config_loader.h"
#include "../utils/log.h"
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

// ---- Loopback fetch ----------------------------------------------------------------

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

// GET `path` from this same server's plain-HTTP listener. `client_ip` rides
// along as X-Forwarded-For (honored only if 127.0.0.1 is a trusted proxy),
// so analytics and rate limiting see the phone rather than the gateway.
static int fetch_local(const char *path, const char *client_ip, HttpResponse *r) {
    memset(r, 0, sizeof(*r));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = FETCH_TIMEOUT_SEC };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)http_port) };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }

    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: 127.0.0.1\r\n"
                     "User-Agent: BoatRudder-WAP-Gateway\r\n"
                     "Accept: text/vnd.wap.wml, image/vnd.wap.wbmp\r\n"
                     "X-Forwarded-For: %s\r\n"
                     "Connection: close\r\n\r\n", path, client_ip);
    if (n <= 0 || n >= (int)sizeof(req) || send(fd, req, (size_t)n, 0) != n) { close(fd); return -1; }

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

// The WSP Reply (everything after the framing) for a GET of `uri`.
static void handle_get(Bytes *out, const char *uri, const char *client_ip, size_t page_bytes,
                       const char *label) {
    int page = wbxml_page_param(uri);
    char page0[1100];
    wbxml_set_page_param(uri, 0, page0, sizeof(page0));

    char path[1100];
    local_path(page0, path, sizeof(path));

    HttpResponse resp = {0};
    int fetched = -1;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        char req_path[1200];
        // wml_pages=all: the whole entry in one deck - this gateway does the
        // paginating, into single-packet pages (see wbxml.h).
        snprintf(req_path, sizeof(req_path), "%s%spreview_epoch=-1&wml_pages=all",
                 path, strchr(path, '?') ? "&" : "?");
        free(resp.raw);
        fetched = fetch_local(req_path, client_ip, &resp);
        if (fetched != 0 || resp.status < 300 || resp.status >= 400 || !resp.location[0]) break;
        local_path(resp.location, path, sizeof(path));
    }

    if (fetched != 0) {
        LOG_WARN("WAP %s %s: site unreachable over loopback", label, uri);
        message_reply(out, "Error", "The site is not responding. Try again later.");
        return;
    }

    if (resp.status == 200 && !strncasecmp(resp.content_type, "text/vnd.wap.wml", 16)) {
        int utf8 = contains_ci(resp.content_type, "utf-8");
        WbxmlBuf page_buf;
        if (wbxml_compile_page((const char *)resp.body, resp.body_len, !utf8, uri, page,
                               page_bytes, &page_buf) == 0) {
            wsp_reply(out, CT_WMLC, page_buf.data, page_buf.len);
            free(page_buf.data);
        } else {
            message_reply(out, "Error", "This page could not be converted.");
        }
    } else if (resp.status == 200 && !strncasecmp(resp.content_type, "image/vnd.wap.wbmp", 18)) {
        wsp_reply(out, CT_WBMP, resp.body, resp.body_len);
    } else if (resp.status == 404) {
        message_reply(out, "Not found", "This page does not exist.");
    } else {
        message_reply(out, "Unavailable", "This content is not available on WAP.");
    }
    LOG_INFO("WAP %s %s -> %d (%zu bytes)", label, uri, resp.status, out->len);
    free(resp.raw);
}

// One datagram in, one datagram out. Anything that isn't a GET-family
// request in the expected framing is ignored (the client simply retries or
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
    if (type < 0x40 || type > 0x44) return;   // Get, Options, Head, Delete, Trace

    size_t pos = wsp_at + 1;
    unsigned long uri_len;
    if (read_uintvar(d, n, &pos, &uri_len) != 0 || uri_len == 0 || uri_len > n - pos || uri_len > 1000)
        return;
    char uri[1024];
    memcpy(uri, d + pos, uri_len);
    uri[uri_len] = '\0';

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
    handle_get(&body, uri, client_ip, WAP_PAGE_BYTES, gs->label);
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
