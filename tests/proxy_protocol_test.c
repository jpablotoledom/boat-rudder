// src/web_server/proxy_protocol.c: v1 and v2 headers parse to the client's
// address, anything else is told apart from them without being consumed,
// and malformed headers are refused rather than half-read.

#include "../src/web_server/proxy_protocol.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *what, int condition) {
    checks++;
    if (!condition) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static ProxyParseStatus parse_text(const char *text, ProxyHeader *h) {
    return proxy_protocol_parse((const unsigned char *)text, strlen(text), h);
}

static const unsigned char SIG[12] = {
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A
};

// A v2 header: signature, version/command, family/protocol, length, payload.
static size_t v2(unsigned char *out, int command, int fam_proto,
                 const unsigned char *payload, size_t payload_len) {
    memcpy(out, SIG, 12);
    out[12] = (unsigned char)(0x20 | command);
    out[13] = (unsigned char)fam_proto;
    out[14] = (unsigned char)(payload_len >> 8);
    out[15] = (unsigned char)(payload_len & 0xFF);
    memcpy(out + 16, payload, payload_len);
    return 16 + payload_len;
}

static void v1_headers(void) {
    ProxyHeader h;

    expect("v1 TCP4", parse_text("PROXY TCP4 203.0.113.7 127.0.0.1 51234 8081\r\nGET / HTTP/1.1\r\n", &h) == PROXY_OK);
    expect("v1 TCP4 source", h.has_source && strcmp(h.source_ip, "203.0.113.7") == 0 && h.source_port == 51234);
    expect("v1 TCP4 length", h.length == strlen("PROXY TCP4 203.0.113.7 127.0.0.1 51234 8081\r\n"));

    expect("v1 TCP6", parse_text("PROXY TCP6 2001:db8::1 ::1 443 8441\r\n", &h) == PROXY_OK);
    expect("v1 TCP6 source", h.has_source && strcmp(h.source_ip, "2001:db8::1") == 0);

    expect("v1 UNKNOWN", parse_text("PROXY UNKNOWN\r\n", &h) == PROXY_OK && !h.has_source);
    expect("v1 UNKNOWN with fields", parse_text("PROXY UNKNOWN ffff::1 ::1 1 2\r\n", &h) == PROXY_OK && !h.has_source);

    expect("v1 split prefix", parse_text("PROX", &h) == PROXY_INCOMPLETE);
    expect("v1 no CRLF yet", parse_text("PROXY TCP4 203.0.113.7 127.0.0", &h) == PROXY_INCOMPLETE);

    expect("v1 family mismatch", parse_text("PROXY TCP4 2001:db8::1 ::1 1 2\r\n", &h) == PROXY_INVALID);
    expect("v1 bad family", parse_text("PROXY UDP4 1.2.3.4 5.6.7.8 1 2\r\n", &h) == PROXY_INVALID);
    expect("v1 bad address", parse_text("PROXY TCP4 1.2.3 5.6.7.8 1 2\r\n", &h) == PROXY_INVALID);
    expect("v1 port out of range", parse_text("PROXY TCP4 1.2.3.4 5.6.7.8 70000 2\r\n", &h) == PROXY_INVALID);
    expect("v1 port leading zero", parse_text("PROXY TCP4 1.2.3.4 5.6.7.8 080 2\r\n", &h) == PROXY_INVALID);
    expect("v1 double space", parse_text("PROXY TCP4  1.2.3.4 5.6.7.8 1 2\r\n", &h) == PROXY_INVALID);
    expect("v1 extra field", parse_text("PROXY TCP4 1.2.3.4 5.6.7.8 1 2 3\r\n", &h) == PROXY_INVALID);
    expect("v1 missing field", parse_text("PROXY TCP4 1.2.3.4 5.6.7.8 1\r\n", &h) == PROXY_INVALID);

    char longline[200];
    memset(longline, 'A', sizeof(longline));
    memcpy(longline, "PROXY ", 6);
    longline[sizeof(longline) - 1] = '\0';
    expect("v1 longer than 107 bytes", parse_text(longline, &h) == PROXY_INVALID);
}

static void v2_headers(void) {
    ProxyHeader h;
    unsigned char buf[PROXY_HEADER_MAX + 64];

    const unsigned char ipv4[12] = {198, 51, 100, 9, 127, 0, 0, 1, 0xC3, 0x50, 0x1F, 0x91};
    size_t n = v2(buf, 1, 0x11, ipv4, sizeof(ipv4));
    memcpy(buf + n, "GET / HTTP/1.1\r\n", 16);
    expect("v2 IPv4", proxy_protocol_parse(buf, n + 16, &h) == PROXY_OK);
    expect("v2 IPv4 source", h.has_source && strcmp(h.source_ip, "198.51.100.9") == 0 && h.source_port == 50000);
    expect("v2 IPv4 length", h.length == 28);

    unsigned char ipv6[36] = {0x20, 0x01, 0x0d, 0xb8};
    ipv6[15] = 0x42;
    ipv6[31] = 1;
    ipv6[32] = 0x01;
    ipv6[33] = 0xBB;
    n = v2(buf, 1, 0x21, ipv6, sizeof(ipv6));
    expect("v2 IPv6", proxy_protocol_parse(buf, n, &h) == PROXY_OK);
    expect("v2 IPv6 source", h.has_source && strcmp(h.source_ip, "2001:db8::42") == 0 && h.source_port == 443);

    n = v2(buf, 0, 0x00, NULL, 0);
    expect("v2 LOCAL", proxy_protocol_parse(buf, n, &h) == PROXY_OK && !h.has_source && h.length == 16);

    unsigned char with_tlv[12 + 7] = {198, 51, 100, 9, 127, 0, 0, 1, 0, 1, 0, 2, 0x04, 0x00, 0x04, 'a', 'b', 'c', 'd'};
    n = v2(buf, 1, 0x11, with_tlv, sizeof(with_tlv));
    expect("v2 TLVs skipped", proxy_protocol_parse(buf, n, &h) == PROXY_OK && h.length == n &&
                              strcmp(h.source_ip, "198.51.100.9") == 0);

    n = v2(buf, 1, 0x12, ipv4, sizeof(ipv4));
    expect("v2 UDP keeps peer", proxy_protocol_parse(buf, n, &h) == PROXY_OK && !h.has_source);

    n = v2(buf, 1, 0x11, ipv4, sizeof(ipv4));
    expect("v2 signature only", proxy_protocol_parse(buf, 7, &h) == PROXY_INCOMPLETE);
    expect("v2 truncated address", proxy_protocol_parse(buf, n - 3, &h) == PROXY_INCOMPLETE);

    n = v2(buf, 1, 0x11, ipv4, 8);
    expect("v2 short IPv4 block", proxy_protocol_parse(buf, n, &h) == PROXY_INVALID);

    n = v2(buf, 1, 0x11, ipv4, sizeof(ipv4));
    buf[12] = 0x11;
    expect("v2 wrong version", proxy_protocol_parse(buf, n, &h) == PROXY_INVALID);
    buf[12] = 0x22;
    expect("v2 unknown command", proxy_protocol_parse(buf, n, &h) == PROXY_INVALID);

    buf[12] = 0x21;
    buf[14] = (unsigned char)((PROXY_V2_MAX_PAYLOAD + 1) >> 8);
    buf[15] = (unsigned char)((PROXY_V2_MAX_PAYLOAD + 1) & 0xFF);
    expect("v2 oversized", proxy_protocol_parse(buf, n, &h) == PROXY_INVALID);
}

static void not_a_header(void) {
    ProxyHeader h;
    expect("HTTP/1.1", parse_text("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &h) == PROXY_NONE);
    expect("HTTP/0.9", parse_text("GET /\r\n", &h) == PROXY_NONE);
    expect("POST", parse_text("POST /login HTTP/1.0\r\n", &h) == PROXY_NONE);
    expect("PROXY-like method", parse_text("PROXYX / HTTP/1.1\r\n", &h) == PROXY_NONE);
    const unsigned char hello[] = {0x16, 0x03, 0x01, 0x02, 0x00};
    expect("TLS ClientHello", proxy_protocol_parse(hello, sizeof(hello), &h) == PROXY_NONE);
    const unsigned char crlf_then[] = {'\r', '\n', 'G', 'E', 'T'};
    expect("leading CRLF", proxy_protocol_parse(crlf_then, sizeof(crlf_then), &h) == PROXY_NONE);
}

static void format_v1(void) {
    char out[128];
    ProxyHeader h;

    size_t n = proxy_protocol_format_v1(out, sizeof(out), "203.0.113.7", 0, "127.0.0.1", 8081);
    expect("format TCP4", n > 0 && strcmp(out, "PROXY TCP4 203.0.113.7 127.0.0.1 0 8081\r\n") == 0);
    expect("format round trip", parse_text(out, &h) == PROXY_OK && strcmp(h.source_ip, "203.0.113.7") == 0);

    proxy_protocol_format_v1(out, sizeof(out), "2001:db8::1", 1, "::1", 2);
    expect("format TCP6", strcmp(out, "PROXY TCP6 2001:db8::1 ::1 1 2\r\n") == 0);

    proxy_protocol_format_v1(out, sizeof(out), "unknown", 0, "127.0.0.1", 80);
    expect("format UNKNOWN", strcmp(out, "PROXY UNKNOWN\r\n") == 0);

    expect("format too small", proxy_protocol_format_v1(out, 10, "203.0.113.7", 0, "127.0.0.1", 80) == 0);
}

// ---- proxy_protocol_read() over a socket pair ----------------------------------------

struct late_write { int fd; const char *text; };

static void *write_later(void *arg) {
    struct late_write *w = arg;
    struct timespec pause = {0, 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
    ssize_t ignored = write(w->fd, w->text, strlen(w->text));
    (void)ignored;
    return NULL;
}

static int pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
    struct timeval tv = {1, 0};
    setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}

static void read_from_socket(void) {
    int fds[2];
    ProxyHeader h;
    char rest[64] = "";

    // A header followed by the request: exactly the header is consumed.
    if (pair(fds) == 0) {
        const char *text = "PROXY TCP4 203.0.113.7 127.0.0.1 1 2\r\nGET / HTTP/1.1\r\n";
        ssize_t ignored = write(fds[1], text, strlen(text));
        (void)ignored;
        expect("read v1", proxy_protocol_read(fds[0], &h, 1) == PROXY_OK && strcmp(h.source_ip, "203.0.113.7") == 0);
        ssize_t got = recv(fds[0], rest, sizeof(rest) - 1, 0);
        rest[got > 0 ? got : 0] = '\0';
        expect("request left intact", strcmp(rest, "GET / HTTP/1.1\r\n") == 0);
        close(fds[0]);
        close(fds[1]);
    }

    // No header: nothing is consumed.
    if (pair(fds) == 0) {
        ssize_t ignored = write(fds[1], "GET /\r\n", 7);
        (void)ignored;
        expect("read none", proxy_protocol_read(fds[0], &h, 1) == PROXY_NONE);
        ssize_t got = recv(fds[0], rest, sizeof(rest) - 1, 0);
        rest[got > 0 ? got : 0] = '\0';
        expect("HTTP/0.9 request untouched", strcmp(rest, "GET /\r\n") == 0);
        close(fds[0]);
        close(fds[1]);
    }

    // A header split across two writes is waited for.
    if (pair(fds) == 0) {
        ssize_t ignored = write(fds[1], "PROXY TCP4 203.0.", 17);
        (void)ignored;
        struct late_write w = {fds[1], "113.7 127.0.0.1 1 2\r\n"};
        pthread_t t;
        pthread_create(&t, NULL, write_later, &w);
        expect("read split header", proxy_protocol_read(fds[0], &h, 2) == PROXY_OK &&
                                    strcmp(h.source_ip, "203.0.113.7") == 0);
        pthread_join(t, NULL);
        close(fds[0]);
        close(fds[1]);
    }

    // A header that never completes times out.
    if (pair(fds) == 0) {
        ssize_t ignored = write(fds[1], "PROXY TCP4", 10);
        (void)ignored;
        expect("incomplete header times out", proxy_protocol_read(fds[0], &h, 1) == PROXY_INVALID);
        close(fds[0]);
        close(fds[1]);
    }

    // A peer that sends nothing and hangs up.
    if (pair(fds) == 0) {
        close(fds[1]);
        expect("closed before a header", proxy_protocol_read(fds[0], &h, 1) == PROXY_INVALID);
        close(fds[0]);
    }
}

int main(void) {
    v1_headers();
    v2_headers();
    not_a_header();
    format_v1();
    read_from_socket();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
