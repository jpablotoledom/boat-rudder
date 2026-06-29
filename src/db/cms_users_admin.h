#ifndef CMS_USERS_ADMIN_H
#define CMS_USERS_ADMIN_H

#include <stddef.h>

// Maximum number of "users" documents returned by cms_get_users() for the
// /dashboard/users listing.
#define USERS_LIST_LIMIT 100

// "admin" | "author" + NUL.
#define USER_ROLE_BUF_SIZE 16

// One "users" document, for the /dashboard/users admin listing.
typedef struct {
    char *id;    // hex ObjectId (24 chars + NUL)
    char *email;
    char *role;  // "admin" | "author" (missing field -> "admin")
} CmsUserAdminItem;

// db.users.find().sort({email: 1}).limit(USERS_LIST_LIMIT). On success, *out
// points to a malloc'd array of *out_count items (possibly 0) that must be
// passed to cms_users_admin_free(). On a DB error or if mongodb is not
// ready, *out = NULL and *out_count = 0.
void cms_get_users(CmsUserAdminItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with
// items == NULL.
void cms_users_admin_free(CmsUserAdminItem *items, size_t count);

// db.users.findOne({_id: id_hex}) -> email/role for the edit form. Returns 0
// on success, -1 if id_hex is invalid, the document doesn't exist, or
// mongodb is not ready.
int cms_get_user_values(const char *id_hex, char *out_email, size_t out_email_size,
                         char *out_role, size_t out_role_size);

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
// {email, password, role}. Returns 0 on success, -1 if email already exists,
// role is not "admin"/"author", or on a DB error.
int cms_create_user(const char *email, const char *password, const char *role);

// db.users.updateOne({_id: id_hex}, {$set: {email, role[, password]}}).
// `new_password`, if non-empty, is hashed and included in the update.
// Returns 0 on success, -1 if id_hex is invalid/not found, email belongs to
// another user, role is not "admin"/"author", or on a DB error.
int cms_update_user(const char *id_hex, const char *email, const char *role,
                     const char *new_password);

// db.users.deleteOne({_id: id_hex}). Returns 0 on success, -1 if id_hex is
// invalid, the document doesn't exist, or on a DB error. Self-delete /
// last-admin protection is enforced by the router, not here.
int cms_delete_user(const char *id_hex);

// Gets the email-derived username (part before '@') for the user with the
// given id. Used for media directory paths. Returns 0 on success, -1 on error.
int cms_get_username_by_id(const char *id_hex, char *out, size_t out_size);

#endif // CMS_USERS_ADMIN_H
