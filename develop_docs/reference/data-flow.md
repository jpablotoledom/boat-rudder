# base-http-server - Data Flow

This document describes how data moves through the server from the moment a TCP connection arrives until the response is fully sent.

---

## 1. Server Startup

```
main()
  │
  ├─ load_config("./configs/settings.conf")
  │     Reads: http_port, https_port, ssl_enabled, ssl_cert, ssl_key, verbose_level,
  │            theme, lang, public_url, force_epoch (retro-compatible CMS, see §5a),
  │            mongodb_uri, mongodb_db, session_ttl_seconds (login/sessions, see §5b)
  │
  ├─ mongodb_manager_init(mongodb_uri, mongodb_db)
  │     sodium_init()  ← required once before any libsodium call
  │     mongoc_init() + mongoc_client_pool_new()
  │     failure → logged, server continues; /login and /dashboard return 503 (see §5b/§5c)
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
  │     │     └─ tls_create_context(cert, key) → ssl_ctx
  │     │
  │     └─ enter accept loop  ──────────────────────────────►  see §2
  │
  └─ sleep loop until SIGINT/SIGTERM → server_stop()
```

---

## 2. Accept Loop (`server_listener.c`)

The main thread runs a blocking `select()` over both listening sockets.

```
while (running):
  select([server_fd_http, server_fd_https]) ─── blocks until activity ───┐
                                                                         │
  ┌──────────────────────────────────────────────────────────────────────┘
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
  ├─ http_route(read_func, conn_ctx, root_directory)  ─────────►  see §4
  │
  └─ cleanup:
        SSL_shutdown + SSL_free  (if SSL)
        close(client_socket)
        unregister_connection()   ← decrements active_connections
```

---

## 4. HTTP Router (`http_router.c`)

This is the core HTTP processing stage.

```
http_route(read_func, ctx, root_directory)
  │
  ├─ malloc(raw_request, RAW_REQUEST_SIZE=32KB)
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
  ├─ url_parse(url, route, sizeof(route), params, &param_count) → route + QueryParam[]
  ├─ url_decode(route) → decoded_url
  │
  ├─ ROUTE DISPATCH:
  │     GET/HEAD, route == "/"          → dynamic home page  ───────────►  see §5a
  │     GET/HEAD, route == "/login"     → login page (epoch-aware)  ─────►  see §5b
  │     GET/HEAD, route == "/dashboard" → dashboard or 302 /login  ──────►  see §5b
  │     GET/HEAD, route == "/logout"    → destroy session, 302 /  ───────►  see §5b
  │     POST,     route == "/login"     → authenticate, 302 /dashboard ─►  see §5b
  │     GET/HEAD, other route           → serve_static_file()  ─────────►  see §5
  │     OPTIONS → send 204
  │     other → send 405 Method Not Allowed
  │
  │     Any non-2xx/3xx response (400/403/404/405/431/500/503) is rendered
  │     via send_error_response(ctx, status_code, status_line, epoch)  ──►  see §5c
  │
  └─ conn_cleanup:
        free(raw_request)
        free(req.body)
        connection_close(ctx)
```

---

## 5a. Dynamic Home Route (`/`) - Retro-Compatible CMS

```
GET/HEAD "/"  (http_router.c)
  │
  ├─ ua = header("User-Agent")
  ├─ epoch = (force_epoch in -1..3) ? force_epoch : detect_epoch(ua)
  │     -1 = WML, 0 = pre-standard, 1 = early, 2 = middle, 3 = modern
  │     force_epoch (config_loader, default unset) overrides detection when in range
  │
  ├─ body = buildHomeWebSite(epoch, lang)         ── html_builder/orchestrator.c
  │     ├─ container(epoch)
  │     │     generate_url_theme("container/container_epoch%d.html", epoch)
  │     │     read_file_to_string() → tpl (4x %s: menu, slider, home_content, home_blog; {{PAGE_TITLE}})
  │     │
  │     ├─ menu("/", epoch)
  │     │     reads menu_epoch%d.html, menu-item_epoch%d.html, menu-item-separator_epoch%d.html
  │     │     for each route in MENU_ROUTES: render_template(menu_item_tpl, link, label, sep)
  │     │     → str_append into items, then render_template(menu_tpl, items)
  │     │
  │     ├─ slider(epoch)
  │     │     generate_url_theme("slider/slider_epoch%d.html", epoch) → read_file_to_string()
  │     │
  │     ├─ home_content(epoch, lang)
  │     │     for each entry in UPDATES: render_template(item_tpl, title, date, text)
  │     │     → str_append into items, then render_template(content_tpl, items)
  │     │
  │     ├─ home_blog(epoch)
  │     │     reads home-blog_epoch%d.html, home-blog-item_epoch%d.html
  │     │     for each entry in BLOG_POSTS: render_template(item_tpl, image, link, title, summary, author, date)
  │     │     (epoch -1/0: render_template(item_tpl, title, date, summary))
  │     │     → str_append into items, then render_template(content_tpl, items)
  │     │
  │     ├─ NULL check: any of the 5 pieces missing → goto cleanup, free non-NULL pieces
  │     │
  │     └─ render_template(container_tpl, menu_html, slider_html, home_content_html, home_blog_html)
  │           free(container_tpl/menu_html/slider_html/home_content_html/home_blog_html)
  │
  ├─ body == NULL? → send 500 Internal Server Error
  │
  ├─ response = build_epoch_response(body, "", epoch)  ── utils/build_epoch_response.c
  │     Content-Type by epoch:
  │       -1 → text/vnd.wap.wml
  │        0,1 → text/html
  │        2,3 → text/html; charset=UTF-8
  │     + SECURITY_HEADERS (X-Content-Type-Options, X-Frame-Options)
  │     free(body)
  │
  ├─ HEAD? → truncate response at end of "\r\n\r\n" (headers only)
  │
  └─ connection_write(ctx, response, response_len); free(response)
```

`generate_url_theme()` resolves every template path as `./html/themes/<theme>/<subpath>`,
relative to the server's working directory, using the global `theme` from `config_loader`
(default `dark`). All static assets referenced by the templates (CSS, images, favicon) are
served from the same `html/` tree via the normal static file path (§5).

---

## 5b. Login, Dashboard and Logout Routes

All four routes share `epoch = resolve_epoch(req)` (`force_epoch` override, else
`detect_epoch(User-Agent)`) and the generic `page_epoch<N>.html` shell via
`buildPageWebSite(epoch, title, content)` (head + menu + `%s` content + footer).

```
GET/HEAD "/login"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  ├─ cookie = get_header_value(req, "Cookie")
  │
  ├─ mongodb_manager_is_ready() && validate_session_cookie(cookie, user_id) == 1 ?
  │     yes → response = build_redirect_response("/dashboard", "", epoch)
  │           "302 Found\r\nLocation: /dashboard"
  │           send_or_error(...)                       ── already logged in, skip the form
  │
  │     no  ↓
  ├─ content = login(epoch, NULL)              ── modules/login
  │     epoch == EPOCH_MODERN → login_epoch3.html, error %s = ""
  │     epoch != EPOCH_MODERN → login_epoch<N>.html, verbatim ("not available")
  ├─ body = buildPageWebSite(epoch, "Boat Rudder - Login", content)
  └─ response = build_epoch_response(body, "", epoch); send_or_error(...)


POST "/login"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  │
  ├─ epoch != EPOCH_MODERN?
  │     → content = login(epoch, NULL); buildPageWebSite(...); 200 OK ("not available")
  │       (no DB access at all)
  │
  ├─ !mongodb_manager_is_ready()?
  │     → send_error_response(ctx, 503, "503 Service Unavailable", epoch)
  │
  └─ else:
        ├─ parse_urlencoded_field(body, "user")     → email
        ├─ parse_urlencoded_field(body, "password") → password
        │
        ├─ user_id = auth_login_user(email, password)   ── db/auth.c
        │     find users.email == email
        │     crypto_pwhash_str_verify(hash, password)   (libsodium, Argon2id)
        │     match → malloc'd 24-hex ObjectId string ; else → NULL
        │     (unknown email / wrong password / DB error → all NULL, no enumeration)
        │
        ├─ user_id == NULL?
        │     → content = login(epoch, "Invalid email or password.")
        │       body = buildPageWebSite(epoch, "Boat Rudder - Login", content)
        │       200 OK (re-rendered form + error block)
        │
        └─ user_id != NULL:
              ├─ token = generate_session_token()        ── db/session_manager.c
              │     32 random bytes (libsodium CSPRNG) → 64-hex string
              ├─ create_session(user_id, token, session_ttl_seconds)
              │     insert sessions: {user_id, token, created_at, expires_at}
              ├─ build_session_cookie_header(token, ttl, ...)
              │     "Set-Cookie: session=<token>; HttpOnly; Path=/;
              │      Max-Age=<ttl>; SameSite=Lax[; Secure]"
              └─ response = build_redirect_response("/dashboard", cookie_header, epoch)
                    "302 Found\r\nLocation: /dashboard\r\nSet-Cookie: ..."


GET/HEAD "/dashboard"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  │
  ├─ !mongodb_manager_is_ready()?
  │     → send_error_response(ctx, 503, "503 Service Unavailable", epoch)
  │
  └─ else:
        ├─ cookie = header("Cookie")
        ├─ validate_session_cookie(cookie, user_id_out)  ── db/session_manager.c
        │     extract_session_token() + validate_session()
        │     1  → valid, non-expired session (user_id_out filled)
        │     0  → missing / invalid / expired
        │     -1 → DB error
        │
        ├─ == 1?
        │     → content = dashboard(epoch)            ── modules/dashboard
        │       body = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content)
        │       200 OK ("Welcome to dashboard")
        │
        └─ != 1 (0 or -1)?
              → response = build_redirect_response("/login", "", epoch)
                "302 Found\r\nLocation: /login"


GET/HEAD "/logout"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  ├─ cookie = header("Cookie")
  ├─ mongodb_manager_is_ready() && extract_session_token(cookie, token)?
  │     → destroy_session(token)                       ── delete from sessions
  ├─ build_session_clear_cookie_header(...)
  │     "Set-Cookie: session=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax[; Secure]"
  └─ response = build_redirect_response("/", clear_cookie, epoch)
        "302 Found\r\nLocation: /\r\nSet-Cookie: session=...; Max-Age=0"
```

---

## 5c. Centralized Epoch-Aware Error Pages

Every non-2xx/3xx response - `400`, `403`, `404`, `405`, `431`, `500`, `503` - including those
returned as a status code from `serve_static_file()` (§5), goes through one helper:

```
send_error_response(ctx, status_code, status_line, epoch)  (http_router.c)
  │
  ├─ content  = error_content(epoch, status_code, NULL)   ── modules/error
  │     loads error_epoch<N>.html, fills 2x %s (status_code, message)
  │     message == NULL → default message from a static table
  │     (400/403/404/405/431/500/503; unknown codes → "Error")
  │
  ├─ body     = buildPageWebSite(epoch, "Boat Rudder - Error <code>", content)
  ├─ response = build_epoch_response_status(body, "", epoch, status_line)
  │
  ├─ response == NULL? (template missing / alloc failure)
  │     → send_simple(ctx, status_line, "<html><body><h1><status_line></h1></body></html>")
  │
  └─ else: connection_write(ctx, response, strlen(response)); free(response)
```

`send_or_error(ctx, response, method, epoch)` is the complementary helper used by every other
route: writes a malloc'd `response` (truncated to headers for `HEAD`), or calls
`send_error_response(ctx, 500, "500 Internal Server Error", epoch)` if `response` is `NULL`
(e.g. `buildPageWebSite()`/`buildHomeWebSite()` returned `NULL`).

---

## 5. Static File Handler (`utils/static_file_server.c`)

`serve_static_file()` returns `int`: `0` if it already wrote a response (`200`/`304`, or a
streaming failure that closed the connection), otherwise an HTTP status code (`403`/`404`/`500`)
with **nothing written yet** - the caller (`http_router.c`) renders that status via the
epoch-aware `send_error_response()` (§5c).

```
serve_static_file(ctx, root_directory, decoded_url, if_modified_since)
  │
  ├─ build path:  safe_path = root_directory + decoded_url
  │
  ├─ stat(safe_path):
  │     ENOENT / error? → return 404  (no response written)
  │
  ├─ S_ISDIR? → append "/index.html" to safe_path, re-stat
  │     too long / re-stat fails? → return 403 / 404
  │
  ├─ Cache validation:
  │     if_modified_since set && file not modified since → send 304 Not Modified → return 0
  │
  └─ Regular file:
        open(safe_path, O_RDONLY)
        error? → return 500  (no response written)
        │
        ├─ get_mime_type(safe_path)  ← extension lookup table
        │
        ├─ Send response header:
        │     "HTTP/1.1 200 OK\r\n"
        │     "Content-Type: <mime>\r\n"
        │     "Content-Length: <size>\r\n"
        │     "Last-Modified: <date>\r\n\r\n"
        │
        ├─ STREAM LOOP:
        │     read(fd, buffer, 8192) → connection_write(ctx, buffer, bytes)
        │     repeat until EOF
        │
        ├─ close(fd)
        └─ return 0
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
  ├─ server_stop():
  │     running = 0
  │     close(server_fd_http)
  │     close(server_fd_https)
  │     tls_free_context(ssl_ctx)
  │
  └─ mongodb_manager_cleanup()
        mongoc_client_pool_destroy() + mongoc_cleanup()
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
| `QueryParam` | `url_parser.h` | Single URL query parameter key-value |
| `ip_entry_t` | `server_listener.c` (internal) | Per-IP rate limiting state |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
