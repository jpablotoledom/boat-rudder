# Boat Rudder

A self-contained HTTP/HTTPS server written in **C17** that doubles as a **retro-compatible
CMS**: every dynamic page is rendered on the fly into the simplest markup the requesting browser
can understand - from 1990s WAP phones to modern HTML5/CSS3 - while every other path is served
as a plain static file. A built-in WAP 1.x gateway even lets real vintage phones browse the site.

External dependencies: **OpenSSL** for the server itself; **libmongoc** and **libsodium** for
the database-backed CMS and the dashboard; **libqrencode** for QR codes on retro browsers;
**libmaxminddb** (optional) for analytics country detection; `libm`. `stb_image.h` is vendored.
Built with **CMake**, concurrency via **POSIX threads**. Full list:
[develop_docs/reference/third-party.md](develop_docs/reference/third-party.md).

Boat Rudder is the software - the `boat-rudder` binary and systemd service, the source tree, the
`boat-rudder-*` CSS namespace, and every document under `develop_docs/`. It descends from
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
- Configurable anti-DDoS: per-IP rate limiting with temporary bans, a global connection cap,
  `429` responses, slow-loris timeouts.
- Trusted-proxy-aware `X-Real-IP` / `X-Forwarded-For` handling.
- **Retro-compatible CMS**: dynamic routes are classified into one of 5 browser "epochs"
  (WAP/WML, plain text, HTML 3.2, HTML4+CSS1, HTML5+CSS3) and assembled from epoch-specific
  templates.
- **MongoDB-backed content**: entries (pages and blog posts, with drafts) built from 13 typed
  content blocks, categories, menu, per-visitor language selection, and a media library with
  automatic image variants.
- **Retro degradation**: server-side syntax highlighting, QR codes (with short links) where an
  old browser can't show a video, image or gallery, Latin-1 output for pre-Unicode browsers,
  WML paginated into single-packet pages.
- **WAP 1.x gateway** (optional): serves the site as compiled WBXML over UDP to real WAP phones
  (via the external `trc-wap-relay`).
- **Themes**: shared templates plus per-theme overrides and a visitor theme selector. A theme
  customizer in the dashboard - pick an epoch, edit its logo, banner, colors, home blog
  backgrounds and footer beside a live preview (WML translated for epoch −1) - plus a
  customization layer over each epoch 3 stylesheet in a lightweight code editor, and a font
  library. Uploads live in `html/content/themes/`, safe from updates; WML images are a PNG with
  its WBMP twin.
- **Analytics**: cookie-less per-day visit counts by epoch, browser, OS, country (optional
  GeoLite2) and route - no IP addresses stored.
- **Dashboard** (modern browsers only): login with Argon2id, session cookies, two roles, an AJAX
  entry editor with server-rendered previews and autosave, plus Categories / Languages / Menu /
  Users / Media / Site settings (themes, logo, CSS, fonts, epoch preview) / Analytics.

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
theme=dark                # fallback theme (the dashboard's active theme wins)
mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder    # one database per site; boat_rudder is only the default
public_url=               # absolute base for QR-code URLs
connection_io_timeout_secs=5
ddos_max_connections=200  # + ddos_rate_*, ddos_max_ips, ddos_cleanup_*, ...
wap_gateway_enabled=0     # + wap_gateway_ips / _rover_port / _wsp_port / _rate_limit
```

**Every key, with its type, default and behavior, is documented in
[develop_docs/reference/configuration.md](develop_docs/reference/configuration.md)** - the single
reference, so nothing here can drift out of date.

If MongoDB is unreachable the server still starts and serves static files; `/login` and the
dashboard answer `503`, entries and galleries `404`, and listings show their empty state.

> The `configs/settings.conf` in the repository carries a development host's values (port 80,
> TLS on, a LAN `public_url`, the WAP gateway enabled). Review it before running elsewhere;
> `install` never overwrites a host's existing copy.

---

## Project Layout

```
boat-rudder/
├── src/
│   ├── main.c                 # Entry point
│   ├── web_server/            # Sockets, TLS, anti-DDoS, routing, static file serving
│   ├── wap_gateway/           # UDP WAP 1.x gateway + WML → WBXML compiler
│   ├── html_builder/          # Orchestrator and page layout per epoch
│   ├── modules/               # One renderer per visual component (home, blog, entry,
│   │                          # editor, dashboard maintainers, settings, analytics, ...)
│   ├── db/                    # MongoDB layer: auth, sessions, entries, media, themes,
│   │                          # fonts, short links, CMS CRUD
│   ├── utils/                 # config, logging, epoch detection, per-request state,
│   │                          # templating, code highlighting, QR, image conversion
│   └── third_party/           # Vendored code (stb_image.h)
├── html/                      # Static content root (CLI root directory)
│   ├── templates/             # Shared templates
│   ├── themes/<theme>/        # Per-theme templates, assets, public + admin epoch 3 CSS
│   ├── assets/                # Site-wide assets (fonts, dashboard scripts)
│   └── content/               # Uploaded media, theme uploads, QR cache
├── configs/settings.conf      # Runtime configuration
├── data/                      # Optional GeoLite2-Country.mmdb
├── ssl/                       # TLS certificate and key (optional)
├── scripts/                   # Build/run/install scripts, systemd unit, logrotate
├── CHANGELOG.md
└── develop_docs/              # Project overview, reference docs, plans and diagrams
    ├── reference/             # Reference documentation (see below)
    ├── plans/                 # Per-feature implementation plans and ADRs
    └── diagrams/              # PlantUML sources
```

---

## Documentation

- **[develop_docs/boat-rudder.md](develop_docs/boat-rudder.md)** - project overview: web
  server, retro-compatible CMS concept, epoch strategy, request lifecycle, with diagrams.
- [CHANGELOG.md](CHANGELOG.md) - what changed, when.

Reference (`develop_docs/reference/`):

| Document | Covers |
|---|---|
| [architecture.md](develop_docs/reference/architecture.md) | Server foundation (sockets, threads, TLS, router, static files), source map, document map |
| [data-flow.md](develop_docs/reference/data-flow.md) | Step-by-step request data flow, startup and shutdown |
| [routes.md](develop_docs/reference/routes.md) | Every route: method, guard, epochs, handler |
| [data-model.md](develop_docs/reference/data-model.md) | Every MongoDB collection, relationships, indexes |
| [rendering.md](develop_docs/reference/rendering.md) | Epochs, templates, the response layer, every public page |
| [templates-catalog.md](develop_docs/reference/templates-catalog.md) | Every template, its arguments and markers |
| [themes.md](develop_docs/reference/themes.md) / [fonts.md](develop_docs/reference/fonts.md) | Themes, site settings, font library |
| [dashboard.md](develop_docs/reference/dashboard.md) | Login, sessions, roles, maintainers |
| [entry-editor.md](develop_docs/reference/entry-editor.md) | The AJAX entry editor |
| [media-admin.md](develop_docs/reference/media-admin.md) | The media library and gallery pages |
| [analytics.md](develop_docs/reference/analytics.md) | Visit tracking, GeoIP, the report |
| [wap-gateway.md](develop_docs/reference/wap-gateway.md) | The UDP WAP gateway |
| [qr-and-short-links.md](develop_docs/reference/qr-and-short-links.md) | QR codes and `/qr/<code>` |
| [code-highlighting.md](develop_docs/reference/code-highlighting.md) | Server-side syntax highlighting |
| [configuration.md](develop_docs/reference/configuration.md) | Every `configs/settings.conf` key |
| [security.md](develop_docs/reference/security.md) | Every defense and the known gaps |
| [scripts.md](develop_docs/reference/scripts.md) | Build/deploy scripts |
| [third-party.md](develop_docs/reference/third-party.md) | Vendored, ported and linked code and data |
| [style-guide.md](develop_docs/reference/style-guide.md) | C coding style and security rules (Google C++ Style Guide + SEI CERT C), and BEM CSS class names |

- [develop_docs/plans/](develop_docs/plans/) - per-feature implementation plans (CMS entry
  model, home blog list, login, site settings/personalization, theme system) and a retroactive
  ADR for the WAP gateway and analytics.
- [develop_docs/diagrams/](develop_docs/diagrams/) - PlantUML source for all diagrams.

---

## License

All Rights Reserved - see [LICENSE](LICENSE). The source is public for
viewing and evaluation only; reuse requires written permission.

---

## Contact

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
