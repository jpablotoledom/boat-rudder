# Creating an extension

A walk-through, from an empty directory to an extension with its own data, enabled on a site.
The rules it follows - manifest, protocol, SDK - are in [extensions.md](extensions.md).

The example builds `notes`: each signed-in user keeps a list of notes.

---

## How it fits together

```
browser ──► Boat Rudder ──(guards: session, CSRF, role, epoch 3)──► extensions_forward()
                                                                         │ Unix socket
                                                                         ▼
                                                    your program: brext_serve() → handle()
                                                                         │
                                                                         ▼
                                                    MongoDB: ext_notes_* (your collections)
```

- Your program never sees a password, cookie or CSRF token: by the time a request reaches it,
  Boat Rudder has checked all of that and tells you who the user is (`brext_user_id()`).
- Your HTML is a fragment; Boat Rudder puts it inside the site's page.
- Boat Rudder starts your program when an admin enables it, and stops it when disabled.

Diagrams:

| | |
|---|---|
| [extensions-components.puml](../diagrams/extensions-components.puml) | the pieces and who talks to whom |
| [sequence-extension-enable.puml](../diagrams/sequence-extension-enable.puml) | install → enable → the program starts → menu entry |
| [sequence-extension-request.puml](../diagrams/sequence-extension-request.puml) | one request, from the browser to your handler and back |
| [extension-lifecycle.puml](../diagrams/extension-lifecycle.puml) | Disabled / Starting / Running / Failed / Invalid |

---

## 1. The repository

```
notes/
├── extension.conf
├── Makefile
├── src/main.c
└── .gitignore        bin/
```

Keep it next to a Boat Rudder checkout (`../boat-rudder`): the build takes the SDK from there.

## 2. The manifest

`extension.conf`:

```ini
name=notes
title=Notes
description=Your own notes.
version=0.1.0
protocol=1
exec=bin/notes
nav=Notes|/
```

`name` is also the directory it will be installed in, the URL (`/dashboard/ext/notes/`) and the
prefix of its collections (`ext_notes_…`). `nav` puts *Notes* in the dashboard menu, pointing at
`/` - the extension's first page.

## 3. The smallest handler

`src/main.c`:

```c
#include "brext.h"

static void handle(const BrextRequest *req, BrextResponse *res, void *data) {
    (void)data;
    if (brext_route(req, "GET", "/", NULL, 0)) {
        brext_html(res, "Notes");
        brext_append(res, "<section class=\"boat-rudder-dashboard\"><h1>Hello, ");
        brext_append_escaped(res, brext_user_name(req));
        brext_append(res, "</h1></section>");
    } else {
        brext_not_found(res);
    }
}

int main(void) { return brext_serve(handle, NULL); }
```

`brext_serve()` does the socket, threads and protocol; `handle()` runs once per request.

## 4. The Makefile

```make
BOAT_RUDDER ?= ../boat-rudder
SDK     := $(BOAT_RUDDER)/sdk
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -Wpedantic -I$(SDK) $(shell pkg-config --cflags libmongoc-1.0)
LDLIBS  += $(shell pkg-config --libs libmongoc-1.0) -lpthread

bin/notes: src/main.c $(SDK)/brext.c $(SDK)/brext.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ src/main.c $(SDK)/brext.c $(LDLIBS)

install: bin/notes
	@test -n "$(DEST)" || { echo "usage: make install DEST=<site>/extensions/notes"; exit 1; }
	install -d $(DEST)/bin
	install -m 755 bin/notes $(DEST)/bin/notes
	install -m 644 extension.conf $(DEST)/extension.conf
```

`-std=gnu17`, not `c17`: libmongoc's headers need the POSIX declarations GNU mode brings.

## 5. Try it without Boat Rudder

```bash
make
BR_SOCKET=/tmp/notes.sock ./bin/notes &
curl --unix-socket /tmp/notes.sock \
     -H 'X-BR-User-Id: 0123456789abcdef01234567' -H 'X-BR-User-Name: Pablo' \
     -H 'X-BR-Mount: /dashboard/ext/notes' http://x/
kill %1
```

You get the raw answer: status line, `Content-Type`, `X-BR-Title`, and the fragment.

## 6. Install and enable it

```bash
make install DEST=/path/to/boat-rudder/extensions/notes
```

Open **Dashboard → Menu → Administration → Extensions**: *Notes* is listed as *Disabled*. Press
**Enable**; within a second or two it shows *Running*, and *Notes* appears in the dashboard menu.
No server restart, at any step.

If it shows *Can't be used*, the reason is on the same row (a manifest field, a missing
executable…). If it goes *Failed*, it kept exiting: its own messages are in the server's log,
prefixed `[notes]`.

## 7. Store data

Use the site's database (`brext_mongodb_uri()`, `brext_mongodb_db()`), a **client pool** (the
handler runs on several threads at once), collections named `ext_<name>_…`, and **always filter
by the user**:

```c
#include <mongoc/mongoc.h>

static mongoc_client_pool_t *g_pool;

static void list_notes(const BrextRequest *req, BrextResponse *res) {
    mongoc_client_t *client = mongoc_client_pool_pop(g_pool);
    mongoc_collection_t *col = mongoc_client_get_collection(client, brext_mongodb_db(), "ext_notes_notes");
    bson_t *filter = BCON_NEW("owner_id", BCON_UTF8(brext_user_id(req)));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, NULL, NULL);
    /* ... append each note with brext_append_escaped() ... */
    mongoc_cursor_destroy(cursor);
    bson_destroy(filter);
    mongoc_collection_destroy(col);
    mongoc_client_pool_push(g_pool, client);
}

int main(void) {
    mongoc_init();
    mongoc_uri_t *uri = mongoc_uri_new(brext_mongodb_uri());
    g_pool = mongoc_client_pool_new(uri);
    mongoc_uri_destroy(uri);
    int rc = brext_serve(handle, NULL);
    mongoc_client_pool_destroy(g_pool);
    mongoc_cleanup();
    return rc;
}
```

The filter by `owner_id` goes into **every** query - find, update, delete - not only the list:
an id in a URL can be guessed, so "not yours" must behave exactly like "doesn't exist" (404).

## 8. Forms

```c
} else if (brext_route(req, "GET", "/notes/new", NULL, 0)) {
    brext_html(res, "New note");
    brext_appendf(res,
        "<section class=\"boat-rudder-dashboard\"><h1>New note</h1>"
        "<form class=\"boat-rudder-dashboard__form boat-rudder-card\" method=\"POST\" action=\"%s/notes/new\">"
        "<div class=\"boat-rudder-dashboard__form-row\"><label for=\"text\">Text</label>"
        "<input class=\"boat-rudder-input\" id=\"text\" name=\"text\" required></div>"
        "<div class=\"boat-rudder-dashboard__button-container\">"
        "<button class=\"boat-rudder-btn boat-rudder-btn--primary\" type=\"submit\">Save</button>"
        "</div></form></section>", brext_mount(req));

} else if (brext_route(req, "POST", "/notes/new", NULL, 0)) {
    char text[1024];
    if (!brext_form(req, "text", text, sizeof(text)) || !text[0]) {
        brext_status(res, 422);          /* show the form again, with the message */
        /* ... */
    } else {
        /* insert {owner_id: brext_user_id(req), text} */
        brext_redirect(res, "/");        /* back to the list: a reload won't resubmit */
    }
}
```

- **Links and actions** start with `brext_mount(req)` (`/dashboard/ext/notes`);
  **redirects** take the path below it (`"/"`).
- **CSRF**: nothing to do. The page's script adds the token to every `method="post"` form, and
  Boat Rudder checks it before your handler runs.
- **Viewers** (the read-only role) never reach a POST handler: Boat Rudder refuses it first.
- **Escape** whatever a user typed: `brext_append_escaped()`, also inside `value="…"`.
- **Look**: use the dashboard's classes (`boat-rudder-btn`, `-card`, `-input`,
  `-dashboard__table`…); list in [extensions.md §5](extensions.md#writing-the-html).

## 9. Update and remove

- **Update**: `make install DEST=…` again, then **Disable** and **Enable** so the new program is
  started.
- **Remove**: **Disable**, then delete `extensions/notes/`. Its data stays: drop the `ext_notes_*`
  collections by hand if it should go too.

## Checklist

- [ ] `name` = directory name; `[a-z0-9_-]`, up to 31 characters
- [ ] `protocol=1`; `exec` builds and is executable
- [ ] Handler thread-safe (pool, no shared mutable state without a lock)
- [ ] Every query filtered by `brext_user_id(req)`
- [ ] Every user-typed value escaped
- [ ] Links with `brext_mount()`, redirects with a path below it
- [ ] Collections `ext_<name>_*`
- [ ] POST → redirect on success, 422 + form on a validation error, 404 for "not found / not yours"
