#ifndef TLS_CONTEXT_H
#define TLS_CONTEXT_H

#include <openssl/ssl.h>

SSL_CTX *tls_create_context(const char *cert_path, const char *key_path);
void     tls_free_context(SSL_CTX *ctx);

#endif // TLS_CONTEXT_H
