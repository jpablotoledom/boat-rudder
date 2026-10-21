#include "cms_users_admin.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Where media folders live - the router's ./html/content/posts/<username>/.
#define MEDIA_POSTS_ROOT "./html/content/posts"

static int is_valid_role(const char *role) {
    return strcmp(role, "admin") == 0 || strcmp(role, "author") == 0 ||
           strcmp(role, "viewer") == 0;
}

void cms_get_users(CmsUserAdminItem **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW(
        "sort", "{", "email", BCON_INT32(1), "}",
        "limit", BCON_INT64((int64_t)USERS_LIST_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsUserAdminItem *items = calloc(USERS_LIST_LIMIT, sizeof(CmsUserAdminItem));
    if (!items) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < USERS_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        char id_str[25] = "";
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), id_str);

        items[count].id = strdup(id_str);
        items[count].name = (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].username = (bson_iter_init_find(&iter, doc, "username") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].email = (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        items[count].role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("admin");

        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_users: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = items;
    *out_count = count;
}

void cms_users_admin_free(CmsUserAdminItem *items, size_t count) {
    if (!items) return;

    for (size_t i = 0; i < count; i++) {
        free(items[i].id);
        free(items[i].name);
        free(items[i].username);
        free(items[i].email);
        free(items[i].role);
    }

    free(items);
}

int cms_get_user_values(const char *id_hex, char *out_email, size_t out_email_size,
                         char *out_role, size_t out_role_size) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        const char *email = (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "";
        strncpy(out_email, email, out_email_size - 1);
        out_email[out_email_size - 1] = '\0';

        const char *role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "admin";
        strncpy(out_role, role, out_role_size - 1);
        out_role[out_role_size - 1] = '\0';

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_user_values: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

int cms_get_user_role(const char *id_hex, char *out_role, size_t out_role_size) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        const char *role = (bson_iter_init_find(&iter, doc, "role") && BSON_ITER_HOLDS_UTF8(&iter))
            ? bson_iter_utf8(&iter, NULL) : "admin";
        strncpy(out_role, role, out_role_size - 1);
        out_role[out_role_size - 1] = '\0';

        found = 1;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_user_role: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

int cms_count_admins(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW(
        "$or", "[",
            "{", "role", BCON_UTF8("admin"), "}",
            "{", "role", "{", "$exists", BCON_BOOL(false), "}", "}",
        "]"
    );

    bson_error_t error;
    int64_t count = mongoc_collection_count_documents(collection, query, NULL, NULL, NULL, &error);
    if (count < 0) LOG_ERROR("cms_count_admins: count failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return (int)count;
}

int cms_username_valid(const char *username) {
    size_t len = username ? strlen(username) : 0;
    if (len < USERNAME_MIN_LEN || len > USERNAME_MAX_LEN || username[0] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

// 1 if some user already has `username`, -1 on a DB error.
static int username_taken(mongoc_collection_t *collection, const char *username) {
    bson_error_t error;
    bson_t *query = BCON_NEW("username", BCON_UTF8(username));
    int64_t n = mongoc_collection_count_documents(collection, query, NULL, NULL, NULL, &error);
    bson_destroy(query);
    if (n < 0) LOG_ERROR("username_taken: count failed: %s", error.message);
    return n < 0 ? -1 : n > 0;
}

int cms_create_user(const char *name, const char *username, const char *email,
                     const char *password, const char *role) {
    if (!is_valid_role(role)) return -1;
    if (!cms_username_valid(username)) return CMS_USER_USERNAME_INVALID;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    int taken = username_taken(collection, username);
    if (taken != 0) {
        mongoc_collection_destroy(collection);
        return taken > 0 ? CMS_USER_USERNAME_TAKEN : -1;
    }

    bson_error_t error;
    bson_t *email_query = BCON_NEW("email", BCON_UTF8(email));
    int64_t existing = mongoc_collection_count_documents(collection, email_query, NULL, NULL, NULL, &error);
    bson_destroy(email_query);
    if (existing != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password, strlen(password),
                           crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *doc = BCON_NEW(
        "name", BCON_UTF8(name),
        "username", BCON_UTF8(username),
        "email", BCON_UTF8(email),
        "password", BCON_UTF8(hash),
        "role", BCON_UTF8(role)
    );
    bool ok = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_create_user: insert failed: %s", error.message);
    bson_destroy(doc);

    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_user(const char *id_hex, const char *name, const char *email, const char *role,
                     const char *new_password) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;
    if (!is_valid_role(role)) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);

    bson_error_t error;
    bson_t *email_query = BCON_NEW(
        "email", BCON_UTF8(email),
        "_id", "{", "$ne", BCON_OID(&oid), "}"
    );
    int64_t existing = mongoc_collection_count_documents(collection, email_query, NULL, NULL, NULL, &error);
    bson_destroy(email_query);
    if (existing != 0) {
        mongoc_collection_destroy(collection);
        return -1;
    }

    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t update;
    bson_init(&update);
    bson_t set_doc;
    bson_append_document_begin(&update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "name", -1, name, -1);
    bson_append_utf8(&set_doc, "email", -1, email, -1);
    bson_append_utf8(&set_doc, "role", -1, role, -1);

    if (new_password[0] != '\0') {
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(hash, new_password, strlen(new_password),
                               crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
            bson_append_document_end(&update, &set_doc);
            bson_destroy(&update);
            bson_destroy(query);
            mongoc_collection_destroy(collection);
            return -1;
        }
        bson_append_utf8(&set_doc, "password", -1, hash, -1);
    }

    bson_append_document_end(&update, &set_doc);

    bool ok = mongoc_collection_update_one(collection, query, &update, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_user: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_delete_user(const char *id_hex) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_t reply;
    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, &reply, &error);
    if (!ok) LOG_ERROR("cms_delete_user: delete failed: %s", error.message);

    int deleted = 0;
    if (ok) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT32(&iter))
            deleted = bson_iter_int32(&iter);
    }
    bson_destroy(&reply);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return (ok && deleted > 0) ? 0 : -1;
}

char *cms_get_user_name_by_id(const char *id_hex) {
    if (!id_hex || !bson_oid_is_valid(id_hex, strlen(id_hex))) return strdup("");

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return strdup("");

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup("");
}

void cms_username_from_email(const char *email, char *out, size_t out_size) {
    if (out_size == 0) return;
    if (!email) email = "";
    const char *at = strchr(email, '@');
    size_t len = at ? (size_t)(at - email) : strlen(email);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, email, len);
    out[len] = '\0';

    // Sanitize: keep only alphanumeric, dash, underscore, dot
    for (size_t i = 0; i < len; i++) {
        char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            out[i] = '-';
    }
}

int cms_get_username_by_id(const char *id_hex, char *out, size_t out_size) {
    if (out_size == 0) return -1;
    out[0] = '\0';
    if (!id_hex || !bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "username") && BSON_ITER_HOLDS_UTF8(&iter) &&
            bson_iter_utf8(&iter, NULL)[0]) {
            snprintf(out, out_size, "%s", bson_iter_utf8(&iter, NULL));
        } else if (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter)) {
            cms_username_from_email(bson_iter_utf8(&iter, NULL), out, out_size);
        }
        found = 1;
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found ? 0 : -1;
}

// ---- Startup migration: users.username ----------------------------------

// The folder under MEDIA_POSTS_ROOT holding the most of `user_oid`'s
// media_directories by name (posts/<folder>/<dir name>/), and not already
// some user's username - where their files already are. 1 and fills `out`
// if one is found, 0 otherwise. `shared` gets the folder their directories
// were found in but that another user already took (two emails with one
// prefix used to share a folder), "" if none.
static int media_folder_of(mongoc_collection_t *users, const bson_oid_t *user_oid,
                           char *out, size_t out_size, char *shared, size_t shared_size) {
    shared[0] = '\0';
    mongoc_collection_t *dirs_col = mongodb_manager_get_collection(MEDIA_DIRECTORIES_COLLECTION);
    if (!dirs_col) return 0;

    char dir_names[32][128];
    size_t dir_count = 0;
    bson_t *query = BCON_NEW("author_id", BCON_OID(user_oid));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(dirs_col, query, NULL, NULL);
    const bson_t *doc;
    while (dir_count < 32 && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            snprintf(dir_names[dir_count++], sizeof(dir_names[0]), "%s", bson_iter_utf8(&iter, NULL));
    }
    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(dirs_col);
    if (dir_count == 0) return 0;

    DIR *root = opendir(MEDIA_POSTS_ROOT);
    if (!root) return 0;

    size_t best_hits = 0;
    struct dirent *de;
    while ((de = readdir(root)) != NULL) {
        const char *folder = de->d_name;
        if (folder[0] == '.' || strlen(folder) >= out_size) continue;
        int ok = 1;
        for (const char *p = folder; *p; p++)
            if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) ok = 0;
        if (!ok) continue;

        size_t hits = 0;
        for (size_t i = 0; i < dir_count; i++) {
            char path[512];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s/%s", MEDIA_POSTS_ROOT, folder, dir_names[i]);
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) hits++;
        }
        if (hits > 0 && username_taken(users, folder) != 0) {
            snprintf(shared, shared_size, "%s", folder);
            continue;
        }
        if (hits > best_hits) {
            best_hits = hits;
            snprintf(out, out_size, "%s", folder);
        }
    }
    closedir(root);
    return best_hits > 0;
}

// The email-derived name made fit for a new username: lowercased, padded or
// cut to USERNAME_MIN_LEN..USERNAME_MAX_LEN, not dot-led, and given a "-2",
// "-3"... suffix while some user already has it.
static void username_for_email(mongoc_collection_t *users, const char *email,
                               char *out, size_t out_size) {
    char base[USERNAME_BUF_SIZE];
    cms_username_from_email(email, base, sizeof(base));
    for (char *p = base; *p; p++) *p = (char)tolower((unsigned char)*p);
    while (base[0] == '.') memmove(base, base + 1, strlen(base));
    if (strlen(base) < USERNAME_MIN_LEN) {
        char short_name[USERNAME_MIN_LEN] = "";
        memcpy(short_name, base, strlen(base) + 1); // < USERNAME_MIN_LEN, NUL included
        snprintf(base, sizeof(base), "user-%s", short_name);
    }
    if (strlen(base) > USERNAME_MAX_LEN - 4) base[USERNAME_MAX_LEN - 4] = '\0'; // room for "-NNN"

    snprintf(out, out_size, "%s", base);
    for (int n = 2; n < 1000 && username_taken(users, out) > 0; n++)
        snprintf(out, out_size, "%s-%d", base, n);
}

// One batch of cms_users_ensure_usernames(): up to USERS_LIST_LIMIT users
// without a username, oldest first. Returns how many got one.
static size_t ensure_usernames_batch(mongoc_collection_t *users, size_t *seen) {
    // Oldest first: a shared email prefix goes to the account that had it first.
    bson_t *query = BCON_NEW("$or", "[",
        "{", "username", "{", "$exists", BCON_BOOL(false), "}", "}",
        "{", "username", BCON_UTF8(""), "}",
    "]");
    bson_t *opts = BCON_NEW("sort", "{", "_id", BCON_INT32(1), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(users, query, opts, NULL);

    // Collect first: updating the collection while its cursor is open could
    // hand the same document back again.
    bson_oid_t ids[USERS_LIST_LIMIT];
    char emails[USERS_LIST_LIMIT][256];
    size_t count = 0;
    const bson_t *doc;
    while (count < USERS_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (!bson_iter_init_find(&iter, doc, "_id") || !BSON_ITER_HOLDS_OID(&iter)) continue;
        bson_oid_copy(bson_iter_oid(&iter), &ids[count]);
        emails[count][0] = '\0';
        if (bson_iter_init_find(&iter, doc, "email") && BSON_ITER_HOLDS_UTF8(&iter))
            snprintf(emails[count], sizeof(emails[0]), "%s", bson_iter_utf8(&iter, NULL));
        count++;
    }
    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);

    size_t done = 0;
    for (size_t i = 0; i < count; i++) {
        char username[USERNAME_BUF_SIZE], shared[USERNAME_BUF_SIZE];
        int from_disk = media_folder_of(users, &ids[i], username, sizeof(username),
                                        shared, sizeof(shared));
        if (!from_disk) username_for_email(users, emails[i], username, sizeof(username));

        bson_t *filter = BCON_NEW("_id", BCON_OID(&ids[i]));
        bson_t *update = BCON_NEW("$set", "{", "username", BCON_UTF8(username), "}");
        bson_error_t error;
        if (mongoc_collection_update_one(users, filter, update, NULL, NULL, &error)) {
            done++;
            LOG_INFO("users: %s gets username '%s' (%s)", emails[i], username,
                     from_disk ? "its existing media folder" : "from its email");
            // Its files sit in a folder that now belongs to someone else: the
            // media library looks in posts/<username>/ and won't find them.
            // Not moved here - published entries' URLs point at where they are.
            if (shared[0] && strcmp(shared, username) != 0)
                LOG_WARN("users: %s's media directories are in html/content/posts/%s/, which is "
                         "user '%s''s folder - move them to html/content/posts/%s/ (and fix the "
                         "entries that link to them) by hand", emails[i], shared, shared, username);
        } else {
            LOG_ERROR("cms_users_ensure_usernames: update failed: %s", error.message);
        }
        bson_destroy(filter);
        bson_destroy(update);
    }

    *seen = count;
    return done;
}

void cms_users_ensure_usernames(void) {
    mongoc_collection_t *users = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!users) return;

    // Batches until none is left - or a batch gets nowhere (a DB error),
    // which must not spin.
    size_t seen;
    while (ensure_usernames_batch(users, &seen) > 0 && seen == USERS_LIST_LIMIT)
        ;

    // Unique among users that have one - partial, so a failed migration
    // above still lets the index (and the server) come up.
    bson_t *command = BCON_NEW(
        "createIndexes", BCON_UTF8(USERS_COLLECTION),
        "indexes", "[",
            "{", "key", "{", "username", BCON_INT32(1), "}",
                 "name", BCON_UTF8("username_unique"),
                 "unique", BCON_BOOL(true),
                 "partialFilterExpression", "{", "username", "{", "$type", BCON_UTF8("string"), "}", "}",
            "}",
        "]");
    bson_error_t error;
    if (!mongoc_collection_write_command_with_opts(users, command, NULL, NULL, &error))
        LOG_WARN("cms_users_ensure_usernames: createIndexes failed: %s", error.message);
    bson_destroy(command);

    mongoc_collection_destroy(users);
}
