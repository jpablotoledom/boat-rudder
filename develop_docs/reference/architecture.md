# Boat Rudder - Architecture

## Overview

**Boat Rudder** is a self-contained HTTP/HTTPS server written in **C17** that doubles as a
retro-compatible CMS. The static-file half grew out of `base-http-server`, a minimal standalone
file server; the CMS half renders every dynamic route into the simplest markup the requesting
browser can understand. The compiled binary, the systemd service and the install directory are
all named `boat-rudder`.

Its core dependency is OpenSSL; the database-backed CMS and the dashboard additionally depend on
**libmongoc** (MongoDB) and **libsodium** (Argon2id password hashing, CSPRNG). It serves files
from a configurable root directory, supports concurrent connections via POSIX threads, and
optionally enables TLS, targeting **Linux and macOS** as primary platforms.

> **Naming.** Boat Rudder is the software: the binary, the source tree, the `boat-rudder__*` CSS
> namespace and every document under `develop_docs/`. A *site* built with it is a separate thing
> - its own MongoDB database, theme and content - and never appears in the source tree. Anything
> a visitor reads (site name, banner, page titles) is content, not code; the defaults shipped in
> the templates say "Boat Rudder" until a site overrides them.

---

## Document map

This document covers the **server foundation**: process lifecycle, sockets, TLS, the router's
mechanics, the static file server and the shared utilities. The layers built on top have their
own documents:

| Document | Covers |
|---|---|
| [rendering.md](rendering.md) | Epochs, the per-epoch template convention, and every public page: home, blog list, category menu, CMS entries, nav menu |
| [dashboard.md](dashboard.md) | Login/sessions/roles and the admin area: entries listing, Categories, Languages, Menu, Users, plus the routing guards |
| [entry-editor.md](entry-editor.md) | The AJAX entry editor at `/dashboard/entries/<id>/edit` |
| [media-admin.md](media-admin.md) | The media library at `/dashboard/media`, the image optimizer and `/gallery/<id>` |
| [configuration.md](configuration.md) | Every `configs/settings.conf` key - the single reference for configuration |
| [data-flow.md](data-flow.md) | The same system read as one request's journey, start to finish |
| [style-guide.md](style-guide.md) | C conventions and the security rules that are non-negotiable here |

---

## Directory Structure

```
boat-rudder/
├── src/
│   ├── main.c                           # Entry point
│   ├── web_server/
│   │   ├── server_listener.c/h          # Socket setup and main accept loop
│   │   ├── connection.c/h               # Unified read/write abstraction (plain + TLS)
│   │   ├── connection_thread.c/h        # Per-connection POSIX thread (30 s socket timeout)
│   │   ├── tls_context.c/h              # OpenSSL context lifecycle
│   │   ├── http_router.c/h              # HTTP routing and dispatch
│   │   ├── http_request_parser.c/h      # Raw HTTP/1.1 request parser
│   │   ├── http_constants.h             # Buffer sizes and compile-time assertions
│   │   └── utils/
│   │       ├── static_file_server.c/h   # Static file serving (MIME, cache, security headers)
│   │       ├── url_parser.c/h           # URL path / query-string splitter + QueryParam
│   │       ├── multipart_parser.c/h     # multipart/form-data body parser (file uploads)
│   │       └── memmem_compat.h          # Portable memmem() shim (used by multipart parser)
│   ├── html_builder/
│   │   └── orchestrator.c/h             # buildHomeWebSite() / buildPageWebSite() /
│   │                                    # buildPageWebSiteAtUrl() / buildBlogListWebSiteAtUrl() /
│   │                                    # buildEntryWebSiteAtUrl(): assemble pages per epoch
│   ├── modules/
│   │   ├── container/container.c/h      # Page shell (head/body wrapper) per epoch
│   │   ├── menu/menu.c/h                # Nav menu with active-item highlighting per epoch
│   │   ├── slider/slider.c/h            # Hero/banner block per epoch
│   │   ├── home_content/home_content.c/h# Home page body content per epoch
│   │   ├── home_blog/home_blog.c/h      # "Latest Blog Posts" gallery per epoch
│   │   ├── blog_list/blog_list.c/h      # /blog and /blog/category/<slug> listing per epoch
│   │   ├── category_menu/category_menu.c/h # Category sub-menu bar under the navbar (blog pages)
│   │   ├── entry_page/entry_page.c/h    # Public CMS entry renderer (content[] blocks only)
│   │   ├── entry_editor/               # AJAX entry editor (dashboard, EPOCH_MODERN only)
│   │   │   ├── entry_editor.c/h        # Page renderer (meta + header sidebars + blocks)
│   │   │   └── entry_editor_blocks.c/h # Per-block editor form renderer
│   │   ├── media_admin/media_admin.c/h  # /dashboard/media page + directory/photo rendering
│   │   ├── entries_admin/entries_admin.c/h # Entries table rows (dashboard)
│   │   ├── categories_admin/            # /dashboard/categories CRUD
│   │   ├── languages_admin/             # /dashboard/languages CRUD
│   │   ├── menu_admin/                  # /dashboard/menu CRUD
│   │   ├── users_admin/                 # /dashboard/users CRUD
│   │   ├── login/login.c/h              # /login form (epoch3 only) per epoch
│   │   ├── dashboard/dashboard.c/h      # /dashboard admin home (entries table, role-based nav)
│   │   └── error/error.c/h              # Centralized error page content per epoch
│   ├── db/
│   │   ├── mongodb_manager.c/h          # MongoDB client pool lifecycle (init/cleanup/get)
│   │   ├── auth.c/h                     # Email/password verification (Argon2id via libsodium)
│   │   ├── session_manager.c/h          # Session tokens, cookies, sessions collection
│   │   ├── cms_entries.c/h              # Public entry reads (blog list, entry by link)
│   │   ├── cms_entries_admin.c/h        # Admin entry reads/writes (editor)
│   │   ├── cms_categories.c/h           # entry_categories CRUD
│   │   ├── cms_languages.c/h            # languages collection + default resolution
│   │   ├── cms_menu.c/h                 # menu collection CRUD
│   │   ├── cms_users_admin.c/h          # users CRUD + cms_get_username_by_id()
│   │   ├── cms_media.c/h                # media + media_directories collections
│   │   └── cms_media_galleries.c/h      # media_galleries collection (gallery blocks)
│   └── utils/
│       ├── config_loader.c/h            # INI-style config file parser
│       ├── log.c/h                      # Leveled, thread-safe logging macros
│       ├── http_utils.c/h               # MIME detection, URL encode/decode, path sanitizer,
│       │                                # trusted proxy check
│       ├── detect_epoch.c/h             # User-Agent → epoch heuristic
│       ├── read_file.c/h                # read_file_to_string(): malloc'd file contents
│       ├── template_utils.c/h           # render_template, str_replace_first, str_append,
│       │                                # image_url_variant(), slugify()
│       ├── json_utils.c/h               # json_escape_alloc(): escape a string for a JSON literal
│       ├── generate_url_theme.c/h       # Builds ./html/themes/<theme>/... paths per epoch
│       └── build_epoch_response.c/h     # Wraps rendered HTML with epoch-correct headers,
│                                         # plus status-line and redirect variants
├── scripts/
│   ├── image-optimizer.sh               # Generates 5 image variants per upload via ImageMagick
│   ├── compile_debug.sh / compile_prod.sh / run_debug.sh / install.sh / uninstall.sh
│   ├── create_local_cert.sh             # Self-signed TLS certificate for local development
│   └── mongodb_start.sh / mongodb_dump.sh / mongodb_restore.sh
├── configs/
│   └── settings.conf                    # Runtime configuration
├── html/                                 # Static + templated content root (themes/, assets/)
│   └── content/posts/                   # Uploaded media files (username/dirname/filename_variant.ext)
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
- Sets socket timeouts (30 s read + write) - raised from 5 s to allow large file uploads.
- Performs `SSL_accept()` if the connection is HTTPS.
- Calls `http_route()` passing the appropriate `read_func_t`.
- Always decrements the global `active_connections` counter on exit.

### `web_server/tls_context.c`
- `tls_create_context(cert, key)` - loads PEM certificate and private key into a new `SSL_CTX`.
- Enforces **TLS 1.2 minimum** via `SSL_CTX_set_min_proto_version`.
- Applies hardened cipher suites for both TLS 1.2 (ECDHE+AESGCM/ChaCha20) and TLS 1.3.
- `tls_free_context(ctx)` - frees the context.

### `web_server/http_router.c`
- Reads the raw HTTP request headers into a fixed 32 KiB buffer; responds `431` if exceeded.
- **Full body reading**: after headers are parsed, reads the body to completion using `Content-Length` - allocates a buffer of the declared size (capped at `MAX_BODY_SIZE` = 10 MiB) and loops until all bytes are received. This supports both small form posts and large multipart file uploads.
- Parses headers with `parse_http_request()`.
- Extracts the real client IP: honors `X-Real-IP` / `X-Forwarded-For` **only when the peer IP matches the `trusted_proxies` config list**, falling back to the raw socket address for all other peers.
- Validates the request line (400 Bad Request on failure).
- `resolve_epoch(req)`: the configured `force_epoch` override (if in `-1..3`), otherwise `detect_epoch(User-Agent)`.
- `get_query_param(params, count, key)`: looks up a parsed query parameter by name (used by `/gallery/<id>?img=N`, `/dashboard/api/media/contents?directory=&start=&end=`).
- Routes `GET`/`HEAD`:
  - `/` → dynamic, epoch-aware home page (see [rendering.md](rendering.md)).
  - `/login` → renders `login_epoch<N>.html` via `buildPageWebSite()`.
  - `/dashboard` → requires a valid session cookie, otherwise redirects to `/login`.
  - `/dashboard/media` → media admin page (session required). See [media-admin.md](media-admin.md).
  - `/dashboard/api/media/contents` → paginated media grid (HTML fragment, session required).
  - `/dashboard/api/media/modal` → media picker modal (HTML, session required).
  - `/gallery/<id>` → public gallery page per epoch; epoch 3: thumbnail grid with lightbox; epochs 1-2: paginated viewer (main image + prev/next + thumbnail strip); epochs -1/0: text links.
  - `/blog` → blog list page via `blog_list()` + `buildBlogListWebSiteAtUrl(epoch, title, content, "/blog", category_menu)`.
  - `/blog/category/<slug>` → blog list filtered by category via `blog_list_category()`; `<slug>` is matched against `slugify(category.name)` over `cms_get_categories()`, `404` if no category matches. Same wrapper, with the matching item marked selected in the category menu.
  - `/blog/<link>` → CMS entry via `serve_cms_entry()`, passes `"/blog"` as active menu URL.
  - `/page/<link>` → CMS entry via `serve_cms_entry()`, passes `"/page/<link>"` as active menu URL.
  - `/logout` → destroys the session and redirects to `/`.
  - anything else → `serve_static_file()`, passing the `If-Modified-Since` header for cache validation; a non-zero return code (`403`/`404`/`500`) is rendered via `send_error_response()`.
- `POST /login` → epoch3 only; other epochs re-render the "not available" `login_epoch<N>.html` without touching the database.
- `POST /dashboard/api/media/directory` → create media directory.
- `POST /dashboard/api/media/directory/rename` → rename (renames physical dir via `rename()`).
- `POST /dashboard/api/media/directory/delete` → delete (removes physical dir via `rmdir()`).
- `POST /dashboard/api/media/upload` → multipart file upload: saves to `html/content/posts/<username>/<dirname>/`, runs `scripts/image-optimizer.sh`, inserts into `media` collection.
- `POST /dashboard/api/entries/<id>/content` → after saving blocks, for each `gallery` block calls `cms_upsert_media_gallery()` to sync the `media_galleries` collection and stores the gallery `_id` in the block's `extra_data`.
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

### `web_server/utils/multipart_parser.c`
- Parses `multipart/form-data` POST bodies for file uploads.
- Extracts the boundary from the `Content-Type` header, iterates parts, and for each part returns: field name, optional filename, content-type header, and a pointer+length into the original body buffer (zero-copy - no allocation of file data).
- `parse_multipart(body, body_len, content_type)` → `MultipartResult*`; `multipart_find(result, name)` looks up a part by field name.
- `memmem_compat.h`: portable `memmem()` implementation used internally by the parser.

### `utils/template_utils.c` (additions)
- `image_url_variant(url, suffix)` → new malloc'd URL with `suffix` inserted before the file extension. Example: `image_url_variant("/content/posts/user/dir/photo.jpg", "_small")` → `"/content/posts/user/dir/photo_small.jpg"`. Used by `home_blog`, `blog_list`, `entries_admin`, and `entry_page` to generate thumbnail and full-size URLs from the base URL stored in `header.image_url` / `content[].text`.
- `slugify(name)` → new malloc'd, lowercased URL slug: `[a-z0-9]` kept as-is, `A-Z` lowercased, spaces/`-`/`_` collapsed into a single `-`, every other byte dropped, trailing `-` trimmed. Example: `slugify("Retro Hardware")` → `"retro-hardware"`. Used to build and match `/blog/category/<slug>` URLs (`cms_entries.c`, `category_menu.c`, `http_router.c`). Slugs are **derived, not stored** - the category's `name` in the current content language is the source of truth, so a category renamed in the admin changes its public URL.

### `utils/json_utils.c`
- `json_escape_alloc(src)` → new malloc'd copy of `src` escaped for embedding inside a JSON string literal (escapes `"`, `\` and control characters; UTF-8 multi-byte sequences pass through unchanged). Used by the entry editor's AJAX endpoints to embed rendered HTML in a JSON response (`/dashboard/api/entries/<id>/blocks`).

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
