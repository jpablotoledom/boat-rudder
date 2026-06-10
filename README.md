# Boat Rudder

A self-contained HTTP/HTTPS server written in **C17** that doubles as a **retro-compatible
CMS**: requests to `/` are rendered on the fly into the simplest markup the requesting browser
can understand - from 1990s WAP phones to modern HTML5/CSS3 - while every other path is served
as a plain static file.

Only external dependency: **OpenSSL**. Built with **CMake**, concurrency via **POSIX threads**.

---

## Features

- Static file server: MIME detection, `Last-Modified`/`If-Modified-Since` caching,
  directory-traversal protection, 8 KiB streaming.
- Optional HTTPS (TLS 1.2+, hardened cipher suites).
- Per-IP rate limiting and a global connection cap.
- Trusted-proxy-aware `X-Real-IP` / `X-Forwarded-For` handling.
- **Retro-compatible CMS**: `/` is classified into one of 5 browser "epochs" (WAP/WML, plain
  text, HTML 3.2, HTML4+CSS1, HTML5+CSS3) and assembled from epoch-specific templates.

For a full tour of the architecture and the CMS, see
**[develop_docs/boat-rudder.md](develop_docs/boat-rudder.md)**.

---

## Quick Start

```bash
# Build (debug, with AddressSanitizer) and run locally
./bhs.sh compiledebug rundebug
```

By default the server listens on the port configured in `configs/settings.conf` and serves
`html/`. Open `http://localhost:<http_port>/` in a browser to see the CMS home page.

---

## `bhs.sh` - Build & Run

```
./bhs.sh <action1> [action2] ...
```

| Action | Description |
|---|---|
| `compiledebug` | Build with debug symbols + AddressSanitizer, assembled into `bin/` |
| `compileprod` | Build optimized + stripped for production, assembled into `bin/` |
| `rundebug` | Run `bin/base-http-server` locally (auto-selects GDB/LLDB if available) |
| `createcert` | Generate a self-signed TLS certificate for local development |
| `install` | Compile for production and install as a systemd service (Linux, requires sudo) |
| `uninstall` | Stop and remove the systemd service (Linux, requires sudo) |

Actions run left to right and can be combined:

```bash
./bhs.sh compiledebug rundebug
./bhs.sh createcert compiledebug rundebug
./bhs.sh compileprod install
```

See [develop_docs/scripts.md](develop_docs/scripts.md) for the full reference.

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

theme=dark                 # active theme under html/themes/<theme>/
lang=Eng                   # content language for the home page
public_url=                # public base URL (reserved for SEO/canonical links)

#force_epoch=3             # force a browser epoch for "/" (-1..3), omit to auto-detect
```

---

## Project Layout

```
boat-rudder/
├── src/
│   ├── main.c                 # Entry point
│   ├── web_server/            # Sockets, TLS, routing, static file serving
│   ├── html_builder/           # Orchestrator: assembles the home page per epoch
│   ├── modules/                # container, menu, slider, home_content
│   └── utils/                  # config, logging, epoch detection, templating
├── html/                       # Static content root + epoch templates (themes/<theme>/)
├── configs/settings.conf        # Runtime configuration
├── ssl/                         # TLS certificate and key (optional)
├── scripts/                     # Build/run/install scripts (called by bhs.sh)
└── develop_docs/                # Architecture, data flow, CMS docs and diagrams
```

---

## Documentation

- **[develop_docs/boat-rudder.md](develop_docs/boat-rudder.md)** - project overview: web
  server, retro-compatible CMS concept, epoch strategy, request lifecycle, with diagrams.
- [develop_docs/architecture.md](develop_docs/architecture.md) - full component breakdown.
- [develop_docs/data-flow.md](develop_docs/data-flow.md) - step-by-step request data flow.
- [develop_docs/retro-compatible-cms.md](develop_docs/retro-compatible-cms.md) - original CMS
  implementation plan.
- [develop_docs/scripts.md](develop_docs/scripts.md) - `bhs.sh` and build/deploy scripts.
- [develop_docs/style-guide.md](develop_docs/style-guide.md) - C coding style and security
  rules (Google C++ Style Guide + SEI CERT C, adapted for this project).
- [develop_docs/diagrams/](develop_docs/diagrams/) - PlantUML source for all diagrams.

---

## License

All Rights Reserved - see [LICENSE](LICENSE). The source is public for
viewing and evaluation only; reuse requires written permission.

---

## Contact

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
