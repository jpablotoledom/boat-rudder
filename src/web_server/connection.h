#ifndef CONNECTION_H
#define CONNECTION_H

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int   client_socket;
    SSL  *ssl;              // NULL → plain TCP, non-NULL → TLS session
    // HTTP/0.9 "Simple-Response" (RFC 1945): when set, connection_write()
    // drops everything up to and including the blank line that ends the
    // header block - however it is split across writes - and sends only the
    // body. Set by the router for a genuine HTTP/0.9 request (no version, no
    // headers), whose client would otherwise show the status line and
    // headers as the start of the page.
    int   simple_response;
    int   header_end_matched;   // bytes of "\r\n\r\n" seen so far; 4 = body
    // The client's address from a PROXY protocol header (connection_thread.c);
    // empty when there was none and the socket's peer is the client.
    char  proxy_client_ip[INET6_ADDRSTRLEN];
} connection_ctx_t;

ssize_t connection_write(void *ctx, const char *buf, size_t count);
void    connection_close(void *ctx);
ssize_t plain_read(void *ctx, char *buf, size_t count);
ssize_t ssl_read(void *ctx, char *buf, size_t count);

#endif // CONNECTION_H
