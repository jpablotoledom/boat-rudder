#include "mongodb_manager.h"
#include "../utils/log.h"
#include <pthread.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

// mongoc_client_t is NOT thread-safe. A mongoc_client_pool_t hands out one
// client per thread; a thread-local destructor returns it to the pool when
// the (detached) connection-handling thread exits.
static mongoc_client_pool_t *global_pool = NULL;
static char                  global_db_name[64] = {0};
static pthread_key_t         tls_client_key;
static pthread_once_t        tls_key_once = PTHREAD_ONCE_INIT;

static void return_client_to_pool(void *client) {
    if (client && global_pool) {
        mongoc_client_pool_push(global_pool, (mongoc_client_t *)client);
    }
}

static void create_tls_key(void) {
    pthread_key_create(&tls_client_key, return_client_to_pool);
}

int mongodb_manager_init(const char *uri, const char *db_name) {
    if (sodium_init() < 0) {
        LOG_ERROR("mongodb_manager_init: libsodium initialization failed");
        return -1;
    }

    mongoc_init();

    mongoc_uri_t *parsed_uri = mongoc_uri_new(uri);
    if (!parsed_uri) {
        LOG_ERROR("mongodb_manager_init: invalid MongoDB URI: %s", uri);
        mongoc_cleanup();
        return -1;
    }

    global_pool = mongoc_client_pool_new(parsed_uri);
    mongoc_uri_destroy(parsed_uri);

    if (!global_pool) {
        LOG_ERROR("mongodb_manager_init: failed to create client pool");
        mongoc_cleanup();
        return -1;
    }

    strncpy(global_db_name, db_name, sizeof(global_db_name) - 1);
    global_db_name[sizeof(global_db_name) - 1] = '\0';

    pthread_once(&tls_key_once, create_tls_key);

    LOG_INFO("MongoDB client pool initialized (db=%s)", global_db_name);
    return 0;
}

void mongodb_manager_cleanup(void) {
    if (global_pool) {
        mongoc_client_pool_destroy(global_pool);
        global_pool = NULL;
        mongoc_cleanup();
        LOG_DEBUG("MongoDB client pool destroyed");
    }
}

int mongodb_manager_is_ready(void) {
    return global_pool != NULL;
}

mongoc_client_t *mongodb_manager_get_client(void) {
    if (!global_pool) return NULL;

    mongoc_client_t *client = (mongoc_client_t *)pthread_getspecific(tls_client_key);
    if (!client) {
        client = mongoc_client_pool_pop(global_pool);
        if (client) pthread_setspecific(tls_client_key, client);
    }
    return client;
}

mongoc_collection_t *mongodb_manager_get_collection(const char *collection_name) {
    mongoc_client_t *client = mongodb_manager_get_client();
    if (!client) return NULL;
    return mongoc_client_get_collection(client, global_db_name, collection_name);
}
