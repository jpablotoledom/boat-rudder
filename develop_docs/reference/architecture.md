# Boat Rudder - Architecture

## Overview

**Boat Rudder** is a self-contained HTTP/HTTPS server written in **C17** that doubles as a
retro-compatible CMS. The static-file half grew out of `base-http-server`, a minimal standalone
file server; the CMS half renders every dynamic route into the simplest markup the requesting
browser can understand. The compiled binary, the systemd service and the install directory are
all named `boat-rudder`.

Its core dependency is OpenSSL; the database-backed CMS and the dashboard additionally depend on
**libmongoc** (MongoDB) and **libsodium** (Argon2id password hashing, CSPRNG); retro epochs'
QR codes need **libqrencode**, and analytics can use **libmaxminddb** (optional) for country
detection. It serves files from a configurable root directory, supports concurrent connections via
POSIX threads, optionally enables TLS, and can run a built-in **WAP 1.x gateway** over UDP for real
vintage phones, targeting **Linux and macOS** as primary platforms. The full dependency list,
including vendored code, is in [third-party.md](third-party.md).

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
| [rendering.md](rendering.md) | Epochs, the per-epoch template convention, the response layer, and every public page: home, blog list, category menu, CMS entries, nav menu |
| [routes.md](routes.md) | **Every route**: method, guard, epochs, module, response |
| [data-model.md](data-model.md) | **Every MongoDB collection**: schema, readers/writers, relationships, indexes |
| [dashboard.md](dashboard.md) | Login/sessions/roles and the admin area: entries listing, Categories, Languages, Menu, Users, plus the routing guards |
| [entry-editor.md](entry-editor.md) | The AJAX entry editor at `/dashboard/entries/<id>/edit` |
| [media-admin.md](media-admin.md) | The media library at `/dashboard/media`, the image optimizer and `/gallery/<id>` |
| [themes.md](themes.md) | Theme resolution, `html/templates/` vs `html/themes/`, Settings: colors, banner/footer/logo, editable CSS, preview |
| [fonts.md](fonts.md) | The uploaded font library used by the epoch 3 logo |
| [analytics.md](analytics.md) | Visit tracking, GeoIP, the `/dashboard/analytics` report |
| [wap-gateway.md](wap-gateway.md) | The UDP WAP 1.x gateway and WBXML compiler |
| [qr-and-short-links.md](qr-and-short-links.md) | QR codes for retro epochs and `/qr/<code>` short links |
| [code-highlighting.md](code-highlighting.md) | Server-side syntax highlighting for `code-text` blocks |
| [templates-catalog.md](templates-catalog.md) | Inventory of every template, its arguments and markers |
| [configuration.md](configuration.md) | Every `configs/settings.conf` key - the single reference for configuration |
| [security.md](security.md) | Every defense (anti-DDoS, TLS, sanitizers, sessions, roles) and known gaps |
| [data-flow.md](data-flow.md) | The same system read as one request's journey, start to finish |
| [scripts.md](scripts.md) / [migrations.md](migrations.md) | Build, run, install, backup scripts; database migrations |
| [third-party.md](third-party.md) | Vendored, ported and linked third-party code and data |
| [style-guide.md](style-guide.md) | C conventions and the security rules that are non-negotiable here |

---

Diagrams: [components.puml](../diagrams/components.puml) (every component and its dependencies),
[class-structures.puml](../diagrams/class-structures.puml) (key data structures).

---

## Directory Structure

```
boat-rudder/
├── src/
│   ├── main.c                           # Entry point: config, MongoDB, GeoIP, listeners, gateway
│   ├── web_server/
│   │   ├── server_listener.c/h          # Sockets, accept thread, anti-DDoS, IP-table cleanup thread
│   │   ├── connection.c/h               # Unified read/write abstraction (plain + TLS)
│   │   ├── connection_thread.c/h        # Per-connection POSIX thread (connection_io_timeout_secs)
│   │   ├── tls_context.c/h              # OpenSSL context lifecycle
│   │   ├── http_router.c/h              # HTTP read/parse, per-request state, analytics, dispatch
│   │   ├── http_request_parser.c/h      # Raw HTTP/1.x (and 0.9) request parser
│   │   ├── http_constants.h             # Buffer sizes and compile-time assertions
│   │   └── utils/
│   │       ├── static_file_server.c/h   # Static file serving (MIME, cache, security headers)
│   │       ├── url_parser.c/h           # URL path / query-string splitter + QueryParam
│   │       ├── multipart_parser.c/h     # multipart/form-data body parser (file uploads)
│   │       └── memmem_compat.h          # Portable memmem() shim (used by multipart parser)
│   ├── wap_gateway/                     # WAP 1.x gateway (see wap-gateway.md)
│   │   ├── wap_gateway.c/h              # UDP sockets, WTP/WSP framings, rate limit, loopback fetch
│   │   └── wbxml.c/h                    # WML parser, WBXML compiler, single-packet pagination
│   ├── html_builder/
│   │   ├── orchestrator.c/h             # buildHomeWebSite() / buildPageWebSite() /
│   │   │                                # buildPageWebSiteAtUrl() / buildBlogListWebSiteAtUrl() /
│   │   │                                # buildEntryWebSiteAtUrl(): assemble pages per epoch
│   │   └── page_layout.c/h              # page_layout_wrap(): layout, footer, footer logo, theme
│   │                                    # colors/fonts, site name, (epoch 3) lightbox/home-modal
│   ├── modules/
│   │   ├── menu/menu.c/h                # Nav menu (logo, items, language, theme, user; WML compact)
│   │   ├── mainbanner/mainbanner.c/h    # Home banner per epoch (theme DB override or file)
│   │   ├── home_content/home_content.c/h# Home page body content per epoch
│   │   ├── blog_list/blog_list.c/h      # Home section, /blog and category listing, one component
│   │   ├── category_menu/category_menu.c/h # Category sub-menu bar (blog pages; WML compact)
│   │   ├── entry_page/entry_page.c/h    # Public CMS entry renderer + entry_page_render_block()
│   │   ├── language_page/               # /language page + language_sanitize_return()
│   │   ├── theme_page/                  # /theme page (epoch 1-3 theme picker)
│   │   ├── entry_editor/                # AJAX entry editor (dashboard, EPOCH_MODERN only)
│   │   │   ├── entry_editor.c/h         # Page renderer (meta + header sidebars + blocks)
│   │   │   └── entry_editor_blocks.c/h  # Per-block editor form renderer
│   │   ├── media_admin/media_admin.c/h  # /dashboard/media page + directory/photo rendering
│   │   ├── entries_admin/entries_admin.c/h # Entries table rows (dashboard)
│   │   ├── categories_admin/            # /dashboard/categories CRUD
│   │   ├── languages_admin/             # /dashboard/languages CRUD
│   │   ├── menu_admin/                  # /dashboard/menu CRUD
│   │   ├── users_admin/                 # /dashboard/users CRUD
│   │   ├── site_settings_admin/         # /dashboard/settings: name, themes, banner/footer/logo,
│   │   │                                # CSS, preview (see themes.md)
│   │   ├── fonts_admin/                 # /dashboard/settings/fonts (see fonts.md)
│   │   ├── analytics/                   # analytics.c (visit tracking), geoip.c (optional GeoLite2)
│   │   ├── analytics_view/              # /dashboard/analytics report + country_continent.c
│   │   ├── login/login.c/h              # /login form (epoch3 only) per epoch
│   │   ├── dashboard/dashboard.c/h      # /dashboard admin home (entries table, role-based nav)
│   │   └── error/error.c/h              # Centralized error page content per epoch
│   ├── db/                              # Every MongoDB access (see data-model.md)
│   │   ├── mongodb_manager.c/h          # Client pool lifecycle, mongodb_manager_aggregate()
│   │   ├── auth.c/h                     # Email/password verification (Argon2id via libsodium)
│   │   ├── session_manager.c/h          # Session tokens, cookies, sessions collection
│   │   ├── bson_lang.c/h                # map<lang,string> resolution, Eng/Esp -> en/es
│   │   ├── cms_entries.c/h              # Public entry reads (blog list, entry by link, drafts)
│   │   ├── cms_entries_admin.c/h        # Admin entry reads/writes (editor)
│   │   ├── cms_categories.c/h           # entry_categories CRUD
│   │   ├── cms_languages.c/h            # languages collection + default resolution
│   │   ├── language_catalog.c/h         # Curated list of addable languages (code/name/native/abbr)
│   │   ├── cms_menu.c/h                 # menu collection CRUD
│   │   ├── cms_users_admin.c/h          # users CRUD + cms_get_username_by_id()
│   │   ├── cms_media.c/h                # media + media_directories collections
│   │   ├── cms_media_galleries.c/h      # media_galleries collection (gallery blocks)
│   │   ├── cms_site_settings.c/h        # site_settings singleton (site name, active theme)
│   │   ├── cms_themes.c/h               # themes collection (colors, banner/footer/logo, CSS)
│   │   ├── cms_fonts.c/h                # fonts collection (uploaded font library)
│   │   └── short_links.c/h              # short_links collection (QR short codes)
│   ├── third_party/
│   │   └── stb_image.h                  # Vendored PNG/JPEG decoder (see third-party.md)
│   └── utils/
│       ├── config_loader.c/h            # INI-style config file parser
│       ├── log.c/h                      # Leveled, thread-safe logging macros
│       ├── http_utils.c/h               # MIME detection, URL encode/decode, HTML encode,
│       │                                # path sanitizer, trusted proxy check
│       ├── detect_epoch.c/h             # User-Agent → epoch heuristic, epoch_to_index()
│       ├── request_lang.c/h             # Per-request content language + request path
│       ├── request_theme.c/h            # Per-request theme (?theme= → cookie → DB → config)
│       ├── request_user.c/h             # Per-request signed-in user's display name
│       ├── request_charset.c/h          # Per-request "real browser needs Latin-1" (Cello)
│       ├── request_code_lines.c/h       # Per-request ?code_lines=off
│       ├── request_wml.c/h              # Per-request WML target + ?wml_pages=all
│       ├── theme_catalog.c/h            # Theme discovery/validation under html/themes/
│       ├── generate_url_theme.c/h       # Template path: html/themes/<theme>/ → html/templates/
│       ├── code_highlight.c/h           # Server-side syntax highlighting (code-highlighting.md)
│       ├── ua_parser.c/h                # User-Agent → browser/OS keys for analytics
│       ├── qr_generator/                # QR codes for retro epochs (GIF/WBMP/text)
│       ├── image_convert/               # PNG/JPEG → 1-bit WBMP (epoch -1 logos)
│       ├── wbmp_writer.c/h              # WBMP writer shared by QR and logo conversion
│       ├── read_file.c/h                # read_file_to_string(): malloc'd file contents
│       ├── template_utils.c/h           # render_template, str_replace_first/_all, str_append,
│       │                                # image_url_variant(), slugify(), capitalize_first()
│       ├── json_utils.c/h               # json_escape_alloc(): escape a string for a JSON literal
│       ├── image_size.c/h               # image_intrinsic_width(): GIF header width, for
│       │                                # sizing retro-epoch images without CSS
│       ├── category_tags.c/h            # category_tags_render(): shared category-tag loop
│       └── build_epoch_response.c/h     # Epoch-correct headers, link re-tagging, WML pagination,
│                                        # Latin-1 transcoding; redirect/JSON/CSS variants
├── html/                                # Static + templated content root (CLI root directory)
│   ├── templates/                       # Shared, theme-agnostic templates (templates-catalog.md)
│   ├── themes/<theme>/                  # Per-theme templates, assets, styles_epoch3.css
│   ├── assets/                          # Site-wide assets (fonts/, slide/)
│   └── content/                         # posts/<user>/<dir>/ uploads, qr/ cache (gitignored)
├── scripts/
│   ├── image-optimizer.sh               # Generates 5 image variants per upload via ImageMagick
│   ├── compile_debug.sh / compile_prod.sh / run_debug.sh / install.sh / uninstall.sh / clean.sh
│   ├── create_local_cert.sh             # Self-signed TLS certificate for local development
│   ├── mongodb_start.sh / mongodb_dump.sh / mongodb_restore.sh
│   ├── boat-rudder.service              # systemd unit installed by install.sh
│   ├── boat-rudder.logrotate            # /etc/logrotate.d/boat-rudder, installed by install.sh
│   ├── migrations/                      # One-off mongosh data migrations (migrations.md)
│   └── show/                            # banner/divbar snippets sourced by the scripts
├── configs/
│   └── settings.conf                    # Runtime configuration (configuration.md)
├── data/
│   └── GeoLite2-Country.mmdb            # Optional GeoIP database (analytics.md)
├── ssl/                                 # TLS certificate and key (optional)
├── boat_rudder_builder.sh               # Entry point for every script (scripts.md)
└── CMakeLists.txt                       # Build definition
```

Also tracked but not part of the build or runtime: `design/` (`.xcf` image sources for theme
artwork), `data/mongo_example.c` and `data/querys.json` (scratch examples), and `db_backup/`,
`db_backup_clean/` (MongoDB dumps - see [security.md](security.md#known-gaps)).

---

## Layers

```
┌────────────────────────────────────────────────────────┐
│                      main.c                            │  ← Entry point: config, signals, lifecycle
├───────────────────────────────────┬────────────────────┤
│   web_server/server_listener      │   wap_gateway/     │  ← accept thread + cleanup thread │ UDP thread
├──────────────┬────────────────────┤  (loopback HTTP ──►│     into the HTTP listener)
│ connection.c │   tls_context.c    │                    │  ← I/O abstraction  │  TLS context
├──────────────┴────────────────────┴────────────────────┤
│              connection_thread.c                       │  ← Per-request pthread + TLS handshake
├────────────────────────────────────────────────────────┤
│              http_router.c                             │  ← HTTP parse, request_*_set(), analytics,
│                                                        │    route dispatch
├──────────────────────────┬─────────────────────────────┤
│ modules/ + html_builder/ │  utils/static_file_server   │  ← Rendering  │  File server
├──────────────────────────┴─────────────────────────────┤
│  db/ (MongoDB)   │  utils/ (log, config, templates…)   │  ← Data access │ Cross-cutting utilities
└────────────────────────────────────────────────────────┘
```

---

## Component Descriptions

### `main.c`
- Accepts `[-c <config>] <root_directory>` arguments.
- Loads configuration via `config_loader`.
- Registers POSIX signal handlers (`SIGINT`, `SIGTERM` → graceful shutdown; `SIGCHLD` → zombie reap; `SIGPIPE` → ignored).
- Uses `_Atomic int running` for the shutdown flag - formally correct for signal handler / main thread coordination.
- Startup order: `load_config()` → `mongodb_manager_init()` (+ `cms_languages_ensure_seeded()`)
  → `geoip_init(GEOIP_DEFAULT_DB_PATH)` → signal handlers → `server_start()` →
  `wap_gateway_start()`. The gateway starts **after** the HTTP listener because it fetches every
  page from it over loopback; a gateway failure is logged and the server continues.
- `server_start()` returns once the listeners are up (the accept loop runs on its own thread),
  so `main()` then sleeps in a 1 s loop until `running == 0`.
- Shutdown order: `wap_gateway_stop()` → `server_stop()` → `geoip_cleanup()` →
  `mongodb_manager_cleanup()`. See [data-flow.md](data-flow.md#7-shutdown).

### `web_server/server_listener.c`
- `server_start()`: `calloc`s the per-IP table (`ddos_max_ips` entries), starts the
  **IP-table cleanup thread**, creates one or two TCP listeners (HTTP and optionally HTTPS),
  creates the TLS context, and starts the **accept thread** - then returns.
- `accept_loop_thread()`: a `select()` loop with a **1 s timeout** (closing the listening fds
  from another thread doesn't reliably wake a blocked `select()`, so the timeout bounds shutdown
  to ~1 s).
- Enforces, at `accept()` time (all values from `settings.conf`, see
  [configuration.md](configuration.md#anti-ddos-and-timeouts)):
  - **Global connection limit** (`ddos_max_connections`, default 200) via a mutex-protected counter.
  - **Per-IP rate limiting** (`ddos_rate_limit` connections per `ddos_rate_window_secs`, default
    500 per 5 s) with a temporary block of 2 × the window and **LRU eviction** when the table is
    full. The table is guarded by `ip_table_mutex`.
  - Rejected plain-HTTP connections get a best-effort **`429 Too Many Requests`**; rejected HTTPS
    connections are closed without a handshake (see [security.md](security.md#connection-level-defenses)).
- `ip_table_cleanup_thread()`: every `ddos_cleanup_interval_secs`, lifts expired bans and frees
  entries idle for `ddos_ip_stale_secs`; woken early by `server_stop()` through a condition variable.
- `server_stop()`: stops and joins the accept thread, waits (≤ 10 s, via `pthread_cond_t`) for
  active connections before freeing `ssl_ctx` - preventing a use-after-free with in-flight TLS
  handshakes - then stops the cleanup thread and frees the table.
- For each accepted connection, allocates `thread_args`, optionally wraps the socket in `SSL_new()`, and spawns a detached `pthread` (2 MiB stack).

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
- Sets `SO_RCVTIMEO`/`SO_SNDTIMEO` to `connection_io_timeout_secs` (default **5 s**). The
  timeout applies per read/write call, not to the whole transfer, so large uploads that keep
  moving are unaffected; a client that stalls longer is dropped (slow-loris defense).
- Performs `SSL_accept()` if the connection is HTTPS.
- Calls `http_route()` passing the appropriate `read_func_t`.
- Always decrements the global `active_connections` counter on exit.

### `web_server/tls_context.c`
- `tls_create_context(cert, key)` - loads PEM certificate and private key into a new `SSL_CTX`.
- Enforces **TLS 1.2 minimum** via `SSL_CTX_set_min_proto_version`.
- Applies hardened cipher suites for both TLS 1.2 (ECDHE+AESGCM/ChaCha20) and TLS 1.3.
- `tls_free_context(ctx)` - frees the context.

### `wap_gateway/`
- Optional second listener (UDP, `wap_gateway_*` keys): answers WAP 1.x requests in two
  framings (Palm Rover WTP on 49300, connectionless WSP on 9200), fetches the page from this
  server over loopback HTTP with `?preview_epoch=-1&wml_pages=all`, compiles the WML to WBXML
  (`wbxml.c`) in single-packet pages of `WAP_PAGE_BYTES` = 860 bytes, and replies.
- Its own thread; per-source-IP rate limit against UDP amplification.
- Full description: [wap-gateway.md](wap-gateway.md).

### `modules/analytics/` and `modules/analytics_view/`
- `analytics_track_visit()` (called by the router for every request) increments per-day
  aggregates in `page_visits_daily` / `entry_visits_daily`; `geoip.c` resolves the country with
  libmaxminddb when built with `HAVE_MAXMINDDB`; `ua_parser.c` classifies browser/OS.
- `analytics_view()` renders `/dashboard/analytics`.
- Full description: [analytics.md](analytics.md).

### `web_server/http_router.c`
- Reads the raw HTTP request headers into a fixed 32 KiB buffer; responds `431` if exceeded.
- **Legacy client tolerance** (added after real period-browser testing, not a spec requirement
  for any modern client): the header-read loop recognizes a true HTTP/0.9 "simple request" - a
  request line with no version token at all - as complete the moment that line arrives, rather
  than waiting for a `\r\n\r\n` such a client never sends. A versioned request line followed by a
  few headers (or none) and no blank-line terminator gets one `poll()`-bounded 400ms window to
  prove more data is still coming before the loop gives up waiting for one - long enough not to
  mistake a slow, well-formed multi-packet request for a truncated one. See
  [rendering.md](rendering.md#legacy-http-compatibility) for
  the real client behavior (Cello 1.x) this was fixed against.
- **Full body reading**: after headers are parsed, reads the body to completion using `Content-Length` - allocates a buffer of the declared size and loops until all bytes are received. A declared length above `MAX_BODY_SIZE` (10 MiB) is answered `413` before reading. This supports both small form posts and large multipart file uploads.
- Parses headers with `parse_http_request()`.
- Extracts the real client IP: honors `X-Real-IP` / `X-Forwarded-For` **only when the peer IP matches the `trusted_proxies` config list**, falling back to the raw socket address for all other peers.
- Validates the request line (400 Bad Request on failure).
- `resolve_epoch(req)`: the configured `force_epoch` override (if in `-1..3`), otherwise a valid `?preview_epoch=N` anywhere in the query string (the dashboard preview), otherwise `detect_epoch(User-Agent)`.
- **Per-request state**, set once before routing and read anywhere below through thread-local
  getters: `request_lang_set()` (`?lang=` / cookie / default), `request_path_set()`,
  `request_user_set()` (session → display name), `request_theme_set()` (`?theme=` / cookie /
  `site_settings` / config), `request_charset_set()` (from the **real** User-Agent, independent
  of `force_epoch`/`preview_epoch`), `request_code_lines_set()` (`?code_lines=off`),
  `request_wml_set()` (raw target + `?wml_pages=`).
- **Analytics**: `analytics_track_visit()` is called for every request before dispatch; it
  filters itself (GET only, no bots, no static assets, nothing under `/dashboard`) - see
  [analytics.md](analytics.md).
- `get_query_param(params, count, key)`: looks up a parsed query parameter by name (used by `/gallery/<id>?img=N`, `/dashboard/api/media/contents?directory=&start=&end=`, `/dashboard/analytics?period=…`, …).
- The complete route table, with guards and handlers, is in [routes.md](routes.md). Summary of
  `GET`/`HEAD`:
  - `/` → dynamic, epoch-aware home page (see [rendering.md](rendering.md)).
  - `/login` → renders `login_epoch<N>.html` via `buildPageWebSite()`.
  - `/dashboard` → requires a valid session cookie, otherwise redirects to `/login`.
  - `/dashboard/media` → media admin page (session required). See [media-admin.md](media-admin.md).
  - `/dashboard/api/media/contents` → paginated media grid (HTML fragment, session required).
  - `/dashboard/api/media/modal` → media picker modal (HTML, session required).
  - `/gallery/<id>` → public gallery page per epoch; epoch 3: thumbnail grid with lightbox; epochs 1-2: paginated viewer (main image + prev/next + thumbnail strip); epochs -1/0: a QR code encoding this same page's public URL, since neither a WAP phone nor a text-only browser can show the photos regardless of layout - see [rendering.md](rendering.md).
  - `/blog` → blog list page via `blog_list()` + `buildBlogListWebSiteAtUrl(epoch, title, content, "/blog", category_menu)`.
  - `/blog/category/<slug>` → blog list filtered by category via `blog_list_category()`; `<slug>` is matched against `slugify(category.name)` over `cms_get_categories()`, `404` if no category matches. Same wrapper, with the matching item marked selected in the category menu.
  - `/blog/categories` → full category menu page (WML's compact `[Categories]` target).
  - `/blog/<link>` → CMS entry via `serve_cms_entry()`, passes `"/blog"` as active menu URL. Drafts are served (marked `[Draft]`, `no-store`) only to a visitor with a dashboard session (`include_drafts`).
  - `/page/<link>` → CMS entry via `serve_cms_entry()`, passes `"/page/<link>"` as active menu URL. Same draft rule. WML pagination is no longer a route concern: every epoch −1 response is paginated by `build_epoch_response.c` (`?__page=N`, see [rendering.md](rendering.md#wml-pagination)).
  - `/menu`, `/language`, `/language/set`, `/theme`, `/theme/set` → nav menu page (WML), language and theme pickers, and their cookie-setting redirects.
  - `/qr/<code>`, `/youtube-qr/<id>`, `/image-qr/<code>` → short-link redirect and epoch-0 QR pages ([qr-and-short-links.md](qr-and-short-links.md)).
  - `/themes/<key>/styles_epoch3.css` → the theme's epoch 3 CSS, possibly DB-overridden ([themes.md](themes.md#6-editable-epoch-3-stylesheet)).
  - `/dashboard/settings*`, `/dashboard/analytics`, `/dashboard/api/theme-assets/list` → see [themes.md](themes.md), [fonts.md](fonts.md), [analytics.md](analytics.md).
  - `/logout` → destroys the session and redirects to `/`.
  - anything else → `serve_static_file()`, passing the `If-Modified-Since` header for cache validation; a non-zero return code (`403`/`404`/`500`) is rendered via `send_error_response()`.
- `POST /login` → epoch3 only; other epochs re-render the "not available" `login_epoch<N>.html` without touching the database.
- `POST /dashboard/api/media/directory` → create media directory.
- `POST /dashboard/api/media/directory/rename` → rename (renames physical dir via `rename()`).
- `POST /dashboard/api/media/directory/delete` → delete (removes physical dir via `rmdir()`).
- `POST /dashboard/api/media/upload` → multipart file upload: saves to `html/content/posts/<username>/<dirname>/`, runs `scripts/image-optimizer.sh`, inserts into `media` collection.
- `POST /dashboard/api/media/move` → moves media items (files and DB) to another directory.
- `POST /dashboard/api/entries/<id>/content` → after saving blocks, for each `gallery` block calls `cms_upsert_media_gallery()` to sync the `media_galleries` collection and stores the gallery `_id` in the block's `extra_data`.
- `POST /dashboard/api/block-preview` → `entry_page_render_block()`: the editor's preview of a block, rendered exactly as epoch 3 would.
- `POST /dashboard/settings*`, `/dashboard/settings/fonts/*`, `/dashboard/api/theme-assets/{upload,delete}` → settings writes (see [routes.md](routes.md#settings-themes-fonts-preview)).
- Returns `204` for `OPTIONS`, `405` for all other methods.
- `send_error_response(ctx, status_code, status_line, epoch)`: renders `error_content()` +
  `buildPageWebSite()` + `build_epoch_response_status()` for any non-2xx/3xx response (`400`,
  `403`, `404`, `405`, `413`, `431`, `500`, `503`), falling back to a hardcoded minimal HTML response
  if template rendering itself fails. This is the single centralized path for all
  epoch-aware error pages.
- `send_or_error(ctx, response, method, epoch)`: writes a malloc'd response (truncated to
  headers for `HEAD`), or calls `send_error_response(ctx, 500, ...)` if `response` is `NULL`
  (e.g. a module returned `NULL`).

### `web_server/http_request_parser.c`
- Parses `METHOD URL PROTOCOL` from the first line - or just `METHOD URL` (a true HTTP/0.9
  request line, no version token; `req.protocol` is left empty), which a real Cello 1.x sends
  for at least some fetches. See `http_router.c`'s legacy-client tolerance above.
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

### `utils/template_utils.c`
- `render_template(tpl, ...)` (printf-style), `str_replace_first()`, `str_replace_all()` (every occurrence - used for `{{MARKERS}}`, never as a format string), `str_append()`, `capitalize_first()` (theme keys as "Dark"/"Light" on epochs 1/2). The former `build_title_tag()` helper was removed: titles are now the layout's `{{PAGE_TITLE}}` marker.
- `image_url_variant(url, suffix)` → new malloc'd URL with `suffix` inserted before the file extension. Example: `image_url_variant("/content/posts/user/dir/photo.jpg", "_small")` → `"/content/posts/user/dir/photo_small.jpg"`. Used by `home_blog`, `blog_list`, `entries_admin`, and `entry_page` to generate thumbnail and full-size URLs from the base URL stored in `header.image_url` / `content[].text`. Those fields always hold the **bare** path: callers append the variant they need, and must not assume the bare path resolves on its own (see [rendering.md](rendering.md)).
- `slugify(name)` → new malloc'd, lowercased URL slug: `[a-z0-9]` kept as-is, `A-Z` lowercased, spaces/`-`/`_` collapsed into a single `-`, every other byte dropped, trailing `-` trimmed. Example: `slugify("Retro Hardware")` → `"retro-hardware"`. Used to build and match `/blog/category/<slug>` URLs (`cms_entries.c`, `category_menu.c`, `http_router.c`). Slugs are **derived, not stored** - the category's `name` in the current content language is the source of truth, so a category renamed in the admin changes its public URL.

### `utils/qr_generator/`
- Ported from the legacy CMS (`../the-retro-center-old`) so `youtube-embed` blocks work before
  HTML5: retro epochs cannot embed a player, so they show a QR code the reader scans. The same
  generic functions are reused by the standalone `/gallery/<id>` page for epochs -1/0, which
  cannot show the photos it lists either - see [rendering.md](rendering.md).
- `generate_youtube_qr()` (GIF, epochs 1-2), `generate_youtube_qr_wbmp()` and
  `generate_qr_wbmp(text, fs_path)` (any text → WBMP via `wbmp_writer.c`, cached on disk; used
  for WML), `generate_qr_halfblock_text(text)` (any text → Unicode half-block `<pre>` art; epoch
  0 in a UTF-8 terminal) and `generate_qr_asciiblock_text(text)` (Latin-1 block glyphs for Cello,
  chosen when `request_needs_legacy_charset()` - see
  [rendering.md](rendering.md#character-encoding)). Long targets are shortened through
  `/qr/<code>` first - see [qr-and-short-links.md](qr-and-short-links.md).
  `extract_youtube_id()` accepts only `[A-Za-z0-9_-]`, since the id is interpolated into
  filesystem paths.
- Encodes the matrix with **libqrencode** (declared in `qrencode_minimal.h`; the library ships
  no pkg-config file, so `CMakeLists.txt` locates it with `find_library`). The GIF and WBMP
  writers are pure C in this module - the original shelled out to ImageMagick per render.
- Assets are cached under `html/content/qr/` (gitignored). Each writer builds a private temp
  file and `rename()`s it into place, and the LZW dictionary is allocated per call, because
  connection threads render pages concurrently and would otherwise interleave writes.

### `utils/json_utils.c`
- `json_escape_alloc(src)` → new malloc'd copy of `src` escaped for embedding inside a JSON string literal (escapes `"`, `\` and control characters; UTF-8 multi-byte sequences pass through unchanged). Used by the entry editor's AJAX endpoints to embed rendered HTML in a JSON response (`/dashboard/api/entries/<id>/blocks`).

### `utils/config_loader.c`
- Reads `key=value` lines from the config file (path configurable via `-c` CLI flag).
- Populates every key documented in [configuration.md](configuration.md): server, TLS and
  logging; `trusted_proxies`, `theme`, `lang`, `force_epoch`, `public_url`; MongoDB and session
  TTL; the six `ddos_*` keys and `connection_io_timeout_secs`; the five `wap_gateway_*` keys.

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
brew install openssl cmake pkg-config mongo-c-driver libsodium qrencode
brew install libmaxminddb          # optional: GeoIP country detection
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
cmake --build build
```

### Build on Linux

```bash
# Debian/Ubuntu
sudo apt install cmake gcc pkg-config libssl-dev libmongoc-dev libsodium-dev libqrencode-dev
sudo apt install libmaxminddb-dev   # optional: GeoIP country detection (HAVE_MAXMINDDB)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake prints whether libmaxminddb was found; without it the build succeeds and every visit's
country is recorded as `Unknown`. The usual entry point is `./boat_rudder_builder.sh`
([scripts.md](scripts.md)).

---

## Concurrency Model

| Thread | Started by | Job |
|---|---|---|
| main | - | Startup, then sleeps until a signal clears `running`; then shutdown |
| accept | `server_start()` | `select()` (1 s timeout) over the HTTP/HTTPS listeners, anti-DDoS checks, spawns connection threads |
| IP-table cleanup | `server_start()` | Every `ddos_cleanup_interval_secs`: lifts bans, frees stale entries |
| connection (one per request) | accept thread | Detached, 2 MiB stack: TLS handshake, `http_route()`, close |
| WAP gateway (optional) | `wap_gateway_start()` | `poll()` over the UDP sockets, one datagram at a time |

- A **mutex-protected counter** caps simultaneous active connections at `ddos_max_connections` (200).
- The **per-IP table** is shared by the accept and cleanup threads and guarded by `ip_table_mutex`.
- The WAP gateway's own rate table is touched only by the gateway thread (no lock).
- **Per-request state** (language, path, theme, user, charset, code-lines, WML target) lives in
  `__thread` variables: a connection thread serves exactly one request, so thread-local equals
  request-local. Anything that renders must run on the request's own thread.
- MongoDB: one `mongoc_client_t` per thread from the pool (`mongoc_client_t` isn't thread-safe).
- No thread pool: threads are created and destroyed per request.

**Known limitations (accepted for initial version):**

| Limitation | Impact | Future path |
|---|---|---|
| `select()` instead of `epoll`/`kqueue` | FD_SETSIZE cap; O(n) scan | `epoll_wait()` on Linux, `kqueue` on macOS |
| Thread-per-connection | ~400 MB RSS at 200 connections | Bounded thread pool with work queue |
| Synchronous analytics writes | Two MongoDB round-trips per counted page view | Batched/async writer |
| Single-threaded WAP gateway | One slow loopback fetch delays every phone | Worker per datagram |
| No HTTP/1.1 Keep-Alive | One TCP handshake per resource | Per-connection request loop |
| No HTTP Range requests | No resumable downloads | `Accept-Ranges` + `206 Partial Content` |
| No response compression | Full transfer for compressible assets | `Content-Encoding: gzip` with zlib |

---

## Security Considerations

The complete description, including known gaps, is [security.md](security.md). In short:

- **Anti-DDoS at `accept()`**: configurable global connection cap and per-IP rate limit with
  temporary bans, LRU table plus a cleanup thread; `429` for rejected plain-HTTP connections,
  silent close for HTTPS (no handshake spent on them).
- **Slow-loris**: per-call socket timeouts (`connection_io_timeout_secs`, 5 s).
- **WAP gateway**: per-source-IP rate limit against UDP amplification.
- `sanitize_path()` uses `realpath()` to prevent directory traversal attacks; theme keys are
  charset-checked before touching the filesystem; `return`/`back` redirects only accept
  site-relative paths.
- `X-Real-IP` / `X-Forwarded-For` are honored **only from trusted proxy IPs** configured in `trusted_proxies`; raw peer address is used for all other connections.
- `SIGPIPE` is ignored globally and suppressed at socket level (`SO_NOSIGPIPE` / `MSG_NOSIGNAL`).
- Body reads are capped at `MAX_BODY_SIZE` (10 MiB, `413`); header buffer at `RAW_REQUEST_SIZE` (32 KiB, `431`).
- TLS minimum version: TLS 1.2. Cipher suites are explicitly hardened (ECDHE+AESGCM, ChaCha20; no RC4, 3DES, export ciphers).
- Every response includes `X-Content-Type-Options: nosniff` and `X-Frame-Options: SAMEORIGIN`.
- Passwords are stored as `crypto_pwhash_str()` (Argon2id) hashes; `auth_login_user()` never
  reveals whether an email or a password was wrong (same `NULL` result for both, and for DB
  errors).
- Session tokens are 32 random bytes from libsodium's CSPRNG, hex-encoded, stored server-side
  in the `sessions` collection with an `expires_at`. The cookie is `HttpOnly; Path=/;
  SameSite=Lax`, plus `; Secure` when `ssl_enabled=1`.
- The login form, credential checks and every dashboard route are restricted to `EPOCH_MODERN`,
  enforced server-side; admin-only routes additionally check the role.
- Analytics stores no IP addresses.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
