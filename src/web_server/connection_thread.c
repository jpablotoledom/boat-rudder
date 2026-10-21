#include "connection_thread.h"
#include "connection.h"
#include "http_router.h"
#include "proxy_protocol.h"
#include "server_listener.h"
#include "../utils/config_loader.h"
#include "../utils/http_utils.h"
#include "../utils/log.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

extern void unregister_connection(void);

// With proxy_protocol=1, reads the PROXY header a front puts before the
// client's bytes (before SSL_accept(): the header is never encrypted) and
// keeps the client's address in conn->proxy_client_ip. Returns 0 when the
// connection must be closed:
//   header from a trusted_proxies peer   -> accepted
//   header from any other peer           -> closed (it could claim any address)
//   no header, loopback peer             -> served as a direct connection
//                                           (WAP gateway, updater health check)
//   no header, any other peer            -> closed (it bypassed the front)
//   malformed, truncated or timed out    -> closed
static int read_proxy_header(connection_ctx_t *conn) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char peer_ip[INET_ADDRSTRLEN] = "unknown";
    int loopback = 0;
    if (getpeername(conn->client_socket, (struct sockaddr *)&peer, &peer_len) == 0 &&
        peer.sin_family == AF_INET) {
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        loopback = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
    }

    ProxyHeader header;
    switch (proxy_protocol_read(conn->client_socket, &header, connection_io_timeout_secs)) {
    case PROXY_OK:
        if (!is_trusted_proxy(peer_ip)) {
            LOG_WARN("PROXY header from untrusted peer %s, closing", peer_ip);
            return 0;
        }
        if (header.has_source)
            snprintf(conn->proxy_client_ip, sizeof(conn->proxy_client_ip), "%s", header.source_ip);
        return 1;
    case PROXY_NONE:
        if (loopback) return 1;
        LOG_WARN("No PROXY header from %s, closing", peer_ip);
        return 0;
    default:
        LOG_WARN("Malformed or incomplete PROXY header from %s, closing", peer_ip);
        return 0;
    }
}

void *connection_thread(void *arg) {
    signal(SIGPIPE, SIG_IGN);

    struct thread_args *args = arg;
    if (!args) {
        LOG_ERROR("connection_thread: args are NULL");
        unregister_connection();
        return NULL;
    }

    connection_ctx_t *conn = malloc(sizeof(*conn));
    if (!conn) {
        LOG_ERROR("connection_thread: malloc failed");
        if (args->ssl) SSL_free(args->ssl);
        close(args->client_socket);
        free(args);
        unregister_connection();
        return NULL;
    }

    conn->client_socket = args->client_socket;
    conn->ssl           = args->ssl;
    conn->simple_response    = 0;
    conn->header_end_matched = 0;
    conn->proxy_client_ip[0] = '\0';
    args->ssl           = NULL;

#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(conn->client_socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

    // Slow-loris defense: applies per read/write call, not to total transfer
    // time, so a slow-but-steady large upload isn't cut off by a low value
    // here - only a client that stalls mid-read/write longer than this is.
    struct timeval timeout = {connection_io_timeout_secs, 0};
    setsockopt(conn->client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(conn->client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (proxy_protocol) {
        if (!read_proxy_header(conn)) goto cleanup;
        // The per-IP limit the accept loop skipped for the front (see
        // rate_limit_after_header() in server_listener.c), now against the
        // visitor's own address.
        if (conn->proxy_client_ip[0] && server_ip_rate_limited(conn->proxy_client_ip)) {
            LOG_WARN("Rate limit: dropping %s", conn->proxy_client_ip);
            if (!conn->ssl) server_send_429(conn->client_socket);
            goto cleanup;
        }
    }

    if (conn->ssl) {
        int ret = SSL_accept(conn->ssl);
        if (ret <= 0) {
            int err = SSL_get_error(conn->ssl, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                LOG_WARN("TLS handshake failed (err=%d)", err);
            }
            goto cleanup;
        }
        LOG_DEBUG("TLS handshake complete");
    }

    http_route(conn->ssl ? ssl_read : plain_read, conn, args->root_directory);

cleanup:
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->client_socket >= 0) {
        close(conn->client_socket);
        conn->client_socket = -1;
    }
    free(conn);
    free(args);
    unregister_connection();
    return NULL;
}
