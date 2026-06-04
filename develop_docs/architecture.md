# base-http-server - Architecture

## Overview

`base-http-server` is a minimal, self-contained static HTTP/HTTPS server written in **C17**. Its only external dependency is OpenSSL. It serves files from a configurable root directory, supports concurrent connections via POSIX threads, and optionally enables TLS.

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
│   └── utils/
│       ├── config_loader.c/h            # INI-style config file parser
│       ├── log.c/h                      # Leveled, thread-safe logging macros
│       └── http_utils.c/h               # MIME detection, URL encode/decode, path sanitizer,
│                                        # trusted proxy check
├── configs/
│   └── config.txt                       # Runtime configuration
├── www/                                 # Default static files root
├── ssl/                                 # TLS certificate and key (optional)
└── CMakeLists.txt                       # Build definition
```

---

## Layers

```
┌──────────────────────────────────────────────────────┐
│                      main.c                          │  ← Entry point: config, signals, lifecycle
├──────────────────────────────────────────────────────┤
│              web_server/server_listener               │  ← Socket creation, select() accept loop
├────────────────────────────┬─────────────────────────┤
│       connection.c          │     tls_context.c       │  ← I/O abstraction  │  TLS context
├────────────────────────────┴─────────────────────────┤
│              connection_thread.c                      │  ← Per-request pthread + TLS handshake
├──────────────────────────────────────────────────────┤
│              http_router.c                            │  ← HTTP parse, route dispatch
├────────────────────────────┬─────────────────────────┤
│    http_request_parser.c    │  utils/static_file_server│  ← Parser  │  File server
├────────────────────────────┴─────────────────────────┤
│       utils/  (log, config_loader, http_utils)        │  ← Cross-cutting utilities
└──────────────────────────────────────────────────────┘
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
- Routes `GET`/`HEAD` to `serve_static_file()`, passing the `If-Modified-Since` header for cache validation.
- Returns `204` for `OPTIONS`, `405` for all other methods.

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

### `web_server/utils/url_parser.c`
- Splits a URL string into a path component and query parameters (`?key=value&…`).
- Owns the `QueryParam` struct definition.

### `utils/config_loader.c`
- Reads `key=value` lines from the config file (path configurable via `-c` CLI flag).
- Populates: `http_port`, `https_port`, `ssl_enabled`, `ssl_cert`, `ssl_key`, `verbose_level`, `trusted_proxies`.

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

## Configuration (`configs/config.txt`)

```ini
verbose_level=3           # 0=none 1=error 2=warn 3=info 4=debug
http_port=8080
https_port=8443
ssl_enabled=0             # 1 to enable HTTPS
ssl_cert=./ssl/cert.pem
ssl_key=./ssl/key.pem
trusted_proxies=          # comma-separated IPs of trusted reverse proxies
                          # e.g. 127.0.0.1,10.0.0.1
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

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
