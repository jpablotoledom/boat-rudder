# Extensions

An extension is a program of its own that runs beside a Boat Rudder site and adds pages to its
dashboard. Boat Rudder doesn't know what an extension does: it finds it, lets an admin enable or
disable it, runs it while it is enabled, and passes it the requests for its pages together with
the signed-in user it verified. What the pages show, what they store and what else the program
does (talk to devices, run timers…) is up to the extension.

Step by step, from an empty directory: [creating-an-extension.md](creating-an-extension.md).
Design decisions and what is planned next: [plans/adr-extensions.md](../plans/adr-extensions.md).

Diagrams: [extensions-components.puml](../diagrams/extensions-components.puml) (the pieces),
[sequence-extension-enable.puml](../diagrams/sequence-extension-enable.puml) (install, enable,
start), [sequence-extension-request.puml](../diagrams/sequence-extension-request.puml) (one
request), [extension-lifecycle.puml](../diagrams/extension-lifecycle.puml) (states).
A complete example, built step by step - `notes`, each user's own list of notes - is in
[creating-an-extension.md](creating-an-extension.md).

---

## 1. What Boat Rudder does for an extension

| | |
|---|---|
| **Finds it** | Every directory under `extensions/` (next to `configs/` and `html/`) holding an `extension.conf` |
| **Enables / disables it** | `/dashboard/settings/extensions` (admin). A newly installed extension is **disabled** until an admin enables it; the choice is stored in the `extensions` collection and survives restarts |
| **Runs it** | Starts the program of every enabled extension, restarts it if it exits (1 s, 2 s… apart; after 5 exits within 60 s it is marked *Failed* until re-enabled), stops it with `SIGTERM` (then `SIGKILL` after 3 s) when disabled or when the server stops |
| **Mounts it** | Its pages live at `/dashboard/ext/<name>/…` |
| **Guards it** | Same as every dashboard page: epoch 3 only, a valid session, the CSRF token on every POST, a *viewer*'s POSTs refused - all checked **before** the request reaches the extension |
| **Frames it** | An HTML answer is wrapped in the site's layout: menu, theme, the dashboard's stylesheet, the CSRF script |
| **Lists it** | An enabled extension with a `nav=` entry gets a link in the dashboard menu's *Extensions* group, for every role |

An extension never handles passwords, cookies, sessions, CSRF or themes.

## 2. Layout of an installed extension

```
extensions/
└── notes/                  directory name = extension name: [a-z0-9_-], 1-31 characters
    ├── extension.conf      the manifest
    └── bin/notes           the program (exec=)
```

Anything else in the directory (data files, assets) is the extension's own. Boat Rudder's updater
never touches `extensions/`, and no Boat Rudder release ships anything there.

## 3. The manifest - `extension.conf`

```ini
# key=value; '#' or ';' starts a comment
name=notes                         ; required, must equal the directory name
title=Notes                        ; required, shown in the admin list and as the default page title
description=Your own notes.
version=0.1.0                      ; required, shown in the admin list
protocol=1                         ; required, the protocol below; this Boat Rudder speaks 1
exec=bin/notes                     ; required, path inside the directory, executable
nav=Notes|/                        ; optional, dashboard menu link: label|path below the mount
```

A manifest that fails any check is listed as *Can't be used* with the reason, and can't be
enabled. The directory is re-read every time the admin page loads, so installing or fixing an
extension needs no server restart.

## 4. The process

Boat Rudder runs `exec` with the extension's directory as the working directory and these
variables added to its environment:

| Variable | Value |
|---|---|
| `BR_EXTENSION` | the extension's name |
| `BR_EXTENSION_DIR` | absolute path of its directory |
| `BR_SOCKET` | the Unix socket to listen on (`run/extensions/<name>.sock`) |
| `BR_MONGODB_URI`, `BR_MONGODB_DB` | the site's database |
| `BR_LOG_LEVEL` | the site's `verbose_level` (0 none … 4 debug) |

The socket's absolute path must fit a Unix socket address (107 bytes): with Boat Rudder installed
in a very long directory the extension is listed as *Can't be used* ("socket path too long").
Standard output and error go to the server's own log. The process runs as the same user as the
server and dies with it (`PR_SET_PDEATHSIG`).

**Data.** Use the site's database and name every collection `ext_<name>_<what>`
(`ext_notes_notes`): the site's backups then include it, and it can never collide with
Boat Rudder's own collections. Key a user's data by `X-BR-User-Id`.

## 5. Protocol 1

HTTP/1.1 over the Unix socket, one request per connection.

### Request (Boat Rudder → extension)

The browser's method (`GET`, `HEAD` or `POST`), the path **below the mount** (`/` at least; still
URL-encoded, query string included), the body and its `Content-Type` as the browser sent them
(the CSRF field `csrf_token` included - ignore it), and:

| Header | |
|---|---|
| `X-BR-Protocol` | `1` |
| `X-BR-Mount` | `/dashboard/ext/<name>` - prefix every link and form action with it |
| `X-BR-User-Id` | the signed-in user's id (24 hex) - never empty |
| `X-BR-User-Name` | display name |
| `X-BR-Role` | `admin`, `author` or `viewer` |
| `X-BR-Lang` | content language (`en`, `es`…) |
| `X-BR-Theme` | the visitor's theme key |
| `X-BR-Epoch` | `3` (the dashboard is epoch 3 only) |
| `X-BR-Request-Id` | an id to correlate logs |

No other browser header is forwarded. These can be trusted: only Boat Rudder can open the socket
(mode `0600`).

### Response (extension → Boat Rudder)

Read until the extension closes the connection; at most 4 MiB, within 15 s.

| Answer | Boat Rudder does |
|---|---|
| `Content-Type: text/html` | Wraps the body (a fragment: no `<html>`/`<head>`) in the site's layout. `X-BR-Title` sets the page title (`<site> - <title>`), else the manifest's `title`. The status code is kept |
| `Content-Type: application/json` | Passes the body through with the status code - for the page's own scripts (`fetch()` gets the CSRF header automatically) |
| `3xx` + `Location: /path` | Redirects the browser to `<mount>/path`. A `Location` that isn't a path (`http://…`, `//…`) is refused with 502 |
| anything else | 502 |

If the extension isn't running or doesn't answer in time the visitor gets a 503 page; a malformed
answer gives 502. Either way the rest of the site is unaffected.

### Writing the HTML

The page uses the dashboard's stylesheet, so its components look native: `boat-rudder-dashboard`
(section), `boat-rudder-dashboard__breadcrumb`, `boat-rudder-dashboard__panel-header` /
`__panel-actions`, `boat-rudder-dashboard__table` (inside `__table-scroll`),
`boat-rudder-dashboard__form` + `boat-rudder-card`, `__form-row`, `boat-rudder-input`,
`boat-rudder-btn` (`--primary`, `--danger`), `boat-rudder-alert boat-rudder-alert--error`,
`boat-rudder-empty`. See [style-guide.md](style-guide.md) §15 and the dashboard's own templates.

- Escape everything a user typed (`brext_append_escaped()`).
- `<form method="post">` needs nothing else: the page's CSRF script adds the token on submit.
- After a successful POST, redirect (`brext_redirect(res, "/")`) so a reload doesn't resubmit;
  on a validation error, answer the form again with status 422.

## 6. The C SDK - `sdk/brext.h`

Two files, `sdk/brext.h` and `sdk/brext.c`, compiled straight into the extension (static: an
installed extension never depends on the Boat Rudder beside it). It implements the server side of
the protocol; the extension only writes a handler.

```c
#include "brext.h"

static void handle(const BrextRequest *req, BrextResponse *res, void *data) {
    char id[64];
    if (brext_route(req, "GET", "/", NULL, 0)) {
        brext_html(res, "Hello");
        brext_append(res, "<section class=\"boat-rudder-dashboard\"><h1>Hello, ");
        brext_append_escaped(res, brext_user_name(req));
        brext_append(res, "</h1></section>");
    } else if (brext_route(req, "POST", "/things/*/delete", id, sizeof(id))) {
        /* delete thing `id` of brext_user_id(req) */
        brext_redirect(res, "/");
    } else {
        brext_not_found(res);
    }
}

int main(void) { return brext_serve(handle, NULL); }
```

| Function | |
|---|---|
| `brext_serve(handler, data)` | Listens on `BR_SOCKET`, one thread per request; returns on `SIGTERM`. **The handler must be thread-safe** (e.g. a `mongoc_client_pool_t`, not one client) |
| `brext_route(req, method, pattern, seg, size)` | Method + path match; one `*` = one segment, decoded into `seg`. `"GET"` also matches `HEAD`. Trailing slashes ignored |
| `brext_form(req, key, out, size)` / `brext_query(…)` | A decoded urlencoded-form / query field; 1 if present |
| `brext_user_id/_name`, `brext_role`, `brext_lang`, `brext_epoch`, `brext_mount`, `brext_header` | The request's identity and context (§5) |
| `brext_html(res, title)` / `brext_json(res)` | Set the answer's type |
| `brext_append`, `brext_appendf`, `brext_append_escaped` | Build the body |
| `brext_status`, `brext_redirect`, `brext_not_found` | Status, `302` to a path below the mount, a 404 page |
| `brext_mongodb_uri/_db`, `brext_name`, `brext_dir`, `brext_log` | Environment (§4) and logging to the server log |

Limits: request body 8 MiB, request head 16 KiB, 15 s per read/write.

### Building

```make
BOAT_RUDDER ?= ../boat-rudder
CFLAGS += -std=gnu17 -Wall -Wextra -I$(BOAT_RUDDER)/sdk $(shell pkg-config --cflags libmongoc-1.0)
LDLIBS += $(shell pkg-config --libs libmongoc-1.0) -lpthread
bin/myext: src/main.c $(BOAT_RUDDER)/sdk/brext.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
```

### Installing, updating, removing

```bash
make install DEST=/path/to/site/extensions/myext   # copy extension.conf + the program
# then /dashboard/settings/extensions -> Enable
```

Updating is installing over it; the supervisor restarts the program when it exits, so after a
new binary is in place, disable and enable it (or stop its process). Removing: disable it, then
delete its directory; its `ext_<name>_*` collections stay until dropped by hand.

## 7. Trying an extension by hand

Without Boat Rudder, run the program with a socket of your own and talk to it with curl:

```bash
BR_SOCKET=/tmp/myext.sock BR_MONGODB_URI=mongodb://localhost:27017 BR_MONGODB_DB=test ./bin/myext &
curl --unix-socket /tmp/myext.sock -H 'X-BR-User-Id: 0123456789abcdef01234567' \
     -H 'X-BR-Mount: /dashboard/ext/myext' http://x/
```

## 8. Not in protocol 1 yet

Public pages (no session) and the retro epochs, answered as content blocks rendered for every
epoch; static assets served from the extension's directory; a `member` role for users who only
use extensions; packaging (`boat-rudder-ext`). See the ADR.
