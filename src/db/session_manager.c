#include "session_manager.h"
#include "mongodb_manager.h"
#include "../utils/config_loader.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SESSION_TOKEN_BYTES 32

char *generate_session_token(void) {
    unsigned char buf[SESSION_TOKEN_BYTES];
    randombytes_buf(buf, sizeof(buf));

    char *token = malloc(SESSION_TOKEN_BUF_SIZE);
    if (!token) return NULL;

    sodium_bin2hex(token, SESSION_TOKEN_BUF_SIZE, buf, sizeof(buf));
    return token;
}

int create_session(const char *user_id_hex, const char *token, int ttl_seconds) {
    if (!bson_oid_is_valid(user_id_hex, strlen(user_id_hex))) {
        LOG_ERROR("create_session: invalid user_id '%s'", user_id_hex);
        return -1;
    }

    mongoc_collection_t *collection = mongodb_manager_get_collection(SESSIONS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t user_oid;
    bson_oid_init_from_string(&user_oid, user_id_hex);

    int64_t now_ms     = (int64_t)time(NULL) * 1000;
    int64_t expires_ms = now_ms + (int64_t)ttl_seconds * 1000;

    bson_t *doc = BCON_NEW(
        "user_id",    BCON_OID(&user_oid),
        "token",      BCON_UTF8(token),
        "created_at", BCON_DATE_TIME(now_ms),
        "expires_at", BCON_DATE_TIME(expires_ms)
    );

    bson_error_t error;
    bool ok = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("create_session: insert failed: %s", error.message);

    bson_destroy(doc);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int validate_session(const char *token, char *user_id_out) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SESSIONS_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW("token", BCON_UTF8(token));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int result = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "expires_at") && BSON_ITER_HOLDS_DATE_TIME(&iter)) {
            int64_t expires_ms = bson_iter_date_time(&iter);
            if (expires_ms > (int64_t)time(NULL) * 1000) {
                if (bson_iter_init_find(&iter, doc, "user_id") && BSON_ITER_HOLDS_OID(&iter)) {
                    bson_oid_to_string(bson_iter_oid(&iter), user_id_out);
                    result = 1;
                }
            }
        }
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("validate_session: cursor error: %s", error.message);
        result = -1;
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result;
}

int extract_session_token(const char *cookie_header, char *token_out, size_t size) {
    if (!cookie_header) return 0;

    static const char prefix[] = SESSION_COOKIE_NAME "=";
    const char *p = strstr(cookie_header, prefix);
    if (!p) return 0;
    p += sizeof(prefix) - 1;

    size_t i = 0;
    while (p[i] && p[i] != ';' && i < size - 1) {
        token_out[i] = p[i];
        i++;
    }
    token_out[i] = '\0';
    return i > 0;
}

int validate_session_cookie(const char *cookie_header, char *user_id_out) {
    char token[SESSION_TOKEN_BUF_SIZE];
    if (!extract_session_token(cookie_header, token, sizeof(token))) return 0;
    return validate_session(token, user_id_out);
}

int destroy_session(const char *token) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SESSIONS_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW("token", BCON_UTF8(token));
    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);
    if (!ok) LOG_ERROR("destroy_session: delete failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

void build_session_cookie_header(const char *token, int ttl_seconds, char *header_out, size_t size) {
    snprintf(header_out, size,
        "Set-Cookie: %s=%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Lax%s\r\n",
        SESSION_COOKIE_NAME, token, ttl_seconds, ssl_enabled ? "; Secure" : "");
}

void build_session_clear_cookie_header(char *header_out, size_t size) {
    snprintf(header_out, size,
        "Set-Cookie: %s=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax%s\r\n",
        SESSION_COOKIE_NAME, ssl_enabled ? "; Secure" : "");
}
