#include "cms_comments.h"
#include "bson_lang.h"
#include "mongodb_manager.h"
#include "cms_users_admin.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Before commenters were users, they had a collection of their own:
// migrated into `users` at startup (cms_comments_ensure_indexes()).
#define LEGACY_COMMENTERS         "commenters"
#define COMMENTS_COLLECTION       "comments"
#define COMMENT_TOKENS_COLLECTION "comment_tokens"
#define CAPTCHA_USED_COLLECTION   "captcha_used"
#define APPROVED_LIMIT            500

static int64_t now_ms(void) { return (int64_t)time(NULL) * 1000; }

static int parse_oid(const char *hex, bson_oid_t *oid) {
    if (!hex || strlen(hex) != 24 || !bson_oid_is_valid(hex, 24)) return 0;
    bson_oid_init_from_string(oid, hex);
    return 1;
}

static char *dup_utf8(const bson_t *doc, const char *key) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, key) && BSON_ITER_HOLDS_UTF8(&it)) return strdup(bson_iter_utf8(&it, NULL));
    return strdup("");
}

static void oid_hex(const bson_t *doc, const char *key, char out[25]) {
    bson_iter_t it;
    out[0] = '\0';
    if (bson_iter_init_find(&it, doc, key) && BSON_ITER_HOLDS_OID(&it)) bson_oid_to_string(bson_iter_oid(&it), out);
}

static void date_str(const bson_t *doc, const char *key, char out[17]) {
    bson_iter_t it;
    out[0] = '\0';
    if (bson_iter_init_find(&it, doc, key) && BSON_ITER_HOLDS_DATE_TIME(&it)) {
        time_t t = (time_t)(bson_iter_date_time(&it) / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(out, 17, "%Y-%m-%d %H:%M", &tm);
    }
}

// Lower-cases ASCII and Latin-1 capitals: names compare without case.
static void fold_case(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (unsigned char)(*p + 32);
        else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) { p[1] = (unsigned char)(p[1] + 0x20); p++; }
    }
}

static void token_hash(const char *token, char out[65]) {
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, (const unsigned char *)token, strlen(token));
    sodium_bin2hex(out, 65, h, sizeof(h));
}

// ---- Indexes ----------------------------------------------------------------------

static void create_index(const char *collection, bson_t *keys, bson_t *opts) {
    mongoc_collection_t *col = mongodb_manager_get_collection(collection);
    if (col) {
        mongoc_index_model_t *model = mongoc_index_model_new(keys, opts);
        bson_error_t error;
        if (!mongoc_collection_create_indexes_with_opts(col, &model, 1, NULL, NULL, &error))
            LOG_ERROR("cms_comments: index on %s: %s", collection, error.message);
        mongoc_index_model_destroy(model);
        mongoc_collection_destroy(col);
    }
    bson_destroy(keys);
    if (opts) bson_destroy(opts);
}

// Emails compare without case: a commenter typing "Ana@X.com" is the user
// "ana@x.com", whoever created it.
static bson_t *email_collation_opts(void) {
    return BCON_NEW("collation", "{", "locale", BCON_UTF8("en"), "strength", BCON_INT32(2), "}");
}

// Moves the legacy `commenters` collection into `users` (role
// "commenter"), keeping each _id - so comments still point at the right
// one - or merging into the user that already has that email.
static void migrate_legacy_commenters(void) {
    mongoc_collection_t *legacy = mongodb_manager_get_collection(LEGACY_COMMENTERS);
    mongoc_collection_t *users = mongodb_manager_get_collection(USERS_COLLECTION);
    mongoc_collection_t *comments = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!legacy || !users || !comments) goto done;
    if (mongoc_collection_count_documents(legacy, &(bson_t)BSON_INITIALIZER, NULL, NULL, NULL, NULL) <= 0) goto done;

    bson_t *all = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(legacy, all, NULL, NULL);
    const bson_t *doc;
    int moved = 0;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t it;
        if (!bson_iter_init_find(&it, doc, "_id") || !BSON_ITER_HOLDS_OID(&it)) continue;
        bson_oid_t old_id;
        bson_oid_copy(bson_iter_oid(&it), &old_id);
        char *email = dup_utf8(doc, "email");
        char *name = dup_utf8(doc, "name");
        char *name_key = dup_utf8(doc, "name_key");
        int verified = bson_iter_init_find(&it, doc, "email_verified_at") && BSON_ITER_HOLDS_DATE_TIME(&it);
        int64_t verified_at = verified ? bson_iter_date_time(&it) : 0;
        bool blocked = bson_iter_init_find(&it, doc, "blocked") && BSON_ITER_HOLDS_BOOL(&it) && bson_iter_bool(&it);

        bson_t *by_email = BCON_NEW("email", BCON_UTF8(email ? email : ""));
        bson_t *opts = email_collation_opts();
        mongoc_cursor_t *uc = mongoc_collection_find_with_opts(users, by_email, opts, NULL);
        const bson_t *user;
        bson_oid_t user_id;
        int existing = mongoc_cursor_next(uc, &user) && bson_iter_init_find(&it, user, "_id") && BSON_ITER_HOLDS_OID(&it);
        if (existing) bson_oid_copy(bson_iter_oid(&it), &user_id);
        mongoc_cursor_destroy(uc);

        int ok = 0;
        if (existing) {
            bson_t *f = BCON_NEW("_id", BCON_OID(&user_id));
            bson_t *u = verified ? BCON_NEW("$set", "{", "email_verified_at", BCON_DATE_TIME(verified_at), "}")
                                 : BCON_NEW("$set", "{", "blocked", BCON_BOOL(blocked), "}");
            ok = mongoc_collection_update_one(users, f, u, NULL, NULL, NULL);
            bson_destroy(f);
            bson_destroy(u);
            bson_t *cf = BCON_NEW("commenter_id", BCON_OID(&old_id));
            bson_t *cu = BCON_NEW("$set", "{", "commenter_id", BCON_OID(&user_id), "}");
            ok = ok && mongoc_collection_update_many(comments, cf, cu, NULL, NULL, NULL);
            bson_destroy(cf);
            bson_destroy(cu);
        } else {
            char username[USERNAME_BUF_SIZE] = "";
            cms_new_username_for_email(email ? email : "", username, sizeof(username));
            bson_t *u = BCON_NEW("_id", BCON_OID(&old_id), "email", BCON_UTF8(email ? email : ""),
                                 "name", BCON_UTF8(name ? name : ""), "name_key", BCON_UTF8(name_key ? name_key : ""),
                                 "username", BCON_UTF8(username), "role", BCON_UTF8("commenter"),
                                 "blocked", BCON_BOOL(blocked), "created_at", BCON_DATE_TIME(now_ms()));
            if (verified) BSON_APPEND_DATE_TIME(u, "email_verified_at", verified_at);
            ok = mongoc_collection_insert_one(users, u, NULL, NULL, NULL);
            bson_destroy(u);
        }
        if (ok) {
            bson_t *d = BCON_NEW("_id", BCON_OID(&old_id));
            mongoc_collection_delete_one(legacy, d, NULL, NULL, NULL);
            bson_destroy(d);
            moved++;
        } else {
            LOG_ERROR("cms_comments: commenter %s could not be moved into users", email ? email : "?");
        }
        bson_destroy(opts);
        bson_destroy(by_email);
        free(email); free(name); free(name_key);
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(all);
    if (moved) LOG_INFO("cms_comments: %d commenter(s) moved into users", moved);
    if (mongoc_collection_count_documents(legacy, &(bson_t)BSON_INITIALIZER, NULL, NULL, NULL, NULL) == 0)
        mongoc_collection_drop(legacy, NULL);
done:
    if (legacy) mongoc_collection_destroy(legacy);
    if (users) mongoc_collection_destroy(users);
    if (comments) mongoc_collection_destroy(comments);
}

void cms_comments_ensure_indexes(void) {
    migrate_legacy_commenters();
    // A commenter's name belongs to one user; team members have no name_key.
    create_index(USERS_COLLECTION, BCON_NEW("name_key", BCON_INT32(1)),
                 BCON_NEW("unique", BCON_BOOL(true), "partialFilterExpression", "{", "name_key", "{", "$type", BCON_UTF8("string"), "}", "}"));
    create_index(USERS_COLLECTION, BCON_NEW("email", BCON_INT32(1)),
                 BCON_NEW("name", BCON_UTF8("email_ci"), "unique", BCON_BOOL(true),
                          "collation", "{", "locale", BCON_UTF8("en"), "strength", BCON_INT32(2), "}"));
    create_index(COMMENTS_COLLECTION, BCON_NEW("entry_id", BCON_INT32(1), "status", BCON_INT32(1), "created_at", BCON_INT32(1)), NULL);
    create_index(COMMENTS_COLLECTION, BCON_NEW("status", BCON_INT32(1), "created_at", BCON_INT32(-1)), NULL);
    create_index(COMMENTS_COLLECTION, BCON_NEW("ip_hash", BCON_INT32(1), "created_at", BCON_INT32(-1)), NULL);
    create_index(COMMENTS_COLLECTION, BCON_NEW("commenter_id", BCON_INT32(1), "created_at", BCON_INT32(-1)), NULL);
    // Only unverified comments carry expires_at; confirming unsets it.
    create_index(COMMENTS_COLLECTION, BCON_NEW("expires_at", BCON_INT32(1)), BCON_NEW("expireAfterSeconds", BCON_INT32(0)));
    create_index(COMMENT_TOKENS_COLLECTION, BCON_NEW("token_hash", BCON_INT32(1)), BCON_NEW("unique", BCON_BOOL(true)));
    create_index(COMMENT_TOKENS_COLLECTION, BCON_NEW("expires_at", BCON_INT32(1)), BCON_NEW("expireAfterSeconds", BCON_INT32(0)));
    create_index(CAPTCHA_USED_COLLECTION, BCON_NEW("nonce", BCON_INT32(1)), BCON_NEW("unique", BCON_BOOL(true)));
    create_index(CAPTCHA_USED_COLLECTION, BCON_NEW("expires_at", BCON_INT32(1)), BCON_NEW("expireAfterSeconds", BCON_INT32(0)));
}

// ---- Entries ----------------------------------------------------------------------

// Title of an entries document in `lang`: header.title, else "".
static char *entry_title(const bson_t *doc, const char *lang) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, "header") && BSON_ITER_HOLDS_DOCUMENT(&it)) {
        uint32_t len;
        const uint8_t *data;
        bson_iter_document(&it, &len, &data);
        bson_t header;
        if (bson_init_static(&header, data, len)) return resolve_lang_map(&header, "title", lang);
    }
    return strdup("");
}

int cms_comments_blog_entry(const char *link, const char *lang, char id_hex[25], bool *closed, char **title) {
    if (title) *title = NULL;
    mongoc_collection_t *col = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!col) return 0;
    bson_t *filter = BCON_NEW("link", BCON_UTF8(link), "type", BCON_UTF8("blog"), "enabled", BCON_BOOL(true));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, NULL, NULL);
    const bson_t *doc;
    int found = 0;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t it;
        oid_hex(doc, "_id", id_hex);
        *closed = bson_iter_init_find(&it, doc, "comments_closed") && BSON_ITER_HOLDS_BOOL(&it) && bson_iter_bool(&it);
        if (title) *title = entry_title(doc, lang);
        found = id_hex[0] != '\0';
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return found;
}

// The ids of owner_id's blog entries, appended to `array` as {$in: [...]}.
static int owned_entry_ids(const char *owner_id, bson_t *in_array) {
    bson_oid_t owner;
    if (!parse_oid(owner_id, &owner)) return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!col) return -1;
    bson_t *filter = BCON_NEW("type", BCON_UTF8("blog"), "created_by", BCON_OID(&owner));
    bson_t *opts = BCON_NEW("projection", "{", "_id", BCON_INT32(1), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, opts, NULL);
    const bson_t *doc;
    int n = 0;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "_id") && BSON_ITER_HOLDS_OID(&it)) {
            char key[16];
            snprintf(key, sizeof(key), "%d", n++);
            bson_append_oid(in_array, key, -1, bson_iter_oid(&it));
        }
    }
    int rc = mongoc_cursor_error(cursor, NULL) ? -1 : n;
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return rc;
}

// ---- Submission -------------------------------------------------------------------

int cms_comments_use_captcha(const char *nonce, long long expires_at) {
    mongoc_collection_t *col = mongodb_manager_get_collection(CAPTCHA_USED_COLLECTION);
    if (!col) return -1;
    bson_t *doc = BCON_NEW("nonce", BCON_UTF8(nonce), "expires_at", BCON_DATE_TIME((int64_t)expires_at * 1000));
    bson_error_t error;
    int rc = 0;
    if (!mongoc_collection_insert_one(col, doc, NULL, NULL, &error))
        rc = error.code == 11000 ? 1 : -1;   // duplicate key: already used
    bson_destroy(doc);
    mongoc_collection_destroy(col);
    return rc;
}

static long long count_since(const char *field, const bson_value_t *value, int64_t since) {
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) return -1;
    bson_t filter;
    bson_init(&filter);
    bson_append_value(&filter, field, -1, value);
    bson_t created;
    bson_append_document_begin(&filter, "created_at", -1, &created);
    bson_append_date_time(&created, "$gte", -1, since);
    bson_append_document_end(&filter, &created);
    bson_error_t error;
    long long n = mongoc_collection_count_documents(col, &filter, NULL, NULL, NULL, &error);
    bson_destroy(&filter);
    mongoc_collection_destroy(col);
    return n;
}

// The commenter with this email: fills id_hex (25) and returns 1, 0 if none.
static int commenter_by_email(const char *email, char id_hex[25]) {
    id_hex[0] = '\0';
    mongoc_collection_t *col = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!col) return 0;
    bson_t *filter = BCON_NEW("email", BCON_UTF8(email));
    bson_t *opts = email_collation_opts();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, opts, NULL);
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) oid_hex(doc, "_id", id_hex);
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return id_hex[0] != '\0';
}

int cms_comments_within_limits(const char *ip_hash, const char *email) {
    int64_t now = now_ms();
    bson_value_t v = { .value_type = BSON_TYPE_UTF8 };
    v.value.v_utf8.str = (char *)ip_hash;
    v.value.v_utf8.len = (uint32_t)strlen(ip_hash);
    long long by_ip = count_since("ip_hash", &v, now - 3600LL * 1000);
    if (by_ip < 0 || by_ip >= COMMENT_LIMIT_PER_IP_HOUR) return 0;

    char id[25];
    if (commenter_by_email(email, id)) {
        bson_value_t c = { .value_type = BSON_TYPE_OID };
        bson_oid_init_from_string(&c.value.v_oid, id);
        long long by_email = count_since("commenter_id", &c, now - 24LL * 3600 * 1000);
        if (by_email < 0 || by_email >= COMMENT_LIMIT_PER_EMAIL_DAY) return 0;
    }
    return 1;
}

// The literal `text` as an anchored, case-insensitive regex pattern.
static char *exact_pattern(const char *text) {
    char *out = malloc(strlen(text) * 2 + 3);
    if (!out) return NULL;
    char *w = out;
    *w++ = '^';
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 0x80 && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == ' '))
            *w++ = '\\';
        *w++ = (char)*p;
    }
    *w++ = '$';
    *w = '\0';
    return out;
}

// 1 if some user other than `email`'s has this name: a commenter's
// name_key, or a team member's name or username. Names compare without case.
static int name_taken(mongoc_collection_t *users, const char *name, const char *name_key, const char *email) {
    char *pattern = exact_pattern(name);
    if (!pattern) return 1;
    bson_t *filter = BCON_NEW("$or", "[",
        "{", "name_key", BCON_UTF8(name_key), "}",
        "{", "name", BCON_REGEX(pattern, "i"), "}",
        "{", "username", BCON_REGEX(pattern, "i"), "}",
    "]");
    bson_t *opts = BCON_NEW("projection", "{", "email", BCON_INT32(1), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(users, filter, opts, NULL);
    const bson_t *doc;
    int taken = 0;
    while (!taken && mongoc_cursor_next(cursor, &doc)) {
        char *other = dup_utf8(doc, "email");
        taken = !other || strcasecmp(other, email) != 0;
        free(other);
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(filter);
    free(pattern);
    return taken;
}

// The user behind s->email, created as a "commenter" if there is none.
// A team member commenting with their own address stays as they are (role
// and name untouched). Fills id_hex, *verified and *blocked; returns 0 or an
// error result.
static CommentSubmitResult commenter_for(const CommentSubmission *s, char id_hex[25], bool *verified, bool *blocked) {
    *verified = *blocked = false;
    id_hex[0] = '\0';
    char name_key[COMMENT_NAME_MAX * 2 + 1];
    snprintf(name_key, sizeof(name_key), "%s", s->name);
    fold_case(name_key);

    mongoc_collection_t *users = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!users) return COMMENT_SUBMIT_ERROR;
    CommentSubmitResult rc = COMMENT_SUBMIT_ERROR;
    bson_error_t error;

    for (int attempt = 0; attempt < 2 && rc == COMMENT_SUBMIT_ERROR; attempt++) {
        bson_t *by_email = BCON_NEW("email", BCON_UTF8(s->email));
        bson_t *opts = email_collation_opts();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(users, by_email, opts, NULL);
        const bson_t *doc;
        int found = mongoc_cursor_next(cursor, &doc);
        char *role = found ? dup_utf8(doc, "role") : NULL;
        if (found) {
            bson_iter_t it;
            oid_hex(doc, "_id", id_hex);
            *verified = bson_iter_init_find(&it, doc, "email_verified_at") && BSON_ITER_HOLDS_DATE_TIME(&it);
            *blocked = bson_iter_init_find(&it, doc, "blocked") && BSON_ITER_HOLDS_BOOL(&it) && bson_iter_bool(&it);
        }
        mongoc_cursor_destroy(cursor);
        bson_destroy(opts);
        bson_destroy(by_email);

        if (name_taken(users, s->name, name_key, s->email)) {
            rc = COMMENT_SUBMIT_NAME_TAKEN;
        } else if (found) {
            rc = 0;
            if (role && strcmp(role, "commenter") == 0) {
                bson_oid_t oid;
                bson_oid_init_from_string(&oid, id_hex);
                bson_t *f = BCON_NEW("_id", BCON_OID(&oid));
                bson_t *u = BCON_NEW("$set", "{", "name", BCON_UTF8(s->name), "name_key", BCON_UTF8(name_key), "}");
                if (!mongoc_collection_update_one(users, f, u, NULL, NULL, &error))
                    rc = error.code == 11000 ? COMMENT_SUBMIT_NAME_TAKEN : COMMENT_SUBMIT_ERROR;
                bson_destroy(f);
                bson_destroy(u);
            }
        } else {
            char username[USERNAME_BUF_SIZE] = "";
            cms_new_username_for_email(s->email, username, sizeof(username));
            bson_oid_t oid;
            bson_oid_init(&oid, NULL);
            bson_t *u = BCON_NEW("_id", BCON_OID(&oid), "email", BCON_UTF8(s->email), "name", BCON_UTF8(s->name),
                                 "name_key", BCON_UTF8(name_key), "username", BCON_UTF8(username),
                                 "role", BCON_UTF8("commenter"), "blocked", BCON_BOOL(false),
                                 "created_at", BCON_DATE_TIME(now_ms()));
            if (mongoc_collection_insert_one(users, u, NULL, NULL, &error)) {
                bson_oid_to_string(&oid, id_hex);
                rc = 0;
            } else if (error.code != 11000) {
                LOG_ERROR("cms_comments: new commenter: %s", error.message);
                attempt = 2;
            }
            // A duplicate key: someone took the email or the name meanwhile -
            // the second pass reads what is there now.
            bson_destroy(u);
        }
        free(role);
    }
    mongoc_collection_destroy(users);
    return rc;
}

CommentSubmitResult cms_comments_submit(const CommentSubmission *s, char token_out[COMMENT_TOKEN_HEX + 1],
                                        char commenter_id_out[25]) {
    token_out[0] = '\0';
    commenter_id_out[0] = '\0';
    bson_oid_t entry_oid;
    if (!parse_oid(s->entry_id, &entry_oid)) return COMMENT_SUBMIT_ERROR;

    char cid[25];
    bool verified, blocked;
    CommentSubmitResult rc = commenter_for(s, cid, &verified, &blocked);
    if (rc != 0) return rc;
    if (blocked) return COMMENT_SUBMIT_BLOCKED;

    // Straight to the moderators when this browser proved this email before.
    int remembered = verified && s->remembered_id && strcmp(s->remembered_id, cid) == 0;
    bson_oid_t commenter_oid, comment_oid;
    bson_oid_init_from_string(&commenter_oid, cid);
    bson_oid_init(&comment_oid, NULL);
    int64_t now = now_ms();

    bson_t *doc = BCON_NEW("_id", BCON_OID(&comment_oid), "entry_id", BCON_OID(&entry_oid),
                           "commenter_id", BCON_OID(&commenter_oid), "name", BCON_UTF8(s->name),
                           "text", BCON_UTF8(s->text), "status", BCON_UTF8(remembered ? "pending" : "unverified"),
                           "created_at", BCON_DATE_TIME(now), "ip_hash", BCON_UTF8(s->ip_hash));
    if (remembered) BSON_APPEND_DATE_TIME(doc, "verified_at", now);
    else BSON_APPEND_DATE_TIME(doc, "expires_at", now + (int64_t)COMMENT_VERIFY_HOURS * 3600 * 1000);

    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    bson_error_t error;
    int stored = col && mongoc_collection_insert_one(col, doc, NULL, NULL, &error);
    if (col && !stored) LOG_ERROR("cms_comments: insert: %s", error.message);
    bson_destroy(doc);
    if (col) mongoc_collection_destroy(col);
    if (!stored) return COMMENT_SUBMIT_ERROR;
    memcpy(commenter_id_out, cid, 25);
    if (remembered) return COMMENT_SUBMIT_PENDING;

    // The token itself goes only into the email; the database keeps its hash.
    unsigned char raw[COMMENT_TOKEN_HEX / 2];
    randombytes_buf(raw, sizeof(raw));
    sodium_bin2hex(token_out, COMMENT_TOKEN_HEX + 1, raw, sizeof(raw));
    char hash[65];
    token_hash(token_out, hash);
    mongoc_collection_t *tokens = mongodb_manager_get_collection(COMMENT_TOKENS_COLLECTION);
    bson_t *tdoc = BCON_NEW("token_hash", BCON_UTF8(hash), "comment_id", BCON_OID(&comment_oid),
                            "expires_at", BCON_DATE_TIME(now + (int64_t)COMMENT_VERIFY_HOURS * 3600 * 1000));
    int ok = tokens && mongoc_collection_insert_one(tokens, tdoc, NULL, NULL, &error);
    bson_destroy(tdoc);
    if (tokens) mongoc_collection_destroy(tokens);
    sodium_memzero(raw, sizeof(raw));
    if (!ok) { token_out[0] = '\0'; return COMMENT_SUBMIT_ERROR; }
    return COMMENT_SUBMIT_VERIFY;
}

// ---- Verification -------------------------------------------------------------------

// The comment id behind a live token; 1 if found.
static int token_comment(const char *token, bson_oid_t *comment_oid, char hash_out[65]) {
    if (!token || strlen(token) != COMMENT_TOKEN_HEX) return 0;
    for (const char *p = token; *p; p++) if (!strchr("0123456789abcdef", *p)) return 0;
    token_hash(token, hash_out);
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENT_TOKENS_COLLECTION);
    if (!col) return 0;
    bson_t *filter = BCON_NEW("token_hash", BCON_UTF8(hash_out), "expires_at", "{", "$gt", BCON_DATE_TIME(now_ms()), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, NULL, NULL);
    const bson_t *doc;
    int found = 0;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "comment_id") && BSON_ITER_HOLDS_OID(&it)) {
            bson_oid_copy(bson_iter_oid(&it), comment_oid);
            found = 1;
        }
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return found;
}

int cms_comments_token_valid(const char *token) {
    bson_oid_t oid;
    char hash[65];
    return token_comment(token, &oid, hash);
}

int cms_comments_verify(const char *token, char commenter_id_out[25], char *entry_link_out, size_t link_size) {
    commenter_id_out[0] = '\0';
    if (link_size) entry_link_out[0] = '\0';
    bson_oid_t comment_oid;
    char hash[65];
    if (!token_comment(token, &comment_oid, hash)) return 0;

    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) return 0;
    int64_t now = now_ms();
    bson_t *filter = BCON_NEW("_id", BCON_OID(&comment_oid), "status", BCON_UTF8("unverified"));
    bson_t *update = BCON_NEW("$set", "{", "status", BCON_UTF8("pending"), "verified_at", BCON_DATE_TIME(now), "}",
                              "$unset", "{", "expires_at", BCON_UTF8(""), "}");
    bson_t reply;
    bson_error_t error;
    int done = 0;
    bson_oid_t entry_oid, commenter_oid;
    int have_ids = 0;
    if (mongoc_collection_find_and_modify(col, filter, NULL, update, NULL, false, false, true, &reply, &error)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, &reply, "value") && BSON_ITER_HOLDS_DOCUMENT(&it)) {
            uint32_t len;
            const uint8_t *data;
            bson_iter_document(&it, &len, &data);
            bson_t c;
            bson_init_static(&c, data, len);
            bson_iter_t f;
            if (bson_iter_init_find(&f, &c, "entry_id") && BSON_ITER_HOLDS_OID(&f)) bson_oid_copy(bson_iter_oid(&f), &entry_oid), have_ids++;
            if (bson_iter_init_find(&f, &c, "commenter_id") && BSON_ITER_HOLDS_OID(&f)) bson_oid_copy(bson_iter_oid(&f), &commenter_oid), have_ids++;
            done = 1;
        }
        bson_destroy(&reply);
    }
    bson_destroy(update);
    bson_destroy(filter);
    mongoc_collection_destroy(col);

    // The token is spent either way.
    mongoc_collection_t *tokens = mongodb_manager_get_collection(COMMENT_TOKENS_COLLECTION);
    if (tokens) {
        bson_t *t = BCON_NEW("token_hash", BCON_UTF8(hash));
        mongoc_collection_delete_one(tokens, t, NULL, NULL, NULL);
        bson_destroy(t);
        mongoc_collection_destroy(tokens);
    }
    if (!done || have_ids != 2) return 0;

    mongoc_collection_t *commenters = mongodb_manager_get_collection(USERS_COLLECTION);
    if (commenters) {
        bson_t *cf = BCON_NEW("_id", BCON_OID(&commenter_oid));
        bson_t *cu = BCON_NEW("$set", "{", "email_verified_at", BCON_DATE_TIME(now), "}");
        mongoc_collection_update_one(commenters, cf, cu, NULL, NULL, NULL);
        bson_destroy(cf);
        bson_destroy(cu);
        mongoc_collection_destroy(commenters);
    }
    bson_oid_to_string(&commenter_oid, commenter_id_out);

    mongoc_collection_t *entries = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (entries && link_size) {
        bson_t *ef = BCON_NEW("_id", BCON_OID(&entry_oid));
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(entries, ef, NULL, NULL);
        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc)) {
            char *link = dup_utf8(doc, "link");
            if (link) snprintf(entry_link_out, link_size, "%s", link);
            free(link);
        }
        mongoc_cursor_destroy(cursor);
        bson_destroy(ef);
    }
    if (entries) mongoc_collection_destroy(entries);
    return 1;
}

int cms_comments_commenter_verified(const char *commenter_id) {
    bson_oid_t oid;
    if (!parse_oid(commenter_id, &oid)) return 0;
    mongoc_collection_t *col = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!col) return 0;
    bson_t *filter = BCON_NEW("_id", BCON_OID(&oid), "blocked", "{", "$ne", BCON_BOOL(true), "}",
                              "email_verified_at", "{", "$exists", BCON_BOOL(true), "}");
    long long n = mongoc_collection_count_documents(col, filter, NULL, NULL, NULL, NULL);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return n > 0;
}

// ---- Lists --------------------------------------------------------------------------

int cms_comments_approved(const char *entry_id, CmsComment **out) {
    *out = NULL;
    bson_oid_t oid;
    if (!parse_oid(entry_id, &oid)) return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) return -1;
    bson_t *filter = BCON_NEW("entry_id", BCON_OID(&oid), "status", BCON_UTF8("approved"));
    bson_t *opts = BCON_NEW("sort", "{", "created_at", BCON_INT32(1), "}", "limit", BCON_INT64(APPROVED_LIMIT));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, opts, NULL);
    CmsComment *list = calloc(APPROVED_LIMIT, sizeof(CmsComment));
    int n = list ? 0 : -1;
    const bson_t *doc;
    while (list && n < APPROVED_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        oid_hex(doc, "_id", list[n].id);
        list[n].name = dup_utf8(doc, "name");
        list[n].text = dup_utf8(doc, "text");
        date_str(doc, "created_at", list[n].date);
        n++;
    }
    if (mongoc_cursor_error(cursor, NULL)) { cms_comments_free(list, n); list = NULL; n = -1; }
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    *out = list;
    return n;
}

void cms_comments_free(CmsComment *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) { free(list[i].name); free(list[i].text); }
    free(list);
}

// Appends {entry_id: {$in: owner's entries}} to `filter`; -1 on a DB error.
static int scope_to_owner(bson_t *filter, const char *owner_id) {
    if (!owner_id) return 0;
    bson_t entry, in;
    bson_append_document_begin(filter, "entry_id", -1, &entry);
    bson_append_array_begin(&entry, "$in", -1, &in);
    int n = owned_entry_ids(owner_id, &in);
    bson_append_array_end(&entry, &in);
    bson_append_document_end(filter, &entry);
    return n < 0 ? -1 : 0;
}

// The confirmation email for a comment sent at `since` (ms) to `email`: the
// newest mail_queue message to that address from then on.
static void mail_state(const char *email, int64_t since, CmsCommentAdmin *c) {
    mongoc_collection_t *col = mongodb_manager_get_collection("mail_queue");
    if (!col) return;
    bson_t *filter = BCON_NEW("to", BCON_UTF8(email), "created_at", "{", "$gte", BCON_DATE_TIME(since - 60000), "}");
    bson_t *opts = BCON_NEW("sort", "{", "created_at", BCON_INT32(-1), "}", "limit", BCON_INT64(1));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, opts, NULL);
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        char *st = dup_utf8(doc, "status");
        snprintf(c->mail_status, sizeof(c->mail_status), "%s", st && strcmp(st, "sending") == 0 ? "queued" : (st ? st : ""));
        free(st);
        c->mail_error = dup_utf8(doc, "last_error");
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
}

int cms_comments_admin_list(const char *status, const char *owner_id, const char *lang, CmsCommentAdmin **out) {
    *out = NULL;
    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "status", status);
    if (scope_to_owner(&filter, owner_id) != 0) { bson_destroy(&filter); return -1; }

    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) { bson_destroy(&filter); return -1; }
    bson_t *opts = BCON_NEW("sort", "{", "created_at", BCON_INT32(-1), "}", "limit", BCON_INT64(COMMENT_ADMIN_LIST_LIMIT));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, &filter, opts, NULL);
    CmsCommentAdmin *list = calloc(COMMENT_ADMIN_LIST_LIMIT, sizeof(CmsCommentAdmin));
    bson_oid_t *entry_ids = calloc(COMMENT_ADMIN_LIST_LIMIT, sizeof(bson_oid_t));
    int64_t *created_at = calloc(COMMENT_ADMIN_LIST_LIMIT, sizeof(int64_t));
    int n = (list && entry_ids && created_at) ? 0 : -1;
    const bson_t *doc;
    while (n >= 0 && n < COMMENT_ADMIN_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        CmsCommentAdmin *c = &list[n];
        bson_iter_t it;
        oid_hex(doc, "_id", c->id);
        oid_hex(doc, "commenter_id", c->commenter_id);
        if (bson_iter_init_find(&it, doc, "entry_id") && BSON_ITER_HOLDS_OID(&it)) bson_oid_copy(bson_iter_oid(&it), &entry_ids[n]);
        c->name = dup_utf8(doc, "name");
        c->text = dup_utf8(doc, "text");
        char *st = dup_utf8(doc, "status");
        snprintf(c->status, sizeof(c->status), "%s", st ? st : "");
        free(st);
        date_str(doc, "created_at", c->date);
        if (bson_iter_init_find(&it, doc, "created_at") && BSON_ITER_HOLDS_DATE_TIME(&it))
            created_at[n] = bson_iter_date_time(&it);
        n++;
    }
    if (n >= 0 && mongoc_cursor_error(cursor, NULL)) n = -1;
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(&filter);
    mongoc_collection_destroy(col);

    // Entry link/title and commenter email/blocked, one lookup each (a list
    // is at most COMMENT_ADMIN_LIST_LIMIT long).
    mongoc_collection_t *entries = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    mongoc_collection_t *commenters = mongodb_manager_get_collection(USERS_COLLECTION);
    for (int i = 0; i < n && entries && commenters; i++) {
        bson_t *ef = BCON_NEW("_id", BCON_OID(&entry_ids[i]));
        mongoc_cursor_t *ec = mongoc_collection_find_with_opts(entries, ef, NULL, NULL);
        if (mongoc_cursor_next(ec, &doc)) {
            list[i].entry_link = dup_utf8(doc, "link");
            list[i].entry_title = entry_title(doc, lang);
        }
        mongoc_cursor_destroy(ec);
        bson_destroy(ef);

        bson_oid_t cid;
        if (parse_oid(list[i].commenter_id, &cid)) {
            bson_t *cf = BCON_NEW("_id", BCON_OID(&cid));
            mongoc_cursor_t *cc = mongoc_collection_find_with_opts(commenters, cf, NULL, NULL);
            if (mongoc_cursor_next(cc, &doc)) {
                bson_iter_t it;
                list[i].email = dup_utf8(doc, "email");
                list[i].commenter_blocked = bson_iter_init_find(&it, doc, "blocked") && BSON_ITER_HOLDS_BOOL(&it) && bson_iter_bool(&it);
            }
            mongoc_cursor_destroy(cc);
            bson_destroy(cf);
        }
        if (strcmp(list[i].status, "unverified") == 0 && list[i].email && list[i].email[0])
            mail_state(list[i].email, created_at[i], &list[i]);
        if (!list[i].entry_link) list[i].entry_link = strdup("");
        if (!list[i].entry_title) list[i].entry_title = strdup("");
        if (!list[i].email) list[i].email = strdup("");
    }
    if (entries) mongoc_collection_destroy(entries);
    if (commenters) mongoc_collection_destroy(commenters);
    free(entry_ids);
    free(created_at);

    if (n < 0) { cms_comments_admin_free(list, 0); return -1; }
    *out = list;
    return n;
}

void cms_comments_admin_free(CmsCommentAdmin *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].entry_link); free(list[i].entry_title); free(list[i].name);
        free(list[i].email); free(list[i].text); free(list[i].mail_error);
    }
    free(list);
}

long cms_comments_pending_count(const char *owner_id) {
    return cms_comments_count("pending", owner_id);
}

long cms_comments_count(const char *status, const char *owner_id) {
    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "status", status);
    if (scope_to_owner(&filter, owner_id) != 0) { bson_destroy(&filter); return -1; }
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    long n = col ? (long)mongoc_collection_count_documents(col, &filter, NULL, NULL, NULL, NULL) : -1;
    if (col) mongoc_collection_destroy(col);
    bson_destroy(&filter);
    return n;
}

int cms_comments_can_moderate(const char *comment_id, const char *owner_id) {
    bson_oid_t oid;
    if (!parse_oid(comment_id, &oid)) return 0;
    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_OID(&filter, "_id", &oid);
    if (scope_to_owner(&filter, owner_id) != 0) { bson_destroy(&filter); return 0; }
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    long long n = col ? mongoc_collection_count_documents(col, &filter, NULL, NULL, NULL, NULL) : 0;
    if (col) mongoc_collection_destroy(col);
    bson_destroy(&filter);
    return n > 0;
}

int cms_comments_set_status(const char *comment_id, const char *status, const char *moderator_id) {
    bson_oid_t oid, mod;
    if (!parse_oid(comment_id, &oid) || !parse_oid(moderator_id, &mod) ||
        (strcmp(status, "approved") != 0 && strcmp(status, "rejected") != 0))
        return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) return -1;
    bson_t *filter = BCON_NEW("_id", BCON_OID(&oid));
    bson_t *update = BCON_NEW("$set", "{", "status", BCON_UTF8(status), "moderated_by", BCON_OID(&mod),
                              "moderated_at", BCON_DATE_TIME(now_ms()), "}",
                              "$unset", "{", "expires_at", BCON_UTF8(""), "}");
    bson_t reply;
    int rc = -1;
    if (mongoc_collection_update_one(col, filter, update, NULL, &reply, NULL)) {
        bson_iter_t it;
        rc = bson_iter_init_find(&it, &reply, "matchedCount") && bson_iter_as_int64(&it) > 0 ? 0 : -1;
    }
    bson_destroy(&reply);
    bson_destroy(update);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return rc;
}

int cms_comments_delete(const char *comment_id) {
    bson_oid_t oid;
    if (!parse_oid(comment_id, &oid)) return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!col) return -1;
    bson_t *filter = BCON_NEW("_id", BCON_OID(&oid));
    int rc = mongoc_collection_delete_one(col, filter, NULL, NULL, NULL) ? 0 : -1;
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    return rc;
}

int cms_comments_block(const char *commenter_id, bool block, bool delete_all) {
    bson_oid_t oid;
    if (!parse_oid(commenter_id, &oid)) return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(USERS_COLLECTION);
    if (!col) return -1;
    bson_t *filter = BCON_NEW("_id", BCON_OID(&oid));
    bson_t *update = BCON_NEW("$set", "{", "blocked", BCON_BOOL(block), "}");
    int rc = mongoc_collection_update_one(col, filter, update, NULL, NULL, NULL) ? 0 : -1;
    bson_destroy(update);
    mongoc_collection_destroy(col);
    if (rc == 0 && delete_all) {
        mongoc_collection_t *comments = mongodb_manager_get_collection(COMMENTS_COLLECTION);
        bson_t *cf = BCON_NEW("commenter_id", BCON_OID(&oid));
        rc = comments && mongoc_collection_delete_many(comments, cf, NULL, NULL, NULL) ? 0 : -1;
        bson_destroy(cf);
        if (comments) mongoc_collection_destroy(comments);
    }
    bson_destroy(filter);
    return rc;
}

int cms_comments_renew_token(const char *comment_id, const char *lang, char token_out[COMMENT_TOKEN_HEX + 1],
                             char **email, char **name, char **title_out) {
    *email = *name = *title_out = NULL;
    token_out[0] = '\0';
    bson_oid_t oid;
    if (!parse_oid(comment_id, &oid)) return -1;
    mongoc_collection_t *comments = mongodb_manager_get_collection(COMMENTS_COLLECTION);
    if (!comments) return -1;

    // Still unverified: 24 more hours from now.
    int64_t until = now_ms() + (int64_t)COMMENT_VERIFY_HOURS * 3600 * 1000;
    bson_t *filter = BCON_NEW("_id", BCON_OID(&oid), "status", BCON_UTF8("unverified"));
    bson_t *update = BCON_NEW("$set", "{", "expires_at", BCON_DATE_TIME(until), "}");
    bson_t reply;
    int rc = -1;
    bson_oid_t entry_id, commenter_id;
    if (mongoc_collection_find_and_modify(comments, filter, NULL, update, NULL, false, false, true, &reply, NULL)) {
        bson_iter_t it, f;
        if (bson_iter_init_find(&it, &reply, "value") && BSON_ITER_HOLDS_DOCUMENT(&it)) {
            uint32_t len;
            const uint8_t *data;
            bson_iter_document(&it, &len, &data);
            bson_t doc;
            bson_init_static(&doc, data, len);
            int ids = 0;
            if (bson_iter_init_find(&f, &doc, "entry_id") && BSON_ITER_HOLDS_OID(&f)) { bson_oid_copy(bson_iter_oid(&f), &entry_id); ids++; }
            if (bson_iter_init_find(&f, &doc, "commenter_id") && BSON_ITER_HOLDS_OID(&f)) { bson_oid_copy(bson_iter_oid(&f), &commenter_id); ids++; }
            *name = dup_utf8(&doc, "name");
            rc = ids == 2 ? 0 : -1;
        }
        bson_destroy(&reply);
    }
    bson_destroy(update);
    bson_destroy(filter);
    mongoc_collection_destroy(comments);
    if (rc != 0) return -1;

    mongoc_collection_t *users = mongodb_manager_get_collection(USERS_COLLECTION);
    mongoc_collection_t *entries = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    mongoc_collection_t *tokens = mongodb_manager_get_collection(COMMENT_TOKENS_COLLECTION);
    const bson_t *doc;
    if (users) {
        bson_t *uf = BCON_NEW("_id", BCON_OID(&commenter_id));
        mongoc_cursor_t *uc = mongoc_collection_find_with_opts(users, uf, NULL, NULL);
        if (mongoc_cursor_next(uc, &doc)) *email = dup_utf8(doc, "email");
        mongoc_cursor_destroy(uc);
        bson_destroy(uf);
    }
    if (entries) {
        bson_t *ef = BCON_NEW("_id", BCON_OID(&entry_id));
        mongoc_cursor_t *ec = mongoc_collection_find_with_opts(entries, ef, NULL, NULL);
        if (mongoc_cursor_next(ec, &doc)) *title_out = entry_title(doc, lang);
        mongoc_cursor_destroy(ec);
        bson_destroy(ef);
    }
    if (tokens && *email && (*email)[0]) {
        // The earlier links stop working: only the newest email confirms.
        bson_t *old = BCON_NEW("comment_id", BCON_OID(&oid));
        mongoc_collection_delete_many(tokens, old, NULL, NULL, NULL);
        bson_destroy(old);
        unsigned char raw[COMMENT_TOKEN_HEX / 2];
        randombytes_buf(raw, sizeof(raw));
        sodium_bin2hex(token_out, COMMENT_TOKEN_HEX + 1, raw, sizeof(raw));
        sodium_memzero(raw, sizeof(raw));
        char hash[65];
        token_hash(token_out, hash);
        bson_t *t = BCON_NEW("token_hash", BCON_UTF8(hash), "comment_id", BCON_OID(&oid), "expires_at", BCON_DATE_TIME(until));
        if (!mongoc_collection_insert_one(tokens, t, NULL, NULL, NULL)) token_out[0] = '\0';
        bson_destroy(t);
    }
    if (users) mongoc_collection_destroy(users);
    if (entries) mongoc_collection_destroy(entries);
    if (tokens) mongoc_collection_destroy(tokens);
    if (!token_out[0] || !*email || !*name || !*title_out) {
        free(*email); free(*name); free(*title_out);
        *email = *name = *title_out = NULL;
        return -1;
    }
    return 0;
}
