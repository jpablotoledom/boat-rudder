#ifndef LANGUAGES_ADMIN_H
#define LANGUAGES_ADMIN_H

// /dashboard/languages - table of active content languages (from
// db.languages), each with "make default"/"remove" actions (omitted for the
// current default), plus an "add language" <select> built from
// LANGUAGE_CATALOG entries not yet active. `error_message` is shown above
// the table if non-NULL/non-empty (e.g. after a rejected add/remove).
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *languages_admin(int epoch, const char *error_message);

#endif // LANGUAGES_ADMIN_H
