# base-http-server - Data Flow

This document describes how data moves through the server from the moment a TCP connection arrives until the response is fully sent.

---

## 1. Server Startup

```
main()
  │
  ├─ load_config("./configs/config.txt")
  │     Reads: http_port, https_port, ssl_enabled, ssl_cert, ssl_key, verbose_level
  │
  ├─ server_start(root_dir, ssl_enabled, ssl_cert, ssl_key, http_port, https_port)
  │     ├─ socket(AF_INET, SOCK_STREAM) → server_fd_http
  │     ├─ bind(server_fd_http, port=http_port)
  │     ├─ listen(server_fd_http, backlog=128)
  │     │
  │     ├─ [if ssl_enabled]
  │     │     ├─ socket() → server_fd_https
  │     │     ├─ bind(server_fd_https, port=https_port)
  │     │     ├─ listen(server_fd_https, backlog=128)
  │     │     └─ ssl_create_context(cert, key) → ssl_ctx
  │     │
  │     └─ enter accept loop  ──────────────────────────────►  see §2
  │
  └─ sleep loop until SIGINT/SIGTERM → server_stop()
```

---

## 2. Accept Loop (`start_stop.c`)

The main thread runs a blocking `select()` over both listening sockets.

```
while (running):
  select([server_fd_http, server_fd_https]) ─── blocks until activity ───┐
                                                                           │
  ┌────────────────────────────────────────────────────────────────────────┘
  │
  ├─ HTTP socket ready?
  │     accept() → client_socket
  │     too_many_connections(ip)?  ──yes──► close(client_socket), continue
  │     try_register_connection()? ──no──►  close(client_socket), continue
  │     malloc(thread_args) { client_socket, root_dir, ssl=NULL }
  │     pthread_create(connection_thread, args)   ──────────────►  see §3
  │     pthread_detach(tid)
  │
  └─ HTTPS socket ready?
        accept() → client_socket
        too_many_connections(ip)?  ──yes──► close(client_socket), continue
        try_register_connection()? ──no──►  close(client_socket), continue
        SSL_new(ssl_ctx) → ssl
        SSL_set_fd(ssl, client_socket)
        malloc(thread_args) { client_socket, root_dir, ssl }
        pthread_create(connection_thread, args)   ──────────────►  see §3
        pthread_detach(tid)
```

**Rate limiting state machine (per IP entry):**
```
NEW IP → count=1, blocked=false
SAME IP within RATE_WINDOW:
  count++ → if count > RATE_LIMIT: blocked=true, record last_rejected
  if blocked && (now - last_rejected) > RATE_WINDOW*2: blocked=false, count=0
```

---

## 3. Connection Thread (`connection_thread.c`)

Each connection runs in its own pthread.

```
connection_thread(thread_args)
  │
  ├─ malloc(connection_ctx_t) { client_socket, ssl }
  ├─ setsockopt SO_RCVTIMEO = 5s, SO_SNDTIMEO = 5s
  │
  ├─ [if ssl != NULL]
  │     SSL_accept(ssl)  ← TLS handshake
  │     failure? → goto cleanup
  │
  ├─ select read_func:
  │     ssl != NULL  →  read_func = ssl_read
  │     ssl == NULL  →  read_func = plain_read
  │
  ├─ request_handler(read_func, conn_ctx, root_directory)  ────►  see §4
  │
  └─ cleanup:
        SSL_shutdown + SSL_free  (if SSL)
        close(client_socket)
        unregister_connection()   ← decrements active_connections
```

---

## 4. Request Handler (`request_handler.c`)

This is the core HTTP processing stage.

```
request_handler(read_func, ctx, root_directory)
  │
  ├─ malloc(raw_request, RAW_REQUEST_SIZE=600KB)
  │
  ├─ READ LOOP: call read_func() until "\r\n\r\n" found (headers complete)
  │     EOF / error? → goto conn_cleanup
  │
  ├─ parse_http_request(raw_request) → HttpRequest { method, url, protocol, headers[], body }
  │     failure? → send 400 Bad Request → goto conn_cleanup
  │
  ├─ Extract headers:
  │     User-Agent, Cookie, Content-Type, Content-Length
  │
  ├─ READ BODY (if Content-Length > 0, max 10 MB):
  │     copy already-read bytes after header end
  │     loop read_func() until body_bytes_read == content_length
  │
  ├─ Extract client IP:
  │     X-Real-IP → X-Forwarded-For → getpeername() fallback
  │
  ├─ Validate request line (sscanf METHOD URL PROTO):
  │     invalid? → send 400 Bad Request → goto conn_cleanup
  │
  ├─ split_url(url) → route + QueryParam[]
  ├─ url_decode(route) → decoded_url
  │
  ├─ ROUTE DISPATCH:
  │     GET  → handle_static_file_or_directory()  ────────────►  see §5
  │     OPTIONS → send 200 with CORS headers
  │     other → send 405 Method Not Allowed
  │
  └─ conn_cleanup:
        free(raw_request)
        free(req.body)
        connection_close(ctx)
```

---

## 5. Static File Handler (`utils/static_handler.c`)

```
handle_static_file_or_directory(ctx, root_directory, decoded_url)
  │
  ├─ build path:  safe_path = root_directory + decoded_url
  │
  ├─ stat(safe_path):
  │     ENOENT / error? → send 404 Not Found → return
  │
  ├─ S_ISDIR? → (directory listing, currently a no-op stub)
  │
  └─ Regular file:
        open(safe_path, O_RDONLY)
        error? → send 500 Internal Server Error → return
        │
        ├─ get_mime_type(safe_path)  ← extension lookup table
        │
        ├─ Send response header:
        │     "HTTP/1.1 200 OK\r\n"
        │     "Content-Type: <mime>\r\n"
        │     "Content-Length: <size>\r\n"
        │     "Connection: close\r\n\r\n"
        │
        ├─ STREAM LOOP:
        │     read(fd, buffer, 4096) → connection_write(ctx, buffer, bytes)
        │     repeat until EOF
        │
        └─ close(fd)
```

---

## 6. I/O Abstraction (`connection.c`)

All writes and reads go through a thin wrapper that handles both plain TCP and SSL transparently.

```
connection_write(ctx, buf, count)
  ctx->ssl != NULL ?
    SSL_write(ssl, buf, count)   ← handles WANT_READ/WANT_WRITE/SYSCALL errors
  :
    send(socket, buf, count, MSG_NOSIGNAL)

plain_read(ctx, buf, count)  →  read(socket, buf, count)
ssl_read(ctx, buf, count)    →  SSL_read(ssl, buf, count)

Return codes:
  > 0  → bytes transferred
  = 0  → connection closed by peer (EOF)
  = -2 → transient error, caller should retry
  = -1 → fatal error
```

---

## 7. Shutdown Flow

```
SIGINT / SIGTERM
  │
  └─ handle_shutdown():  running = 0

main loop exits
  │
  └─ server_stop():
        running = 0
        close(server_fd_http)
        close(server_fd_https)
        ssl_free_context(ssl_ctx)
```

Active threads finish naturally (they check nothing from main, they just run to completion with the 5 s socket timeout as backstop).

---

## Data Structures Summary

| Structure | Location | Purpose |
|---|---|---|
| `connection_ctx_t` | `connection.h` | Holds `client_socket` + `SSL*` for a single connection |
| `thread_args` | `connection_thread.h` | Passed to each pthread: socket, root_dir, ssl pointer |
| `HttpRequest` | `http_request_parser.h` | Parsed HTTP request: method, url, protocol, headers[], body |
| `HttpHeader` | `http_request_parser.h` | Single header key-value pair |
| `QueryParam` | `params.h` | Single URL query parameter key-value |
| `ip_entry_t` | `start_stop.c` (internal) | Per-IP rate limiting state |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
