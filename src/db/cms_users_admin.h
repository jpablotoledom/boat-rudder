#ifndef CMS_USERS_ADMIN_H
#define CMS_USERS_ADMIN_H

#include <stddef.h>

// Maximum number of "users" documents returned by cms_get_users() for the
// /dashboard/users listing.
#define USERS_LIST_LIMIT 100

// users.role: "admin", "author", "viewer" (read-only admin), "moderator"
// (only reviews and approves/rejects blog comments) or "commenter" (a blog
// commenter: no password, can't sign in - see cms_comments.h). + NUL.
#define USER_ROLE_BUF_SIZE 16

// users.username: the user's permanent handle, set when the account is
// created and never changed - it names the user's media folder,
// html/content/posts/<username>/, which every stored media URL contains, so
// changing the email no longer moves (and breaks) it. New usernames are
// 3-32 characters of [a-z0-9._-], not starting with '.'. A username set by
// the startup migration (cms_users_ensure_usernames()) may keep a legacy
// folder's capitals.
#define USERNAME_MIN_LEN  3
#define USERNAME_MAX_LEN  32
#define USERNAME_BUF_SIZE 64 // > USERNAME_MAX_LEN: legacy folder names can be longer

// cms_create_user()'s errors besides -1 (generic: email taken, bad role, DB).
#define CMS_USER_USERNAME_INVALID -2
#define CMS_USER_USERNAME_TAKEN   -3

// One "users" document, for the /dashboard/users admin listing.
typedef struct {
    char *id;    // hex ObjectId (24 chars + NUL)
    char *name;
    char *username;
    char *email;
    char *role;  // see USER_ROLE_BUF_SIZE (missing field -> "admin")
} CmsUserAdminItem;

// db.users.find().sort({email: 1}).limit(USERS_LIST_LIMIT): the site's team
// (every role but "commenter") or, with `commenters`, the blog commenters
// only. On success, *out points to a malloc'd array of *out_count items
// (possibly 0) that must be passed to cms_users_admin_free(). On a DB error
// or if mongodb is not ready, *out = NULL and *out_count = 0.
void cms_get_users(int commenters, CmsUserAdminItem **out, size_t *out_count);

// 1 if `role` is one of the roles above.
int cms_user_role_valid(const char *role);

// 1 if the user has a password (can sign in), 0 if not or unknown.
int cms_user_has_password(const char *id_hex);

// A fresh username for a new account with `email` (as the startup
// migration would make it: from the address, made unique). `out` >=
// USERNAME_BUF_SIZE. Returns 0, -1 if MongoDB is not ready.
int cms_new_username_for_email(const char *email, char *out, size_t out_size);

// Frees every item's fields and the array itself. Safe to call with
// items == NULL.
void cms_users_admin_free(CmsUserAdminItem *items, size_t count);

// db.users.findOne({_id: id_hex}) -> email/role for the edit form. Returns 0
// on success, -1 if id_hex is invalid, the document doesn't exist, or
// mongodb is not ready.
int cms_get_user_values(const char *id_hex, char *out_email, size_t out_email_size,
                         char *out_role, size_t out_role_size);

// 1 if `username` is acceptable for a new account (see USERNAME_MIN_LEN).
int cms_username_valid(const char *username);

// db.users.findOne({_id: id_hex}) -> role only ("admin" if the field is
// absent). Returns 0 on success, -1 if id_hex is invalid, the document
// doesn't exist, or mongodb is not ready - callers should treat -1 as
// "admin" too, since require_dashboard_session() already proved the session
// is valid.
int cms_get_user_role(const char *id_hex, char *out_role, size_t out_role_size);

// db.users.countDocuments({$or: [{role: "admin"}, {role: {$exists: false}}]}).
// Used to block removing/demoting the last admin. Returns -1 on a DB error
// or if mongodb is not ready.
int cms_count_admins(void);

// Hashes `password` (crypto_pwhash_str, Argon2id) and inserts
// {name, username, email, password, role}. Returns 0 on success,
// CMS_USER_USERNAME_INVALID / CMS_USER_USERNAME_TAKEN for the username, -1
// if email already exists, role is not "admin"/"author"/"viewer", or on a DB
// error.
int cms_create_user(const char *name, const char *username, const char *email,
                     const char *password, const char *role);

// db.users.updateOne({_id: id_hex}, {$set: {name, email, role[, password]}}).
// Never the username: it is fixed at creation.
// `new_password`, if non-empty, is hashed and included in the update.
// Returns 0 on success, -1 if id_hex is invalid/not found, email belongs to
// another user, role is not "admin"/"author"/"viewer", or on a DB error.
int cms_update_user(const char *id_hex, const char *name, const char *email, const char *role,
                     const char *new_password);

// db.users.findOne({_id: id_hex}) -> malloc'd name string, or "" on not found.
// Caller must free the returned string.
char *cms_get_user_name_by_id(const char *id_hex);

// db.users.deleteOne({_id: id_hex}). Returns 0 on success, -1 if id_hex is
// invalid, the document doesn't exist, or on a DB error. Self-delete /
// last-admin protection is enforced by the router, not here.
int cms_delete_user(const char *id_hex);

// users.username for the user with the given id - the media folder name
// (html/content/posts/<username>/). A user the startup migration has not
// reached yet falls back to the email-derived name it used to be
// (cms_username_from_email()). Returns 0 on success, -1 on error.
int cms_get_username_by_id(const char *id_hex, char *out, size_t out_size);

// The pre-username media folder name: the email's part before '@', every
// character outside [A-Za-z0-9._-] turned into '-'. Only for users that
// have no users.username yet.
void cms_username_from_email(const char *email, char *out, size_t out_size);

// Startup migration, after mongodb_manager_init(): gives every user without
// a username one - the folder under html/content/posts/ that already holds
// their media directories if one does (so stored media URLs keep working
// whatever the email is now), else the email-derived name, lowercased,
// with a "-2", "-3"... suffix if taken - then ensures a unique index on
// users.username. Users are taken oldest first. No-op without MongoDB.
void cms_users_ensure_usernames(void);

#endif // CMS_USERS_ADMIN_H
