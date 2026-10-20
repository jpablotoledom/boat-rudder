#define _POSIX_C_SOURCE 200809L

#include "proxy_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const unsigned char V2_SIGNATURE[12] = {
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A
};
static const char V1_PREFIX[] = "PROXY ";
#define V1_PREFIX_LEN 6
#define V1_MAX_LEN    107   // the spec's worst case, "\r\n" included

static int is_prefix_of(const unsigned char *buf, size_t len, const unsigned char *sig, size_t sig_len) {
    return len < sig_len && memcmp(buf, sig, len) == 0;
}

// ---- v2 ------------------------------------------------------------------------------

static ProxyParseStatus parse_v2(const unsigned char *buf, size_t len, ProxyHeader *out) {
    if (len < 16) return PROXY_INCOMPLETE;

    int version = buf[12] >> 4;
    int command = buf[12] & 0x0F;
    int family  = buf[13] >> 4;
    int proto   = buf[13] & 0x0F;
    size_t payload = ((size_t)buf[14] << 8) | buf[15];

    if (version != 2 || command > 1 || payload > PROXY_V2_MAX_PAYLOAD) return PROXY_INVALID;
    if (len < 16 + payload) return PROXY_INCOMPLETE;

    out->length = 16 + payload;
    // LOCAL (health checks from the proxy itself), UNSPEC, UNIX or a
    // datagram protocol: the spec says to keep the connection's own address.
    if (command == 0 || proto != 1 || (family != 1 && family != 2)) return PROXY_OK;

    const unsigned char *a = buf + 16;
    if (family == 1) {
        if (payload < 12) return PROXY_INVALID;
        inet_ntop(AF_INET, a, out->source_ip, sizeof(out->source_ip));
        out->source_port = (a[8] << 8) | a[9];
    } else {
        if (payload < 36) return PROXY_INVALID;
        inet_ntop(AF_INET6, a, out->source_ip, sizeof(out->source_ip));
        out->source_port = (a[32] << 8) | a[33];
    }
    out->has_source = 1;
    return PROXY_OK;
}

// ---- v1 ------------------------------------------------------------------------------

static int parse_port(const char *s, int *port) {
    if (!*s || strlen(s) > 5) return 0;
    if (s[0] == '0' && s[1]) return 0;              // no leading zeros, per spec
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (*s - '0');
    }
    if (v > 65535) return 0;
    *port = v;
    return 1;
}

static ProxyParseStatus parse_v1(const unsigned char *buf, size_t len, ProxyHeader *out) {
    const unsigned char *crlf = NULL;
    size_t scan = len < V1_MAX_LEN ? len : V1_MAX_LEN;
    for (size_t i = 0; i + 1 < scan; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n') { crlf = buf + i; break; }
    if (!crlf) return len < V1_MAX_LEN ? PROXY_INCOMPLETE : PROXY_INVALID;

    char line[V1_MAX_LEN];
    size_t line_len = (size_t)(crlf - buf);
    memcpy(line, buf, line_len);
    line[line_len] = '\0';
    out->length = line_len + 2;

    // Exactly one space between fields; strtok_r would quietly accept two.
    // A seventh field means a malformed line, so up to 7 are split off.
    char *field[7];
    int count = 0;
    for (char *p = line; count < 7; ) {
        field[count++] = p;
        char *space = strchr(p, ' ');
        if (!space) break;
        *space = '\0';
        p = space + 1;
    }

    if (count >= 2 && strcmp(field[1], "UNKNOWN") == 0) return PROXY_OK;
    if (count != 6) return PROXY_INVALID;

    int af;
    if (strcmp(field[1], "TCP4") == 0) af = AF_INET;
    else if (strcmp(field[1], "TCP6") == 0) af = AF_INET6;
    else return PROXY_INVALID;

    unsigned char src[16], dst[16];
    int src_port, dst_port;
    if (inet_pton(af, field[2], src) != 1 || inet_pton(af, field[3], dst) != 1 ||
        !parse_port(field[4], &src_port) || !parse_port(field[5], &dst_port))
        return PROXY_INVALID;

    inet_ntop(af, src, out->source_ip, sizeof(out->source_ip));
    out->source_port = src_port;
    out->has_source = 1;
    return PROXY_OK;
}

// ---- Public --------------------------------------------------------------------------

ProxyParseStatus proxy_protocol_parse(const unsigned char *buf, size_t len, ProxyHeader *out) {
    memset(out, 0, sizeof(*out));
    if (len == 0) return PROXY_INCOMPLETE;

    if (len >= sizeof(V2_SIGNATURE) && memcmp(buf, V2_SIGNATURE, sizeof(V2_SIGNATURE)) == 0)
        return parse_v2(buf, len, out);
    if (len >= V1_PREFIX_LEN && memcmp(buf, V1_PREFIX, V1_PREFIX_LEN) == 0)
        return parse_v1(buf, len, out);

    if (is_prefix_of(buf, len, V2_SIGNATURE, sizeof(V2_SIGNATURE)) ||
        is_prefix_of(buf, len, (const unsigned char *)V1_PREFIX, V1_PREFIX_LEN))
        return PROXY_INCOMPLETE;
    return PROXY_NONE;
}

ProxyParseStatus proxy_protocol_read(int fd, ProxyHeader *out, int timeout_secs) {
    unsigned char buf[PROXY_HEADER_MAX];
    time_t deadline = time(NULL) + (timeout_secs > 0 ? timeout_secs : 1);
    ProxyParseStatus status;

    // MSG_PEEK returns at once while anything is buffered, so waiting for
    // the rest of a split header is a short sleep loop. A front sends the
    // header in one write right after connect(), so in practice the first
    // peek sees all of it.
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_PEEK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return PROXY_INVALID;

        status = proxy_protocol_parse(buf, (size_t)n, out);
        if (status != PROXY_INCOMPLETE) break;
        if ((size_t)n == sizeof(buf) || time(NULL) >= deadline) return PROXY_INVALID;

        struct timespec pause = {0, 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }

    if (status != PROXY_OK) return status;

    size_t drained = 0;
    while (drained < out->length) {
        ssize_t n = recv(fd, buf + drained, out->length - drained, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return PROXY_INVALID;
        drained += (size_t)n;
    }
    return PROXY_OK;
}

size_t proxy_protocol_format_v1(char *out, size_t out_size,
                                const char *src_ip, int src_port,
                                const char *dst_ip, int dst_port) {
    unsigned char addr[16];   // only validated, never used
    const char *family = NULL;
    if (src_ip && dst_ip) {
        if (inet_pton(AF_INET, src_ip, addr) == 1 && inet_pton(AF_INET, dst_ip, addr) == 1)
            family = "TCP4";
        else if (inet_pton(AF_INET6, src_ip, addr) == 1 && inet_pton(AF_INET6, dst_ip, addr) == 1)
            family = "TCP6";
    }
    if (src_port < 0 || src_port > 65535 || dst_port < 0 || dst_port > 65535) family = NULL;

    int n = family
        ? snprintf(out, out_size, "PROXY %s %s %s %d %d\r\n", family, src_ip, dst_ip, src_port, dst_port)
        : snprintf(out, out_size, "PROXY UNKNOWN\r\n");
    return (n > 0 && (size_t)n < out_size) ? (size_t)n : 0;
}
