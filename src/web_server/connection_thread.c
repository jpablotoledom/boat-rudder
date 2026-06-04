#include "connection_thread.h"
#include "connection.h"
#include "http_router.h"
#include "../utils/log.h"
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
    args->ssl           = NULL;

#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(conn->client_socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

    struct timeval timeout = {5, 0};
    setsockopt(conn->client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(conn->client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

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
