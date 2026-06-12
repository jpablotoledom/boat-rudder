#ifndef USERS_ADMIN_H
#define USERS_ADMIN_H

// /dashboard/users - table of every `users` document (sorted by email), with
// a "Role" column ("Administrador"/"Autor") and a "New user" link.
// `error_message` is shown above the table if non-NULL/non-empty (e.g. the
// self-delete / last-admin-removal messages from the router). Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
char *users_admin_list(int epoch, const char *error_message);

// /dashboard/users/new (id == "") and /dashboard/users/<id>/edit (id == hex
// ObjectId). `email`/`role` are the user's current email/role ("" / "author"
// for a new user). The password field is always rendered empty - on edit, a
// blank submission leaves the stored password unchanged. `error_message` is
// shown above the form if non-NULL/non-empty (e.g. after a failed save).
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *users_admin_form(int epoch, const char *id, const char *email, const char *role,
                        const char *error_message);

#endif // USERS_ADMIN_H
