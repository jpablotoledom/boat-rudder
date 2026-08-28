#include "request_user.h"
#include "../db/session_manager.h"
#include "../db/cms_users_admin.h"
#include <stdlib.h>
#include <string.h>

// Per-thread, per-request. See request_user.h for why this is not a parameter.
static __thread char current_user_name[128] = "";

void request_user_set(const char *cookie_header) {
    current_user_name[0] = '\0';

    char user_id[USER_ID_HEX_BUF_SIZE];
    if (validate_session_cookie(cookie_header, user_id) != 1) return;

    // cms_get_user_name_by_id() reflects the "Users" maintainer's optional
    // display `name` field, same as the blog author byline - it is often
    // unset (the account that ships with a fresh install has none), so an
    // active session must still show *something*, not a blank link.
    // cms_get_username_by_id() (email-prefix-derived, same as the media
    // library's per-author directories) is the existing fallback for that.
    char *name = cms_get_user_name_by_id(user_id);
    if (name && name[0]) {
        strncpy(current_user_name, name, sizeof(current_user_name) - 1);
        current_user_name[sizeof(current_user_name) - 1] = '\0';
    } else {
        cms_get_username_by_id(user_id, current_user_name, sizeof(current_user_name));
    }
    free(name);
}

const char *request_user_name(void) {
    return current_user_name;
}
