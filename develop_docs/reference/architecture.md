# base-http-server - Architecture

## Overview

`base-http-server` is a minimal, self-contained static HTTP/HTTPS server written in **C17**. Its core dependency is OpenSSL; the login/session feature additionally depends on **libmongoc** (MongoDB) and **libsodium** (Argon2id password hashing, CSPRNG). It serves files from a configurable root directory, supports concurrent connections via POSIX threads, and optionally enables TLS.

The design goal is to be a clean, portable foundation that can be embedded in other C projects or used as a standalone server for static assets, targeting **Linux and macOS** as primary platforms.

---

## Directory Structure

```
base-http-server/
├── src/
│   ├── main.c                           # Entry point
│   ├── web_server/
│   │   ├── server_listener.c/h          # Socket setup and main accept loop
│   │   ├── connection.c/h               # Unified read/write abstraction (plain + TLS)
│   │   ├── connection_thread.c/h        # Per-connection POSIX thread
│   │   ├── tls_context.c/h              # OpenSSL context lifecycle
│   │   ├── http_router.c/h              # HTTP routing and dispatch
│   │   ├── http_request_parser.c/h      # Raw HTTP/1.1 request parser
│   │   ├── http_constants.h             # Buffer sizes and compile-time assertions
│   │   └── utils/
│   │       ├── static_file_server.c/h   # Static file serving (MIME, cache, security headers)
│   │       └── url_parser.c/h           # URL path / query-string splitter + QueryParam
│   ├── html_builder/
│   │   └── orchestrator.c/h             # buildHomeWebSite()/buildPageWebSite(): assemble pages per epoch
│   ├── modules/
│   │   ├── container/container.c/h      # Page shell (head/body wrapper) per epoch
│   │   ├── menu/menu.c/h                # Nav menu + menu-item + separator per epoch
│   │   ├── slider/slider.c/h            # Hero/banner block per epoch
│   │   ├── home_content/home_content.c/h# Home page body content per epoch
│   │   ├── home_blog/home_blog.c/h      # "Latest Blog Posts" gallery per epoch
│   │   ├── login/login.c/h              # /login form (epoch3 only) per epoch
│   │   ├── dashboard/dashboard.c/h      # /dashboard static "Welcome" content per epoch
│   │   └── error/error.c/h              # Centralized error page content per epoch
│   ├── db/
│   │   ├── mongodb_manager.c/h          # MongoDB client pool lifecycle (init/cleanup/get)
│   │   ├── auth.c/h                     # Email/password verification (Argon2id via libsodium)
│   │   └── session_manager.c/h          # Session tokens, cookies, sessions collection
│   └── utils/
│       ├── config_loader.c/h            # INI-style config file parser
│       ├── log.c/h                      # Leveled, thread-safe logging macros
│       ├── http_utils.c/h               # MIME detection, URL encode/decode, path sanitizer,
│       │                                # trusted proxy check
│       ├── detect_epoch.c/h             # User-Agent → epoch heuristic
│       ├── read_file.c/h                # read_file_to_string(): malloc'd file contents
│       ├── template_utils.c/h           # render_template, str_replace_first, str_append
│       ├── generate_url_theme.c/h       # Builds ./html/themes/<theme>/... paths per epoch
│       └── build_epoch_response.c/h     # Wraps rendered HTML with epoch-correct headers,
│                                         # plus status-line and redirect variants
├── configs/
│   └── settings.conf                    # Runtime configuration
├── html/                                 # Static + templated content root (themes/, assets/)
├── ssl/                                 # TLS certificate and key (optional)
└── CMakeLists.txt                       # Build definition
```

---

## Layers

```
┌────────────────────────────────────────────────────────┐
│                      main.c                            │  ← Entry point: config, signals, lifecycle
├────────────────────────────────────────────────────────┤
│              web_server/server_listener                │  ← Socket creation, select() accept loop
├────────────────────────────┬───────────────────────────┤
│       connection.c         │     tls_context.c         │  ← I/O abstraction  │  TLS context
├────────────────────────────┴───────────────────────────┤
│              connection_thread.c                       │  ← Per-request pthread + TLS handshake
├────────────────────────────────────────────────────────┤
│              http_router.c                             │  ← HTTP parse, route dispatch
├────────────────────────────┬───────────────────────────┤
│    http_request_parser.c    │  utils/static_file_server│  ← Parser  │  File server
├────────────────────────────┴───────────────────────────┤
│       utils/  (log, config_loader, http_utils)         │  ← Cross-cutting utilities
└────────────────────────────────────────────────────────┘
```

---

## Component Descriptions

### `main.c`
- Accepts `[-c <config>] <root_directory>` arguments.
- Loads configuration via `config_loader`.
- Registers POSIX signal handlers (`SIGINT`, `SIGTERM` → graceful shutdown; `SIGCHLD` → zombie reap; `SIGPIPE` → ignored).
- Uses `_Atomic int running` for the shutdown flag - formally correct for signal handler / main thread coordination.
- Calls `server_start()` and blocks until `running == 0`.
- Calls `server_stop()` and exits cleanly.

### `web_server/server_listener.c`
- Creates one or two TCP sockets (HTTP and optionally HTTPS).
- Binds and listens on the configured ports.
- Runs a `select()`-based accept loop (no busy-wait, efficient for moderate traffic).
- Enforces:
  - **Global connection limit** (`MAX_CONNECTIONS = 200`) via a mutex-protected counter.
  - **Per-IP rate limiting** (`MAX_IPS = 1024` entries, `RATE_LIMIT = 500` req per `RATE_WINDOW = 5 s`) with temporary IP blocking and **LRU eviction** when the table is full.
- Uses `pthread_cond_t` in `server_stop()` to wait for active TLS threads before freeing `ssl_ctx`, preventing a use-after-free race condition.
- For each accepted connection, allocates `thread_args`, optionally wraps the socket in `SSL_new()`, and spawns a detached `pthread`.

### `web_server/connection.c`
Provides a transparent I/O layer:

| Function | Description |
|---|---|
| `connection_write()` | Write bytes to a plain socket or TLS session |
| `plain_read()` | Read from a plain TCP socket |
| `ssl_read()` | Read from a TLS session |
| `connection_close()` | Orderly shutdown (TLS or plain) and `close()` |

Both `ssl_read` and `plain_read` share the `read_func_t` signature so the rest of the code is protocol-agnostic.

### `web_server/connection_thread.c`
- Entry point for each per-connection pthread.
- Sets socket timeouts (5 s read + write).
- Performs `SSL_accept()` if the connection is HTTPS.
- Calls `http_route()` passing the appropriate `read_func_t`.
- Always decrements the global `active_connections` counter on exit.

### `web_server/tls_context.c`
- `tls_create_context(cert, key)` - loads PEM certificate and private key into a new `SSL_CTX`.
- Enforces **TLS 1.2 minimum** via `SSL_CTX_set_min_proto_version`.
- Applies hardened cipher suites for both TLS 1.2 (ECDHE+AESGCM/ChaCha20) and TLS 1.3.
- `tls_free_context(ctx)` - frees the context.

### `web_server/http_router.c`
- Reads the raw HTTP request into a fixed 32 KiB buffer; responds `431` if exceeded.
- Parses headers with `parse_http_request()`.
- Extracts the real client IP: honors `X-Real-IP` / `X-Forwarded-For` **only when the peer IP matches the `trusted_proxies` config list**, falling back to the raw socket address for all other peers.
- Validates the request line (400 Bad Request on failure).
- `resolve_epoch(req)`: the configured `force_epoch` override (if in `-1..3`), otherwise
  `detect_epoch(User-Agent)`.
- Routes `GET`/`HEAD`:
  - `/` → dynamic, epoch-aware home page (see [Retro-Compatible CMS](#retro-compatible-cms-epoch-based-rendering) below).
  - `/login` → renders `login_epoch<N>.html` via `buildPageWebSite()` (see [Login, Dashboard, Logout](#login-dashboard-and-logout)).
  - `/dashboard` → requires a valid session cookie, otherwise redirects to `/login`.
  - `/logout` → destroys the session and redirects to `/`.
  - anything else → `serve_static_file()`, passing the `If-Modified-Since` header for cache
    validation; a non-zero return code (`403`/`404`/`500`) is rendered via
    `send_error_response()`.
- `POST /login` → epoch3 only; other epochs re-render the "not available" `login_epoch<N>.html`
  without touching the database. See [Login, Dashboard, Logout](#login-dashboard-and-logout).
- Returns `204` for `OPTIONS`, `405` for all other methods.
- `send_error_response(ctx, status_code, status_line, epoch)`: renders `error_content()` +
  `buildPageWebSite()` + `build_epoch_response_status()` for any non-2xx/3xx response (`400`,
  `403`, `404`, `405`, `431`, `500`, `503`), falling back to a hardcoded minimal HTML response
  if template rendering itself fails. This is the single centralized path for all
  epoch-aware error pages.
- `send_or_error(ctx, response, method, epoch)`: writes a malloc'd response (truncated to
  headers for `HEAD`), or calls `send_error_response(ctx, 500, ...)` if `response` is `NULL`
  (e.g. a module returned `NULL`).

### `web_server/http_request_parser.c`
- Parses `METHOD URL PROTOCOL` from the first line.
- Parses headers into key-value pairs (up to `MAX_HEADERS = 64`).
- URL field sized at 2048 bytes to handle long query strings without truncation.

### `web_server/utils/static_file_server.c`
- Validates the resolved path with `sanitize_path()` (directory traversal prevention).
- Directories: serves `index.html` inside them; returns 404 if it does not exist.
- Cache validation: reads `st_mtime`, sends `Last-Modified` and `Cache-Control: public, max-age=3600`. Responds `304 Not Modified` when the `If-Modified-Since` header matches.
- Sends security headers on every response: `X-Content-Type-Options: nosniff`, `X-Frame-Options: SAMEORIGIN`.
- Streams files in 8 KiB blocks.
- `serve_static_file()` returns `int`, not `void`: `0` if a response was already written
  (`200`, `304`, or a streaming failure that closed the connection mid-transfer), otherwise an
  HTTP status code (`403`, `404` or `500`) with **no response written yet**. This lets
  `http_router.c` render these errors via the same epoch-aware `send_error_response()` used
  for every other error path, while keeping `static_file_server.c` free of any dependency on
  `html_builder`/`modules` (it only depends on `web_server/connection` and `utils/`).

### `web_server/utils/url_parser.c`
- Splits a URL string into a path component and query parameters (`?key=value&…`).
- Owns the `QueryParam` struct definition.

### `utils/config_loader.c`
- Reads `key=value` lines from the config file (path configurable via `-c` CLI flag).
- Populates: `http_port`, `https_port`, `ssl_enabled`, `ssl_cert`, `ssl_key`, `verbose_level`, `trusted_proxies`, `theme`, `lang`, `force_epoch`, `public_url`, `mongodb_uri`, `mongodb_db`, `session_ttl_seconds`.

### `utils/log.h`
- Four-level logging macros: `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`.
- All writes are serialized through a `pthread_mutex_t` - safe for concurrent threads.
- Controlled by the global `log_level` integer set from config.

### `utils/http_utils.c`
| Function | Description |
|---|---|
| `get_mime_type(path)` | MIME string from file extension |
| `url_decode(dst, src)` | Percent-decode a URL |
| `url_encode(dst, src, size)` | Percent-encode a string |
| `html_encode(dst, src, size)` | HTML entity encoding |
| `sanitize_path(url, safe, size, root)` | `realpath()` + validates path stays inside root |
| `is_trusted_proxy(peer_ip)` | Checks peer IP against `trusted_proxies` config list |

---

## Retro-Compatible CMS (epoch-based rendering)

The `/` route is rendered dynamically instead of being served as a static file. The goal is to send each
client the simplest markup its browser can handle, from WAP-era phones to modern HTML5/CSS3 browsers.

### Epochs

`utils/detect_epoch.c` classifies the `User-Agent` header into one of five epochs:

| Epoch | Constant | Target | `Content-Type` |
|---|---|---|---|
| -1 | `EPOCH_WML` | WAP phones | `text/vnd.wap.wml` |
| 0 | `EPOCH_PRESTANDARD` | Text browsers (e.g. Lynx) | `text/html` |
| 1 | `EPOCH_EARLY` | Old browsers (table layouts, `<font>`) | `text/html` |
| 2 | `EPOCH_MIDDLE` | HTML4 + CSS1 era | `text/html; charset=UTF-8` |
| 3 | `EPOCH_MODERN` | Modern HTML5 + CSS3 | `text/html; charset=UTF-8` |

### Templates (`html/themes/<theme>/`)

**Convention: no HTML/markup embedded in C.** All HTML, WML and other markup lives in
template files under `html/themes/<theme>/`, loaded via `generate_url_theme()` +
`read_file_to_string()` and rendered with `render_template()` / `str_replace_first()`. C code
must not contain inline markup string literals (e.g. `"<p>...</p>"`); if a new piece of markup
is needed, add a template file instead. The one accepted exception is the catastrophic
fallback in `send_error_response()` (`http_router.c`), which sends the plain-text status line
via `send_simple()` when the template pipeline itself has failed and cannot be relied on.

Each visual component has one HTML template per epoch, named `<component>_epoch<N>.html`
(`<N>` ∈ {-1, 0, 1, 2, 3}), under `html/themes/<theme>/<component>/`:

- `container/` - page shell (`<head>`/`<body>` wrapper); contains `{{PAGE_TITLE}}` and 4 `%s`
  placeholders for menu, slider, home content and home blog.
- `menu/` - `menu_epoch<N>.html` (1 `%s`: items), `menu-item_epoch<N>.html` (3 `%s`: link, name,
  separator), `menu-item-separator_epoch<N>.html` (static, no placeholders). See "Menu" below.
- `slider/` - hero/banner block, static per epoch (no placeholders).
- `home-content/` - `home-content_epoch<N>.html` (1 `%s`: items) and
  `home-content-item_epoch<N>.html` (3 `%s`: title, date, text).
- `home-blog/` - `home-blog_epoch<N>.html` (1 `%s`: items) and `home-blog-item_epoch<N>.html`
  (epoch -1/0: 4 `%s` - title, date, summary, categories; epoch 1/2/3: 7 `%s` - image, link,
  title, summary, author, categories, date); `home-blog/empty_epoch<N>.html`, a static
  "No blog entries found" message rendered instead of `items` when there are no blog entries.
  Category tags reuse `elements/category/category_epoch<N>.html` items, concatenated without
  the `entry-categories` wrapper. See "Home blog list" below.
- `page/` - `page_epoch<N>.html`, the generic page shell for non-home routes (`/login`,
  `/dashboard`, error pages); contains `{{PAGE_TITLE}}`, 1 `%s` for the menu and 1 `%s` for the
  page's content fragment.
- `login/` - `login_epoch<N>.html`. Epoch 3 has 1 `%s` for an optional error block; epochs
  -1/0/1/2 are static "login is not available on this device" content.
- `dashboard/` - `dashboard_epoch<N>.html`, static "Welcome to dashboard" content per epoch (no
  placeholders).
- `error/` - `error_epoch<N>.html`, 2 `%s` placeholders (status code, message).
- `redirect/` - `redirect_epoch<N>.html`, the standalone "click here to continue" body sent
  with `302` responses (for clients that don't auto-follow `Location`); 2 `%s` placeholders
  (both the redirect target: link `href` and link text). Not wrapped by `page_epoch<N>.html`.
- `entry/` - `entry-header_epoch<N>.html`, the header/SEO block for a CMS entry (epochs 1-3:
  5 `%s` - image, title, summary, author, date; epochs -1/0: 4 `%s` - title, summary, author,
  date); `entry-categories_epoch<N>.html`, the category "tags" wrapper (1 `%s` - concatenated
  rendered category items). See "CMS entries" below.
- `elements/<type>/` - one subdirectory per content-block type (e.g. `tittle`, `paragraph`,
  `image`, `byline`, `category`), each with `<type>_epoch<N>.html`. See "CMS entries" below.

`%s` placeholders are resolved with `printf`-family formatting, so any literal `%` in a template
that is itself used as a format string must be written as `%%`. Templates that are only ever
substituted *into* another template's `%s` (e.g. `slider`, `menu-item-separator`) are inserted
as-is and must use a single `%`.

### Build pipeline

```
http_router.c  (route == "/")
  │
  ├─ epoch = (force_epoch in -1..3) ? force_epoch : detect_epoch(User-Agent)
  │
  ├─ buildHomeWebSite(epoch, lang)        ── html_builder/orchestrator.c
  │     ├─ container(epoch)               ── modules/container
  │     ├─ menu("/", epoch)                ── modules/menu
  │     │     cms_get_menu_items(lang, &items, &count) ── src/db/cms_menu.c
  │     ├─ slider(epoch)                   ── modules/slider
  │     ├─ home_content(epoch, lang)       ── modules/home_content
  │     ├─ home_blog(epoch, lang)          ── modules/home_blog
  │     │     cms_get_blog_entries(lang, &items, &count) ── src/db/cms_entries.c
  │     └─ render_template(container, menu, slider, home_content, home_blog)
  │
  └─ build_epoch_response(body, extra_headers, epoch) ── utils/build_epoch_response.c
        sets Content-Type per epoch, reuses SECURITY_HEADERS
```

Each module resolves its template path via `generate_url_theme("<subpath>_epoch%d.html", epoch)`,
which expands to `./html/themes/<theme>/<subpath>` (relative to the process working directory,
using the global `theme` from `config_loader`), reads it with `read_file_to_string()`, and renders
it with `render_template()`. The orchestrator frees every intermediate buffer on all paths.

For `HEAD /`, `http_router.c` builds the same response and truncates it at the end of the header
block (`\r\n\r\n`) before writing.

### CMS entries (`GET /page/<link>`)

A first increment of the database-backed CMS described in
`develop_docs/plans/cms-entry-model-plan.md`: a single `entries` MongoDB collection
(`ENTRIES_COLLECTION`, `src/db/mongodb_manager.h`) holds one self-contained document per page -
`header` (image, title, summary, author, date) plus an ordered `content[]` array of typed
blocks - with all user-facing text stored as a `map<lang,string>` keyed by ISO 639-1 codes
(`en`, `es`, ...).

```
http_router.c  (route == "/page/<link>")
  │
  ├─ cms_get_entry_by_link(link, lang, &entry)  ── src/db/cms_entries.c
  │     db.entries.findOne({ link, enabled: true })
  │     resolves header.* and content[].text map<lang,string> to `lang`
  │     (configs/settings.conf "Eng"/"Esp" mapped to ISO "en"/"es", default "en"),
  │     falling back to "en" if the requested language is missing
  │     resolves entries.categories[] (ObjectId[]) -> entry_categories.name
  │     (ENTRY_CATEGORIES_COLLECTION) via db.entry_categories.find({_id: {$in: [...]}}),
  │     same lang resolution; entries with no categories get category_count == 0
  │
  ├─ entry_page(&entry, epoch)                   ── src/modules/entry_page/entry_page.c
  │     ├─ entry/entry-header_epoch<N>.html       (header)
  │     ├─ entry/entry-categories_epoch<N>.html   (category "tags", skipped if none)
  │     │     + elements/category/category_epoch<N>.html (one per category)
  │     └─ elements/<type>/<type>_epoch<N>.html  (one per content[] block, in order)
  │
  └─ buildPageWebSite(epoch, entry.header_title, content)
```

`cms_get_entry_by_link()`'s query (`db.entries.findOne({ link, enabled: true })`) has no `type`
filter, but `http_router.c` only renders the result for `entries.type` of `"page"` or `"blog"`
(other types 404) - this is what lets the home blog list's items link to `/page/<link>`.
Unknown `content[].type` values render as empty output, so a page still renders if it contains
a block type this increment doesn't support.

This increment implements 4 content-block types - `tittle`, `paragraph`, `image`, `byline` -
a minimal but useful set: a heading, body text, an image, and an attribution line.

`entries.categories[]` (an `ObjectId[]` referencing `entry_categories._id`, per
`plans/cms-entry-model-plan.md` §2.2) is resolved to category names and rendered as a small "tags"
block under the header. `entry_categories` documents are `{ _id, name: <map<lang,string>> }` -
a separate collection (kept normalized, since categories are shared across entries). An entry
with no `categories` field/empty array renders with no tags block.

**Not yet implemented**: `media`/`media_directories`, the `/blog/` route (incl. filtering by
category), heading levels via `content[].extra_data` for `tittle`, and additional element
types (gallery, table, forms, etc.) - see `develop_docs/plans/cms-entry-model-plan.md` for the
full target schema.

### Home blog list (`/`)

The home page's "Latest Blog Posts" gallery (`modules/home_blog/home_blog.c`) lists `entries`
documents with `type == "blog"`:

```
home_blog(epoch, lang)                          ── src/modules/home_blog/home_blog.c
  │
  ├─ cms_get_blog_entries(lang, &items, &count)  ── src/db/cms_entries.c
  │     db.entries.find({ type: "blog", enabled: true })
  │       .sort({ "header.date": -1 }).limit(HOME_BLOG_LIMIT)
  │     resolves header.* and categories[] to `lang`, reusing the same
  │     resolve_header_fields()/resolve_category_names() helpers as cms_get_entry_by_link()
  │
  ├─ if count == 0: home-blog/empty_epoch<N>.html ("No blog entries found")
  │
  └─ for each item: home-blog-item_epoch<N>.html
        link        -> "/page/<item.link>"
        categories  -> elements/category/category_epoch<N>.html items, concatenated
                        (no entry-categories wrapper)
```

`HOME_BLOG_LIMIT` (`src/db/cms_entries.h`, currently 10) bounds the result size;
`cms_get_blog_entries()` allocates a fixed-size array of that length and never grows it. On a
DB error or if MongoDB is not ready, it returns `*out_count == 0` and the empty-state template
is shown - the home blog list is decorative and must never fail the home page.

### Menu (all pages)

The nav bar (`modules/menu/menu.c`, used by both `buildHomeWebSite()` and `buildPageWebSite()`)
is backed by its own `menu` collection (`MENU_COLLECTION`, `src/db/mongodb_manager.h`),
decoupling navigation from `entries` so items can point anywhere (`/`, `/page/<link>`, external
URLs, ...):

```
menu(current_url, epoch)                        ── src/modules/menu/menu.c
  │
  ├─ cms_get_menu_items(lang, &items, &count)    ── src/db/cms_menu.c
  │     db.menu.find({ enabled: true })
  │       .sort({ order: 1 }).limit(MENU_ITEM_LIMIT)
  │     resolves name (map<lang,string>) to `lang` via resolve_lang_map()
  │     (src/db/bson_lang.c, shared with cms_entries.c)
  │
  └─ for each item: menu-item_epoch<N>.html (link, name, separator)
```

`menu` documents are `{ _id, link, name: <map<lang,string>>, order, enabled }`. `MENU_ITEM_LIMIT`
(`src/db/cms_menu.h`, currently 20) bounds the result size, same fixed-array pattern as
`HOME_BLOG_LIMIT`. If `cms_get_menu_items()` returns 0 items (DB not ready, empty collection, or
a DB error), `menu()` falls back to a single built-in `{"/", "Home"}` item so the nav bar is
never empty - the menu is decorative and must never fail the page.

---

## Login, Dashboard and Logout

A small authentication slice sits alongside the CMS, sharing the same epoch/template
infrastructure via a new generic page shell.

### `html_builder/orchestrator.c`: `buildPageWebSite()`

```c
char *buildPageWebSite(int epoch, const char *page_title, char *html_content);
```

Wraps an arbitrary content fragment (`html_content`, already epoch-rendered) in
`page_epoch<N>.html` (head + menu + footer), resolving `{{PAGE_TITLE}}` to
`<title>page_title</title>` and the menu `%s`. Takes ownership of `html_content` (always
frees it, even on failure). Used by `/login`, `/dashboard` and every error page - `/` keeps
using `buildHomeWebSite()` and its own `container_epoch<N>.html`.

### `src/db/mongodb_manager.c`
- `mongodb_manager_init(uri, db_name)`: calls `sodium_init()` (required once, before any
  libsodium call) then `mongoc_init()` and creates a `mongoc_client_pool_t` from `mongodb_uri`/
  `mongodb_db` (`configs/settings.conf`). Called once from `main()`, after `load_config()` and
  before `server_start()`. Returns `0`/`-1`; on failure the server keeps running but `/login`
  and `/dashboard` degrade to a `503` error page.
- `mongodb_manager_is_ready()`: `1` if init succeeded.
- `mongodb_manager_get_client()` / `mongodb_manager_get_collection(name)`: per-thread client
  from the pool (mongoc clients are not thread-safe), and a convenience collection handle
  against the configured database.
- `mongodb_manager_cleanup()`: destroys the pool and calls `mongoc_cleanup()`. Called once from
  `main()` during shutdown.

### `src/db/auth.c`
- `auth_login_user(email, password)`: looks up `email` in the `users` collection and verifies
  `password` against its `crypto_pwhash_str()` (Argon2id) hash via libsodium. Returns a
  malloc'd 24-char ObjectId hex string on success, or `NULL` for unknown email, wrong password,
  *or* a DB error - all three are indistinguishable to the caller, to avoid user enumeration.

### `src/db/session_manager.c`
- `generate_session_token()`: 32 random bytes (libsodium CSPRNG), hex-encoded (64 chars).
- `create_session(user_id_hex, token, ttl_seconds)`: inserts `{user_id, token, created_at,
  expires_at}` into the `sessions` collection.
- `validate_session_cookie(cookie_header, user_id_out)` (= `extract_session_token()` +
  `validate_session()`): `1` if the `session` cookie names a non-expired session, `0` if
  missing/invalid/expired, `-1` on DB error. `/dashboard` treats `0` and `-1` the same way
  (redirect to `/login`); a `mongodb_manager_is_ready()` check before this handles the
  "DB never connected" case separately with a `503`.
- `destroy_session(token)`: deletes the session document (`/logout`).
- `build_session_cookie_header(token, ttl_seconds, ...)` /
  `build_session_clear_cookie_header(...)`: build `Set-Cookie: session=<token>; HttpOnly;
  Path=/; Max-Age=<n>; SameSite=Lax` (+ `; Secure` when `ssl_enabled=1`), or the same with
  `Max-Age=0` to clear it.

### `src/modules/login/login.c`
```c
char *login(int epoch, const char *error_message);
```
Loads `login_epoch<N>.html`. For `EPOCH_MODERN`, fills the single `%s` with an
`error_message` block (or empty string if `NULL`/`""`). For all other epochs, the template has
no placeholders and is returned verbatim - login is restricted to `EPOCH_MODERN` (see below),
so the other epochs only ever show "this functionality is not available".

### `src/modules/dashboard/dashboard.c`
```c
char *dashboard(int epoch);
```
Loads the static `dashboard_epoch<N>.html` content fragment. The MVP dashboard has no dynamic
placeholders.

### `src/modules/error/error.c`
```c
char *error_content(int epoch, int status_code, const char *message);
```
Loads `error_epoch<N>.html` and fills its 2 `%s` placeholders (status code, message). If
`message` is `NULL`, a default message for `status_code` is used (a static table covering
`400`/`403`/`404`/`405`/`431`/`500`/`503`); unrecognized codes fall back to `"Error"`.

### Routes

| Route | Method | Behavior |
|---|---|---|
| `/login` | `GET` | If `mongodb_manager_is_ready()` and the request carries a valid session cookie, `302 /dashboard`. Otherwise renders `login_epoch<N>.html` via `buildPageWebSite()`. For `EPOCH_MODERN`, a real form; other epochs show "not available". |
| `/login` | `POST` | **`EPOCH_MODERN` only.** Other epochs re-render the "not available" page without any DB access. If `mongodb_manager_is_ready()` is false, `503`. Otherwise `auth_login_user()`; on success, `generate_session_token()` + `create_session()` + `Set-Cookie` + `302 /dashboard`; on failure, re-renders `/login` (`200`) with "Invalid email or password." |
| `/dashboard` | `GET` | If `!mongodb_manager_is_ready()` → `503`. Else `validate_session_cookie()`: valid → `dashboard(epoch)` via `buildPageWebSite()`; otherwise → `302 /login`. |
| `/logout` | `GET` | If a valid session cookie is present, `destroy_session()`; always responds `302 /` with a `Set-Cookie` that clears the cookie (`Max-Age=0`). |

### Epoch restriction (security)

The login form (and all credential handling) is restricted to `EPOCH_MODERN`, enforced
server-side on **both** `GET` and `POST /login` via `resolve_epoch()` - never inferred from
client-supplied data beyond the same `User-Agent`/`force_epoch` resolution used everywhere
else. Other epochs see a static "this functionality is not available" message and never reach
`auth_login_user()`.

See [diagrams/auth-components.puml](../diagrams/auth-components.puml) for the component
diagram and [diagrams/sequence-login-route.puml](../diagrams/sequence-login-route.puml) for
the `POST /login` sequence diagram.

---

## Configuration (`configs/settings.conf`)

```ini
verbose_level=3           # 0=none 1=error 2=warn 3=info 4=debug
http_port=8080
https_port=8443
ssl_enabled=0             # 1 to enable HTTPS
ssl_cert=./ssl/cert.pem
ssl_key=./ssl/key.pem
trusted_proxies=          # comma-separated IPs of trusted reverse proxies
                          # e.g. 127.0.0.1,10.0.0.1
theme=dark                 # active theme under html/themes/<theme>/
lang=Eng                   # content language passed to home_content
public_url=                # public base URL (reserved for future SEO/canonical links)
#force_epoch=3             # force a browser epoch for "/" (-1..3), omit to auto-detect

# MongoDB connection (login/sessions, epoch3 only). If the connection fails
# at startup, /login and /dashboard serve a 503 error page; the rest of the
# site (epoch CMS + static files) is unaffected.
mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder

# Session cookie lifetime, in seconds (default: 24h).
session_ttl_seconds=86400
```

---

## C Standard - C17

The project uses **C17** (`-std=c17`), which provides:

- **`_Atomic` / `stdatomic.h`** - the `running` shutdown flag uses `_Atomic int` instead of `volatile`. `volatile` prevents compiler optimisation but does not guarantee atomicity at the hardware level; `_Atomic` provides formal memory-ordering guarantees for signal handler / main thread coordination.
- **`_Static_assert`** - compile-time validation of buffer sizes and constants in `http_constants.h`. Catches misconfiguration at build time rather than at runtime.
- **C17 over C11** - C17 is a bug-fix revision of C11 with identical syntax. It has better compiler support across all target platforms and signals long-term maintenance intent without introducing breaking changes.

C17 is fully supported by:
- GCC 8+ (Linux)
- Clang 6+ / Apple Clang Xcode 10+ (macOS)

---

## Platform Compatibility

### Target platforms

| Platform | Compiler | Status |
|---|---|---|
| Linux (Debian/Ubuntu/Arch) | GCC 8+ | Primary |
| macOS 12+ (Monterey and later) | Apple Clang (Xcode 14+) | Primary |
| FreeBSD | Clang 6+ | Compatible |

### macOS-specific notes

**`timegm()`** is used in `static_file_server.c` to parse `If-Modified-Since` HTTP dates. Its availability varies by platform:

| Platform | Availability | Macro needed |
|---|---|---|
| Linux (glibc) | Extension | `_GNU_SOURCE` |
| macOS / BSD | Native (BSD heritage) | none |
| Windows | Not available | manual implementation required |

The file uses a platform detection block:
```c
#if defined(__linux__) || defined(__GLIBC__)
#  define _GNU_SOURCE       // exposes timegm() on glibc
#else
#  define _XOPEN_SOURCE 700 // macOS/BSD expose timegm() by default
#endif
```

All other source files use `#define _XOPEN_SOURCE 700` (POSIX.1-2008) uniformly across both platforms.

**`SO_NOSIGPIPE`** is set on each socket where available. On Linux it does not exist (SIGPIPE is suppressed via `MSG_NOSIGNAL` on `send()`); on macOS it exists and prevents SIGPIPE at the socket level. Both paths are handled with `#ifdef SO_NOSIGPIPE`.

### Build on macOS

```bash
brew install openssl cmake
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
cmake --build build
```

### Build on Linux

```bash
sudo apt install cmake gcc libssl-dev   # Debian/Ubuntu
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Concurrency Model

- One main thread runs the `select()` accept loop.
- Each accepted connection is handled by a **detached pthread** (stack: 2 MB).
- A **mutex-protected counter** caps simultaneous active connections at 200.
- A **lock-free IP table** (accessed only from the accept loop) provides per-IP rate limiting with LRU eviction when full.
- No thread pool: threads are created and destroyed per request.

**Known limitations (accepted for initial version):**

| Limitation | Impact | Future path |
|---|---|---|
| `select()` instead of `epoll`/`kqueue` | FD_SETSIZE cap; O(n) scan | `epoll_wait()` on Linux, `kqueue` on macOS |
| Thread-per-connection | ~400 MB RSS at 200 connections | Bounded thread pool with work queue |
| No HTTP/1.1 Keep-Alive | One TCP handshake per resource | Per-connection request loop |
| No HTTP Range requests | No resumable downloads | `Accept-Ranges` + `206 Partial Content` |
| No response compression | Full transfer for compressible assets | `Content-Encoding: gzip` with zlib |

---

## Security Considerations

- `sanitize_path()` uses `realpath()` to prevent directory traversal attacks.
- `X-Real-IP` / `X-Forwarded-For` are honored **only from trusted proxy IPs** configured in `trusted_proxies`; raw peer address is used for all other connections.
- `SIGPIPE` is ignored globally and suppressed at socket level (`SO_NOSIGPIPE` / `MSG_NOSIGNAL`).
- Body reads are capped at `MAX_BODY_SIZE` (10 MiB).
- Header buffer is capped at `RAW_REQUEST_SIZE` (32 KiB); requests exceeding it receive `431`.
- TLS minimum version: TLS 1.2. Cipher suites are explicitly hardened (ECDHE+AESGCM, ChaCha20; no RC4, 3DES, export ciphers).
- Every response includes `X-Content-Type-Options: nosniff` and `X-Frame-Options: SAMEORIGIN`.
- Passwords are stored as `crypto_pwhash_str()` (Argon2id) hashes; `auth_login_user()` never
  reveals whether an email or a password was wrong (same `NULL` result for both, and for DB
  errors).
- Session tokens are 32 random bytes from libsodium's CSPRNG, hex-encoded, stored server-side
  in the `sessions` collection with an `expires_at`. The cookie is `HttpOnly; Path=/;
  SameSite=Lax`, plus `; Secure` when `ssl_enabled=1`.
- The login form and credential checks are restricted to `EPOCH_MODERN`, enforced server-side
  on both `GET` and `POST /login`.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
