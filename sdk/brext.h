#ifndef BREXT_H
#define BREXT_H

// Boat Rudder extension SDK - the server side of the extension protocol, for
// an extension written in C. Copy brext.h and brext.c into the extension (or
// compile them from a Boat Rudder checkout) and link them in statically: an
// installed extension then never depends on the Boat Rudder it runs beside.
//
// Boat Rudder starts the extension's program with BR_SOCKET (and the other
// BR_* variables below) set, sends it every request for
// /dashboard/ext/<name>/... with the signed-in user it verified, and shows
// what the handler answers inside the site. Reference:
// develop_docs/reference/extensions.md.
//
//     static void handle(const BrextRequest *req, BrextResponse *res, void *data) {
//         if (brext_route(req, "GET", "/", NULL, 0)) {
//             brext_html(res, "Hello");
//             brext_append(res, "<p>Hello, ");
//             brext_append_escaped(res, brext_user_name(req));
//             brext_append(res, "</p>");
//         } else {
//             brext_not_found(res);
//         }
//     }
//     int main(void) { return brext_serve(handle, NULL); }

#include <stddef.h>

#define BREXT_PROTOCOL      1
#define BREXT_MAX_HEADERS   48
#define BREXT_MAX_BODY      (8 * 1024 * 1024)

typedef struct {
    char *name;
    char *value;
} BrextHeader;

typedef struct {
    char        method[16];        // "GET", "HEAD", "POST"
    char        path[1024];        // below the extension's mount, "/" at least, still URL-encoded
    char        query[2048];       // after '?', "" if none
    BrextHeader headers[BREXT_MAX_HEADERS];
    int         header_count;
    char       *body;              // NUL-terminated; body_len bytes before the NUL
    size_t      body_len;
} BrextRequest;

typedef struct {
    int    status;                 // 200 unless set
    char   content_type[96];
    char   title[160];             // X-BR-Title: the page title Boat Rudder shows
    char   location[512];          // set by brext_redirect()
    char  *body;
    size_t len, cap;
    int    failed;                 // an allocation failed: answered as 500
} BrextResponse;

typedef void (*BrextHandler)(const BrextRequest *req, BrextResponse *res, void *userdata);

// Listens on BR_SOCKET and calls `handler` for every request, one thread per
// connection - so the handler must be thread-safe (a MongoDB client pool,
// not a single client). Returns when Boat Rudder sends SIGTERM (it does so
// when the extension is disabled or the server stops): 0 then, 1 if the
// socket could not be set up. Meant to be main()'s return value.
int brext_serve(BrextHandler handler, void *userdata);

// ---- Environment set by Boat Rudder ------------------------------------------
const char *brext_name(void);          // BR_EXTENSION      - the extension's name
const char *brext_dir(void);           // BR_EXTENSION_DIR  - its directory (also the cwd)
const char *brext_mongodb_uri(void);   // BR_MONGODB_URI    - the site's database server
const char *brext_mongodb_db(void);    // BR_MONGODB_DB     - the site's database; name your
                                       //                     collections ext_<name>_...
int         brext_log_level(void);     // BR_LOG_LEVEL      - 0 none .. 4 debug

// Writes "[<name>] message" to stderr, which ends up in the server's log, when
// `level` <= brext_log_level() (1 error, 2 warning, 3 info, 4 debug).
void brext_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- Request ------------------------------------------------------------------

// Value of a request header (case-insensitive), "" when absent. Never NULL.
const char *brext_header(const BrextRequest *req, const char *name);

// Who is asking, as Boat Rudder verified it. On every request an extension
// receives there is a signed-in user: these are never "".
const char *brext_user_id(const BrextRequest *req);    // 24-hex user id - the key for "my data"
const char *brext_user_name(const BrextRequest *req);  // display name
const char *brext_role(const BrextRequest *req);       // "admin", "author" or "viewer"
const char *brext_lang(const BrextRequest *req);       // content language, e.g. "en"
int         brext_epoch(const BrextRequest *req);      // 3 today: the dashboard is epoch 3 only
// Where the extension's pages are, "/dashboard/ext/<name>" - the prefix for
// every link and form action in its HTML (brext_redirect() takes a path
// below it instead).
const char *brext_mount(const BrextRequest *req);

// Matches the request's method and path against `pattern`, where one '*'
// stands for one path segment ("/notes/*/edit"). The segment, decoded,
// goes to `segment` (when not NULL). Returns 1 on a match. "GET" also
// matches a HEAD request.
int brext_route(const BrextRequest *req, const char *method, const char *pattern,
                char *segment, size_t segment_size);

// A query-string (brext_query) or urlencoded-form (brext_form) field,
// decoded into `out` (truncated to out_size - 1). Returns 1 if present.
int brext_query(const BrextRequest *req, const char *key, char *out, size_t out_size);
int brext_form(const BrextRequest *req, const char *key, char *out, size_t out_size);

// ---- Response -----------------------------------------------------------------

// An HTML fragment Boat Rudder wraps in the site's layout - menu, theme,
// the dashboard's stylesheet (use its classes: boat-rudder-btn, -card,
// -input, -dashboard__table...) and the CSRF script that lets <form
// method="post"> work. `title` becomes the page title.
void brext_html(BrextResponse *res, const char *title);

// JSON, passed through as it is - for the page's own scripts.
void brext_json(BrextResponse *res);

// 302 to a path inside the extension ("/" is its first page).
void brext_redirect(BrextResponse *res, const char *path);

void brext_status(BrextResponse *res, int status);
void brext_not_found(BrextResponse *res);      // 404 with a short HTML fragment

void brext_append(BrextResponse *res, const char *text);
void brext_appendf(BrextResponse *res, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Appends `text` HTML-escaped (& < > " ') - for anything a user typed.
void brext_append_escaped(BrextResponse *res, const char *text);

#endif // BREXT_H
