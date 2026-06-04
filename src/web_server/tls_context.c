#include "tls_context.h"
#include "../utils/log.h"
#include <openssl/err.h>
#include <openssl/ssl.h>

SSL_CTX *tls_create_context(const char *cert_path, const char *key_path) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        LOG_ERROR("Failed to create TLS context");
        return NULL;
    }

    // Minimum TLS 1.2; reject older protocols.
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        LOG_ERROR("Failed to set minimum TLS version");
        SSL_CTX_free(ctx);
        return NULL;
    }

    // TLS 1.2 cipher suites: ECDHE + AESGCM or ChaCha20, no weak ciphers.
    if (SSL_CTX_set_cipher_list(ctx,
            "ECDHE+AESGCM:ECDHE+CHACHA20:!aNULL:!eNULL:!RC4:!3DES:!EXPORT") != 1) {
        LOG_WARN("Failed to set TLS 1.2 cipher list; using OpenSSL defaults");
    }

    // TLS 1.3 cipher suites.
    if (SSL_CTX_set_ciphersuites(ctx,
            "TLS_AES_256_GCM_SHA384:"
            "TLS_CHACHA20_POLY1305_SHA256:"
            "TLS_AES_128_GCM_SHA256") != 1) {
        LOG_WARN("Failed to set TLS 1.3 cipher suites; using OpenSSL defaults");
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load TLS certificate: %s", cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load TLS private key: %s", key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

void tls_free_context(SSL_CTX *ctx) {
    if (ctx) SSL_CTX_free(ctx);
}
