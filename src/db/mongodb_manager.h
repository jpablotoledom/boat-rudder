#ifndef MONGODB_MANAGER_H
#define MONGODB_MANAGER_H

#include <mongoc/mongoc.h>

#define USERS_COLLECTION            "users"
#define SESSIONS_COLLECTION         "sessions"
#define ENTRIES_COLLECTION          "entries"
#define ENTRY_CATEGORIES_COLLECTION "entry_categories"
#define MENU_COLLECTION             "menu"
#define LANGUAGES_COLLECTION        "languages"
#define MEDIA_COLLECTION            "media"
#define MEDIA_DIRECTORIES_COLLECTION "media_directories"

// Initializes the MongoDB client pool from `uri`/`db_name` (see
// configs/settings.conf: mongodb_uri, mongodb_db). Must be called once from
// main(), after load_config() and before server_start().
// Returns 0 on success, -1 on failure (logged via LOG_ERROR). The server
// keeps running on failure - only /login and /dashboard degrade to a 503
// error page (see src/modules/error/error.c).
int mongodb_manager_init(const char *uri, const char *db_name);

// Destroys the client pool. Called once from main() during shutdown.
void mongodb_manager_cleanup(void);

// Returns 1 if mongodb_manager_init() succeeded, 0 otherwise.
int mongodb_manager_is_ready(void);

// Returns a per-thread mongoc_client_t from the pool (mongoc_client_t is not
// thread-safe, so each thread keeps its own client for its lifetime; it is
// returned to the pool automatically on thread exit). Returns NULL if the
// pool was never initialized.
mongoc_client_t *mongodb_manager_get_client(void);

// Convenience: mongodb_manager_get_client() + mongoc_client_get_collection()
// against the configured database. Caller must mongoc_collection_destroy()
// the result. Returns NULL if no client is available.
mongoc_collection_t *mongodb_manager_get_collection(const char *collection_name);

#endif // MONGODB_MANAGER_H
