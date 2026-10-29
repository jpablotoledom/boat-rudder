#ifndef USERS_ADMIN_H
#define USERS_ADMIN_H

// /dashboard/users - table of every `users` document (sorted by email), with
// "Username", "Email" and "Role" columns and a "New user" link.
// `error_message` is shown above the table if non-NULL/non-empty (e.g. the
// self-delete / last-admin-removal messages from the router). Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
// `commenters`: the blog commenters (role "commenter") instead of the team.
char *users_admin_list(int epoch, const char *error_message, int commenters);

// /dashboard/users/new (id == "") and /dashboard/users/<id>/edit (id == hex
// ObjectId). `name`/`username`/`email`/`role` are the user's current values
// ("" / "" / "" / "author" for a new user); the username is editable only on
// a new user. The password field is always rendered empty - on
// edit, a blank submission leaves the stored password unchanged. `error_message`
// is shown above the form if non-NULL/non-empty. Returns a malloc'd string, or
// NULL on a missing template / allocation failure.
char *users_admin_form(int epoch, const char *id, const char *name, const char *username,
                        const char *email, const char *role, const char *error_message);

#endif // USERS_ADMIN_H
