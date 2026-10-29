#ifndef EXTENSIONS_H
#define EXTENSIONS_H

#include <stddef.h>

// Extensions: programs of their own, installed under ./extensions/<name>/
// (next to configs/ and html/), that this server runs and puts inside the
// signed-in dashboard. Boat Rudder knows nothing of what one does: it finds
// it by its manifest (extension.conf), lets an admin enable or disable it
// (/dashboard/settings/extensions), starts and watches the process of every
// enabled one, and forwards /dashboard/ext/<name>/... to it over a Unix
// socket with the request's verified identity. The wire contract and the C
// SDK every extension links are in develop_docs/reference/extensions.md
// and sdk/brext.h.

#define EXTENSIONS_DIR          "./extensions"
#define EXTENSIONS_SOCKET_DIR   "./run/extensions"
#define EXTENSION_ROUTE_PREFIX  "/dashboard/ext/"
#define EXTENSION_PROTOCOL      1
#define EXTENSION_NAME_MAX      31

typedef enum {
    EXTENSION_DISABLED,   // found, valid, switched off
    EXTENSION_STARTING,   // process launched, socket not answering yet
    EXTENSION_RUNNING,    // socket answers
    EXTENSION_FAILED,     // kept exiting, or could not be launched - see `problem`
    EXTENSION_INVALID,    // manifest unusable - see `problem`; can't be enabled
} ExtensionState;

typedef struct {
    char name[EXTENSION_NAME_MAX + 1];
    char title[81];
    char description[241];
    char version[33];
    char nav_label[81];      // "" = no entry in the dashboard menu
    char nav_path[129];      // below /dashboard/ext/<name>, starts with '/'
    int  enabled;
    ExtensionState state;
    char problem[200];
} ExtensionInfo;

// Discovers the installed extensions, reads which ones are enabled
// (cms_extensions_enabled()) and starts the supervisor thread that launches
// and watches their processes. Call once from main() after MongoDB is up.
// Returns 0, or -1 if the supervisor could not start.
int extensions_start(void);

// Stops every extension process (SIGTERM, then SIGKILL) and the supervisor.
void extensions_stop(void);

// Re-reads ./extensions/: picks up extensions installed since, drops the
// ones whose directory is gone (stopping their process) and re-reads every
// manifest. The admin page calls it on each load.
void extensions_rescan(void);

// A snapshot of every known extension, sorted by name. *out is malloc'd
// (free() it); returns the count, 0 with *out = NULL when there are none.
size_t extensions_list(ExtensionInfo **out);

// One extension's snapshot. Returns 1 if `name` is known, 0 otherwise.
int extensions_get(const char *name, ExtensionInfo *out);

// Persists the choice and starts or stops the process accordingly.
// Returns 0, -1 for an unknown or invalid extension, -2 on a DB error.
int extensions_set_enabled(const char *name, int enabled);

typedef struct {
    const char *method;
    const char *mount;         // where the extension is mounted, "/dashboard/ext/<name>"
    const char *target;        // path below the mount plus query, "/..."
    const char *content_type;  // of the body, may be NULL
    const char *body;
    size_t      body_len;
    const char *user_id;
    const char *user_name;
    const char *role;
    const char *lang;
    const char *theme;
    int         epoch;
    const char *request_id;
} ExtensionRequest;

typedef struct {
    int    status;
    char   content_type[128];
    char   title[161];
    char   location[512];
    char  *body;               // NUL-terminated, body_len bytes before the NUL
    size_t body_len;
} ExtensionResponse;

// Sends `req` to `name`'s socket and reads its answer into `*out` (free
// out->body). Returns 0, -1 if the extension isn't running or didn't answer
// in time, -2 if the answer isn't a well-formed HTTP response.
int extensions_forward(const char *name, const ExtensionRequest *req, ExtensionResponse *out);

#endif // EXTENSIONS_H
