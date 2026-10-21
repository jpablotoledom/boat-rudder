#ifndef PROXY_PROTOCOL_H
#define PROXY_PROTOCOL_H

// PROXY protocol v1 (text) and v2 (binary) - the header a layer-4 front
// (HAProxy in `mode tcp`, send-proxy-v2) puts before the client's own bytes
// so the instance behind it learns the client's real address. Spec:
// https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt. Read in
// connection_thread.c when `proxy_protocol=1` (see configuration.md), and
// written by the WAP gateway for its loopback fetches.

#include <netinet/in.h>
#include <stddef.h>

// The longest header accepted: v2's 16 fixed bytes plus an address block
// and TLVs. HAProxy's send-proxy-v2 sends 28 (IPv4) or 52 (IPv6) bytes
// without TLVs; v1 is at most 107 by spec.
#define PROXY_V2_MAX_PAYLOAD 512
#define PROXY_HEADER_MAX     (16 + PROXY_V2_MAX_PAYLOAD)

typedef enum {
    PROXY_NONE,         // the bytes don't start with a PROXY header
    PROXY_INCOMPLETE,   // they could still become one - more bytes needed
    PROXY_INVALID,      // a PROXY signature followed by a malformed header
    PROXY_OK            // a complete header, described in ProxyHeader
} ProxyParseStatus;

typedef struct {
    size_t length;                          // bytes the header occupies
    int    has_source;                      // 0: v2 LOCAL, v1 UNKNOWN, or a non-IP family
    char   source_ip[INET6_ADDRSTRLEN];     // the client's address, when has_source
    int    source_port;
} ProxyHeader;

// Parses the start of `buf`. Pure - no I/O - so it can be unit tested.
ProxyParseStatus proxy_protocol_parse(const unsigned char *buf, size_t len, ProxyHeader *out);

// Reads a header from `fd` without consuming anything past it: peeks until
// the bytes are classified, then drains exactly the header. PROXY_NONE
// leaves the client's bytes untouched for the HTTP parser or SSL_accept().
// Relies on the socket's SO_RCVTIMEO for each read and gives up after
// `timeout_secs` overall; a timeout, a closed connection or a header split
// beyond PROXY_HEADER_MAX all return PROXY_INVALID.
ProxyParseStatus proxy_protocol_read(int fd, ProxyHeader *out, int timeout_secs);

// Writes a v1 header ("PROXY TCP4 <src> <dst> <sport> <dport>\r\n", TCP6
// for IPv6 addresses, "PROXY UNKNOWN\r\n" when either address isn't valid).
// Returns its length, or 0 if `out` is too small.
size_t proxy_protocol_format_v1(char *out, size_t out_size,
                                const char *src_ip, int src_port,
                                const char *dst_ip, int dst_port);

#endif // PROXY_PROTOCOL_H
