#include "auth.h"
#include "mongodb_manager.h"
#include "session_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

char *auth_login_user(const char *email, const char *password) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return NULL;

    bson_t *query = BCON_NEW("email", BCON_UTF8(email));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *user_id = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        bson_iter_t id_iter;

        if (bson_iter_init_find(&iter, doc, "password") && BSON_ITER_HOLDS_UTF8(&iter) &&
            bson_iter_init_find(&id_iter, doc, "_id") && BSON_ITER_HOLDS_OID(&id_iter)) {

            const char *stored_hash = bson_iter_utf8(&iter, NULL);

            if (crypto_pwhash_str_verify(stored_hash, password, strlen(password)) == 0) {
                user_id = malloc(USER_ID_HEX_BUF_SIZE);
                if (user_id) bson_oid_to_string(bson_iter_oid(&id_iter), user_id);
            }
        }
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("auth_login_user: cursor error: %s", error.message);
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return user_id;
}
