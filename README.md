# Boat Rudder

A self-contained HTTP/HTTPS server written in **C17** that doubles as a **retro-compatible
CMS**: requests to `/` are rendered on the fly into the simplest markup the requesting browser
can understand - from 1990s WAP phones to modern HTML5/CSS3 - while every other path is served
as a plain static file.

External dependencies: **OpenSSL** for the server itself, plus **libmongoc** and **libsodium**
for the database-backed CMS and the dashboard. Built with **CMake**, concurrency via
**POSIX threads**.

Boat Rudder is the software - the `boat-rudder` binary and systemd service, the source tree, the
`boat-rudder__*` CSS namespace, and every document under `develop_docs/`. It descends from
`base-http-server`, a minimal standalone static file server, which survives only as the shape of
the web-server half. A **site** built with Boat Rudder is a separate thing: its own MongoDB
database, theme and content. Nothing site-specific belongs in the source tree - the site name,
banner and page titles a visitor reads are content, and the templates ship "Boat Rudder" only as
the default until a site overrides it.

---

## Features

- Static file server: MIME detection, `Last-Modified`/`If-Modified-Since` caching,
  directory-traversal protection, 8 KiB streaming.
- Optional HTTPS (TLS 1.2+, hardened cipher suites).
- Per-IP rate limiting and a global connection cap.
- Trusted-proxy-aware `X-Real-IP` / `X-Forwarded-For` handling.
- **Retro-compatible CMS**: dynamic routes are classified into one of 5 browser "epochs"
  (WAP/WML, plain text, HTML 3.2, HTML4+CSS1, HTML5+CSS3) and assembled from epoch-specific
  templates.
- **MongoDB-backed content**: entries (pages and blog posts) with 14 typed content blocks,
  categories, menu, multi-language text, and a media library with automatic image variants.
- **Dashboard** (modern browsers only): login with Argon2id, session cookies, two roles, an AJAX
  entry editor with live preview and autosave, plus Categories / Languages / Menu / Users /
  Media maintainers.

For a full tour of the architecture and the CMS, see
**[develop_docs/boat-rudder.md](develop_docs/boat-rudder.md)**.

---

## Quick Start

```bash
# Build (debug, with AddressSanitizer) and run locally
./boat_rudder_builder.sh compiledebug rundebug
```

By default the server listens on the port configured in `configs/settings.conf` and serves
`html/`. Open `http://localhost:<http_port>/` in a browser to see the CMS home page.

---

## `boat_rudder_builder.sh` - Build & Run

```
./boat_rudder_builder.sh <action1> [action2] ...
```

| Action | Description |
|---|---|
| `compiledebug` | Build with debug symbols + AddressSanitizer into `bin/` (incremental: `build/` is reused) |
| `compileprod` | Build optimized + stripped for production, assembled into `bin/` |
| `clean` | Remove `build/` and `bin/` (forces a full rebuild next time) |
| `rundebug` | Run `bin/boat-rudder` locally (auto-selects GDB/LLDB if available) |
| `createcert` | Generate a self-signed TLS certificate for local development |
| `install` | Compile for production and install as a systemd service (Linux, requires sudo) |
| `uninstall` | Stop and remove the systemd service (Linux, requires sudo) |

Actions run left to right and can be combined:

```bash
./boat_rudder_builder.sh compiledebug rundebug
./boat_rudder_builder.sh createcert compiledebug rundebug
./boat_rudder_builder.sh compileprod install
```

See [develop_docs/reference/scripts.md](develop_docs/reference/scripts.md) for the full reference.

---

## Configuration (`configs/settings.conf`)

An INI-style `key=value` file, read at startup (override the path with `-c <file>`). The keys
you are most likely to touch first:

```ini
http_port=8080
https_port=8443
ssl_enabled=0             # 1 to enable HTTPS
theme=dark                # active theme under html/themes/<theme>/
mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder    # one database per site; boat_rudder is only the default
```

**Every key, with its type, default and behavior, is documented in
[develop_docs/reference/configuration.md](develop_docs/reference/configuration.md)** - the single
reference, so nothing here can drift out of date.

If MongoDB is unreachable the server still starts and serves static files, but the dashboard and
every database-backed page degrade to a `503`.

---

## Project Layout

```
boat-rudder/
├── src/
│   ├── main.c                 # Entry point
│   ├── web_server/            # Sockets, TLS, routing, static file serving
│   ├── html_builder/           # Orchestrator: assembles each page shell per epoch
│   ├── modules/                # One renderer per visual component (home, blog, entry,
│   │                            # editor, dashboard maintainers, error pages, ...)
│   ├── db/                      # MongoDB layer: auth, sessions, entries, media, CMS CRUD
│   └── utils/                  # config, logging, epoch detection, templating
├── html/                       # Static content root + epoch templates (themes/<theme>/)
├── configs/settings.conf        # Runtime configuration
├── ssl/                         # TLS certificate and key (optional)
├── scripts/                     # Build/run/install scripts (called by boat_rudder_builder.sh)
└── develop_docs/                # Project overview, reference docs, plans and diagrams
    ├── reference/                # Architecture, data flow, scripts, style guide
    ├── plans/                    # Per-feature implementation plans
    └── diagrams/                 # PlantUML sources
```

---

## Documentation

- **[develop_docs/boat-rudder.md](develop_docs/boat-rudder.md)** - project overview: web
  server, retro-compatible CMS concept, epoch strategy, request lifecycle, with diagrams.
- [develop_docs/reference/architecture.md](develop_docs/reference/architecture.md) - the server
  foundation (sockets, TLS, router, static files), plus a map of every other document.
- [develop_docs/reference/rendering.md](develop_docs/reference/rendering.md) - epochs, the
  per-epoch template convention and every public page.
- [develop_docs/reference/dashboard.md](develop_docs/reference/dashboard.md) - login, sessions,
  roles and the Categories / Languages / Menu / Users maintainers.
- [develop_docs/reference/configuration.md](develop_docs/reference/configuration.md) - every
  `configs/settings.conf` key.
- [develop_docs/reference/data-flow.md](develop_docs/reference/data-flow.md) - step-by-step
  request data flow.
- [develop_docs/reference/entry-editor.md](develop_docs/reference/entry-editor.md) - the AJAX
  entry editor at `/dashboard/entries/<id>/edit`.
- [develop_docs/reference/media-admin.md](develop_docs/reference/media-admin.md) - the media
  library at `/dashboard/media`.
- [develop_docs/reference/scripts.md](develop_docs/reference/scripts.md) - `boat_rudder_builder.sh` and
  build/deploy scripts.
- [develop_docs/reference/style-guide.md](develop_docs/reference/style-guide.md) - C coding
  style and security rules (Google C++ Style Guide + SEI CERT C, adapted for this project).
- [develop_docs/plans/](develop_docs/plans/) - per-feature implementation plans (CMS entry
  model, home blog list, login).
- [develop_docs/diagrams/](develop_docs/diagrams/) - PlantUML source for all diagrams.

---

## License

All Rights Reserved - see [LICENSE](LICENSE). The source is public for
viewing and evaluation only; reuse requires written permission.

---

## Contact

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
