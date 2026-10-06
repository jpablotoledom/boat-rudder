# Boat Rudder - Data Flow

This document describes how data moves through the server from the moment a TCP connection arrives until the response is fully sent.

Diagrams: [startup-flow.puml](../diagrams/startup-flow.puml),
[sequence-http.puml](../diagrams/sequence-http.puml),
[sequence-https.puml](../diagrams/sequence-https.puml),
[sequence-home-route.puml](../diagrams/sequence-home-route.puml),
[sequence-wap-gateway.puml](../diagrams/sequence-wap-gateway.puml).

---

## 1. Server Startup

```
main()
  │
  ├─ load_config("./configs/settings.conf")          (or -c <path>)
  │     Reads every key in configuration.md: ports/TLS/logging, trusted_proxies,
  │     theme, lang, public_url, force_epoch, mongodb_*, session_ttl_seconds,
  │     ddos_* + connection_io_timeout_secs, wap_gateway_*
  │
  ├─ mongodb_manager_init(mongodb_uri, mongodb_db)
  │     sodium_init()  ← required once before any libsodium call
  │     mongoc_init() + mongoc_client_pool_new()
  │     failure → logged, server continues; /login and /dashboard return 503 (see §5b/§5c)
  │
  ├─ cms_languages_ensure_seeded()   ← only if mongodb_manager_init() succeeded
  │     inserts {code:"en", name:"English", is_default:true} iff `languages` is empty
  │
  ├─ geoip_init("./data/GeoLite2-Country.mmdb")      ── modules/analytics/geoip.c
  │     HAVE_MAXMINDDB: MMDB_open(MMAP); missing/bad file → warning, countries "Unknown"
  │     without libmaxminddb: no-op stub, logs that country detection is disabled
  │
  ├─ signal(SIGINT/SIGTERM → handle_shutdown, SIGCHLD → reap)
  │
  ├─ server_start(root_dir, ssl_enabled, ssl_cert, ssl_key, http_port, https_port)
  │     ├─ ip_table = calloc(ddos_max_ips, sizeof(ip_entry_t))
  │     ├─ pthread_create(ip_table_cleanup_thread)   ──────────────────►  see §2
  │     ├─ socket/bind/listen(http_port, backlog=128) → server_fd_http
  │     ├─ [if ssl_enabled]
  │     │     socket/bind/listen(https_port) → server_fd_https
  │     │     tls_create_context(cert, key) → ssl_ctx
  │     ├─ pthread_create(accept_loop_thread)        ──────────────────►  see §2
  │     └─ return 0                                  (listeners are up)
  │
  ├─ wap_gateway_start()                             ── only if wap_gateway_enabled
  │     must run AFTER server_start(): the gateway fetches every page from
  │     127.0.0.1:http_port. Opens UDP sockets per wap_gateway_ips × {rover, wsp}
  │     port, starts the gateway thread. Failure → warning, server continues.
  │     See wap-gateway.md.
  │
  └─ while (running) sleep(1)   until SIGINT/SIGTERM  ─────────────────►  see §7
```

---

## 2. Accept Loop and Cleanup Thread (`server_listener.c`)

The accept loop runs on **its own thread** (`accept_loop_thread()`), so `server_start()` can
return and `main()` can react to signals.

```
while (running):
  select([server_fd_http, server_fd_https], timeout = 1 s)
  timeout? → re-check running, continue        ← bounds shutdown to ~1 s
  │
  ├─ HTTP socket ready?
  │     accept() → client_socket
  │     too_many_connections(ip)?   ──yes──► write 429, close, continue
  │     try_register_connection()?  ──no───► write 429, close, continue
  │     malloc(thread_args) { client_socket, root_dir, ssl=NULL }
  │     pthread_create(connection_thread, 2 MiB stack)   ─────────►  see §3
  │     pthread_detach(tid)
  │
  └─ HTTPS socket ready?
        accept() → client_socket
        too_many_connections(ip)?   ──yes──► close (no TLS handshake, no 429), continue
        try_register_connection()?  ──no───► close, continue
        SSL_new(ssl_ctx) → ssl; SSL_set_fd(ssl, client_socket)
        malloc(thread_args) { client_socket, root_dir, ssl }
        pthread_create(connection_thread)   ──────────────────────►  see §3
        pthread_detach(tid)
```

The `429` is best-effort and only for plain HTTP: completing a TLS handshake just to say "too
many requests" would spend exactly the CPU the rejection is meant to save.

**Rate limiting state machine (per IP entry, under `ip_table_mutex`):**
```
NEW IP → empty slot: count=1, blocked=0
         no empty slot: evict the entry with the oldest last_conn (LRU)
KNOWN IP:
  blocked && (now - last_rejected) <= 2 × ddos_rate_window_secs → reject
  blocked && expired                                            → blocked=0, count=0
  (now - last_conn) <  ddos_rate_window_secs → count++, last_conn=now
                                               count > ddos_rate_limit → blocked=1,
                                               last_rejected=now, reject
  (now - last_conn) >= ddos_rate_window_secs → count=1, last_conn=now
```

**Cleanup thread** (`ip_table_cleanup_thread()`), every `ddos_cleanup_interval_secs`
(`pthread_cond_timedwait`, so `server_stop()` can wake it at once):
```
lock ip_table_mutex
for each entry:
  blocked && ban expired                       → blocked = 0
  !blocked && (now - last_conn) > ddos_ip_stale_secs → zero the entry (slot freed)
unlock; log how many were freed
```

---

## 3. Connection Thread (`connection_thread.c`)

Each connection runs in its own pthread.

```
connection_thread(thread_args)
  │
  ├─ malloc(connection_ctx_t) { client_socket, ssl }
  ├─ setsockopt SO_RCVTIMEO = SO_SNDTIMEO = connection_io_timeout_secs (default 5 s)
  │     per read/write call - a stalled client is dropped (slow-loris),
  │     a slow-but-steady upload is not
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
        unregister_connection()   ← decrements active_connections, signals server_stop()
```

---

## 4. HTTP Router

This is the core HTTP processing stage (`http_router.c`). The complete route table is in
[routes.md](routes.md).

```
http_route(read_func, ctx, root_directory)
  │
  ├─ malloc(raw_request, RAW_REQUEST_SIZE=32KB)
  │
  ├─ READ LOOP until the header block is complete:
  │     "\r\n\r\n" or "\n\n"                      → done
  │     request line without version (HTTP/0.9)   → done immediately
  │     versioned line, no terminator, plain HTTP → wait ≤ 400 ms for more, else done
  │     buffer full → 431;  EOF / error → cleanup
  │
  ├─ READ BODY (if Content-Length > 0):
  │     Content-Length > MAX_BODY_SIZE (10 MiB) → 413
  │     realloc to header + body size, loop read_func() until complete
  │
  ├─ parse_http_request(raw_request) → HttpRequest { method, url, protocol, headers[], body }
  │     failure, or protocol present but not "HTTP/…" → 400
  │
  ├─ client_ip:
  │     peer = getpeername()
  │     is_trusted_proxy(peer) ? X-Real-IP → first X-Forwarded-For → peer : peer
  │
  ├─ url_parse(url) → route + QueryParam[];  url_decode(route) → decoded_url
  │
  ├─ PER-REQUEST STATE (thread-local, read by any module below):
  │     request_lang_set(Cookie, ?lang=)        ?lang= → lang cookie → languages default
  │     request_path_set(decoded_url)
  │     request_user_set(Cookie)                session → users.name (navbar)
  │     request_theme_set(Cookie, ?theme=)      ?theme= → theme cookie →
  │                                             site_settings.active_theme → config `theme`
  │     request_charset_set(User-Agent)         real browser needs Latin-1? (Cello)
  │     request_code_lines_set(?code_lines=)
  │     request_wml_set(raw url, ?wml_pages=)
  │     content_lang = request_lang()
  │
  ├─ analytics_track_visit(method, decoded_url, User-Agent, client_ip, resolve_epoch())
  │     GET only; skips bots, static extensions, /dashboard*  → see analytics.md
  │     two $inc upserts (page_visits_daily, entry_visits_daily) on this thread
  │
  ├─ ROUTE DISPATCH (first match wins; full table in routes.md):
  │     GET/HEAD "/"                       → home page  ─────────────────►  see §5a
  │     GET/HEAD "/blog", "/blog/category/<slug>", "/blog/categories", "/blog/<link>"
  │     GET/HEAD "/page/<link>"            → CMS entry (drafts only with a session)
  │     GET/HEAD "/gallery/<id>"           → gallery page (QR on epochs -1/0)
  │     GET/HEAD "/menu", "/language", "/language/set", "/theme", "/theme/set"
  │     GET/HEAD "/qr/<code>", "/youtube-qr/<id>", "/image-qr/<code>"
  │     GET/HEAD "/themes/<key>/styles_epoch3.css" → DB-overridable theme CSS
  │     GET/HEAD/POST "/login", POST "/logout", GET "/dashboard" ──────►  see §5b
  │     GET/POST "/dashboard/..."          → admin area (E3 + session/role guards)
  │     GET/HEAD other                     → serve_static_file()  ───────►  see §5
  │     OPTIONS → 204;  other method → 405
  │
  │     Every dynamic route resolves its epoch via resolve_epoch(req):
  │       force_epoch (if -1..3) → ?preview_epoch=N (if -1..3) → detect_epoch(User-Agent)
  │     and wraps its content with one of the html_builder/orchestrator.c page builders.
  │
  │     Any non-2xx/3xx response (400/403/404/405/413/431/500/503) is rendered
  │     via send_error_response(ctx, status_code, status_line, epoch)  ──►  see §5c
  │
  └─ cleanup:
        free(raw_request); free(req.body); connection_close(ctx)
```

---

## 5a. Dynamic Home Route (`/`) - Retro-Compatible CMS

```
GET/HEAD "/"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  │     force_epoch (if -1..3) → ?preview_epoch=N → detect_epoch(User-Agent)
  │     -1 = WML, 0 = pre-standard, 1 = early, 2 = middle, 3 = modern
  │
  ├─ body = buildHomeWebSite(epoch, lang)         ── html_builder/orchestrator.c
  │     ├─ generate_url_theme("page/page-home_epoch%d.html", epoch)
  │     │     read_file_to_string() → tpl (4x %s: menu, mainbanner, home_content, home_blog; {{FOOTER}})
  │     │
  │     ├─ menu("/", epoch)
  │     │     cms_get_menu_items(lang, &items, &count)  ── db.menu.find({enabled:true})
  │     │       .sort({order:1}); falls back to a single built-in {"/", "Home"} item
  │     │       if the query returns nothing (DB down, empty collection, error)
  │     │     reads menu_epoch%d.html, menu-item_epoch%d.html,
  │     │       menu-item-selected_epoch%d.html, menu-item-separator_epoch%d.html
  │     │     per item: render_template(menu_item_tpl, link, name, sep)
  │     │       (the item whose link == current_url uses the -selected template)
  │     │     → str_append into items, then render_template(menu_tpl, items)
  │     │
  │     ├─ mainbanner(epoch)
  │     │     cms_get_theme_banner(request_theme(), epoch)  ── themes.banner_html.<epoch>
  │     │       if empty → the theme's mainbanner/mainbanner_epoch%d.html
  │     │
  │     ├─ home_content(epoch, lang)
  │     │     the "/" CMS entry's content blocks if one exists (entry_page_render_content()),
  │     │     otherwise the built-in UPDATES[] array: render_template(item_tpl, title, date, text)
  │     │     → str_append into items, then render_template(content_tpl, items)
  │     │
  │     ├─ home_blog(epoch, lang)
  │     │     cms_get_blog_entries(lang, HOME_BLOG_LIMIT, &items, &count) ── db.entries.find(
  │     │       {type:"blog", enabled:true}).sort({"header.date":-1}).limit(HOME_BLOG_LIMIT)
  │     │     reads home-blog_epoch%d.html, home-blog-item_epoch%d.html
  │     │     per item: render_template(item_tpl, image, link, title, summary, author,
  │     │                                categories, date)
  │     │     (epoch -1/0: title, date, summary, categories)
  │     │     count == 0 → home-blog/empty_epoch%d.html; never fails the page
  │     │     → str_append into items, then render_template(content_tpl, items)
  │     │
  │     ├─ NULL check: any of the 5 pieces missing → goto cleanup, free non-NULL pieces
  │     │
  │     └─ render_template(container_tpl, menu_html, mainbanner_html, home_content_html, home_blog_html)
  │           free(container_tpl/menu_html/mainbanner_html/home_content_html/home_blog_html)
  │
  ├─ body == NULL? → send 500 Internal Server Error
  │
  ├─ (inside the builder) page_layout_wrap(): {{FOOTER}}, {{FOOTER_LOGO}}, {{SITE_NAME}},
  │     layout_epoch%d.html, {{PAGE_TITLE}}, {{COLOR_*}} (epochs 1/2), {{THEME_COLORS}} (epoch 3)
  │
  ├─ response = build_epoch_response(body, "", epoch)  ── utils/build_epoch_response.c
  │     retrofit_body_for_epoch():
  │       epochs -1/0/1: add ?lang=<code> to internal links
  │       epoch -1 (unless ?wml_pages=all): wml_paginate() to 860-byte pages,
  │         page = ?__page=N, Prev/Next links
  │       epochs 1/2: add ?theme=<key> to internal links
  │       epoch -1: close <br>, strip HTML lists
  │       epochs -1/1, and 0 when request_needs_legacy_charset(): UTF-8 → Latin-1
  │     Content-Type by epoch:
  │       -1 → text/vnd.wap.wml
  │        0 → text/html; charset=UTF-8   (bare text/html for Cello)
  │        1 → text/html                  (no charset: Mosaic matches it literally)
  │      2,3 → text/html; charset=UTF-8
  │     + SECURITY_HEADERS (X-Content-Type-Options, X-Frame-Options)
  │     free(body)
  │
  ├─ HEAD? → truncate response at end of "\r\n\r\n" (headers only)
  │
  └─ connection_write(ctx, response, response_len); free(response)
```

`generate_url_theme()` resolves every template path as `./html/themes/<request_theme()>/<subpath>`
when the theme has that file, otherwise `./html/templates/<subpath>` - relative to the server's
working directory (see [themes.md](themes.md#3-template-resolution)). All static assets
referenced by the templates (images, fonts, favicon) are served from the same `html/` tree via
the normal static file path (§5); the epoch 3 stylesheet is the exception, served by its own
route so it can come from the database.

---

## 5b. Login, Dashboard and Logout Routes

All four routes share `epoch = resolve_epoch(req)` (`force_epoch` override, else
`?preview_epoch=`, else `detect_epoch(User-Agent)`) and the generic `page_epoch<N>.html` shell via
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
  ├─ body = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content)
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
        │       body = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content)
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
        │     → role = cms_get_user_role(user_id)     ── "admin" | "author"
        │       content = dashboard(epoch, content_lang, user_id, role)  ── modules/dashboard
        │         epoch 3: nav links (admin only) + entries table
        │           admin  → entries_admin_rows(epoch, lang, NULL, NULL)     (every entry)
        │           author → entries_admin_rows(epoch, lang, "blog", user_id) (own posts only)
        │         other epochs: static "Welcome to dashboard" fragment
        │       body = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content)
        │       200 OK
        │
        └─ != 1 (0 or -1)?
              → response = build_redirect_response("/login", "", epoch)
                "302 Found\r\nLocation: /login"


GET/HEAD "/logout"  → 405 (logout is POST only)

POST "/logout"  (http_router.c)
  │
  ├─ epoch = resolve_epoch(req)
  ├─ cookie = header("Cookie")
  ├─ has_token = extract_session_token(cookie, token)
  ├─ has_token && !verify_csrf_token(cookie, csrf_token field | X-CSRF-Token)?
  │     → 403 "Invalid or missing CSRF token"          ── stop
  ├─ has_token && mongodb_manager_is_ready()?
  │     → destroy_session(token)                       ── delete from sessions
  ├─ build_session_clear_cookie_header(...)
  │     "Set-Cookie: session=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax[; Secure]"
  └─ response = build_redirect_response("/", clear_cookie, epoch)
        "302 Found\r\nLocation: /\r\nSet-Cookie: session=...; Max-Age=0"
```

---

## 5c. Centralized Epoch-Aware Error Pages

Every non-2xx/3xx response - `400`, `403`, `404`, `405`, `413`, `431`, `500`, `503` - including those
returned as a status code from `serve_static_file()` (§5), goes through one helper:

```
send_error_response(ctx, status_code, status_line, epoch)  (http_router.c)
  │
  ├─ content  = error_content(epoch, status_code, NULL)   ── modules/error
  │     loads error_epoch<N>.html, fills 2x %s (status_code, message)
  │     message == NULL → default message from a static table
  │     (400/403/404/405/431/500/503; unknown codes → "Error")
  │
  ├─ body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Error <code>", content)
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

## 7. Shutdown

```
SIGINT / SIGTERM
  │
  └─ handle_shutdown():  running = 0           (main's _Atomic flag)

main loop exits (≤ 1 s)
  │
  ├─ wap_gateway_stop()
  │     gateway_running = 0; join gateway thread (≤ 500 ms poll timeout); close UDP sockets
  │     first, so no new loopback fetch starts against a closing listener
  │
  ├─ server_stop()
  │     running = 0; close(server_fd_http); close(server_fd_https)
  │     join accept thread (≤ 1 s select timeout)
  │     [if TLS] wait on conn_cond until active_connections == 0 (≤ 10 s), then
  │              tls_free_context(ssl_ctx)  ← avoids use-after-free with in-flight handshakes
  │     stop + join the IP-table cleanup thread; free(ip_table)
  │
  ├─ geoip_cleanup()            MMDB_close (no-op without libmaxminddb)
  │
  └─ mongodb_manager_cleanup()
        mongoc_client_pool_destroy() + mongoc_cleanup()
```

Detached connection threads are not joined. Each is bounded by `connection_io_timeout_secs`
per read/write, which is what keeps the 10 s TLS drain wait realistic; on a plain-HTTP-only
server nothing waits for them. The systemd unit allows 10 s (`TimeoutStopSec=10`) before
`SIGKILL`.

---

## 8. WAP Gateway Request

A parallel entry point, on its own thread, that ends up back in §4 over loopback:

```
UDP datagram (rover 49300 / wsp 9200)
  ├─ framing + PDU type check (GET-family only), URI ≤ 1000 bytes
  ├─ rate_allow(source ip)  ← wap_gateway_rate_limit per minute
  ├─ fetch_local("<path>?preview_epoch=-1&wml_pages=all", X-Forwarded-For: <phone>)
  │     → TCP 127.0.0.1:http_port → §2 → §3 → §4 (WML deck, unpaginated)
  ├─ wbxml_compile_page(deck, page = __page) → ≤ 860-byte WBXML page
  └─ sendto(): WTP Result / WSP Reply with application/vnd.wap.wmlc
```

Details: [wap-gateway.md](wap-gateway.md).

---

## Data Structures Summary

| Structure | Location | Purpose |
|---|---|---|
| `connection_ctx_t` | `connection.h` | Holds `client_socket` + `SSL*` for a single connection |
| `thread_args` | `connection_thread.h` | Passed to each pthread: socket, root_dir, ssl pointer |
| `HttpRequest` | `http_request_parser.h` | Parsed HTTP request: method, url, protocol, headers[], body |
| `HttpHeader` | `http_request_parser.h` | Single header key-value pair |
| `QueryParam` | `url_parser.h` | Single URL query parameter key-value |
| `ip_entry_t` | `server_listener.c` (internal) | Per-IP rate limiting state; array of `ddos_max_ips`, guarded by `ip_table_mutex` (accept + cleanup threads) |
| `RateEntry` | `wap_gateway.c` (internal) | WAP gateway per-IP minute window (256 entries, gateway thread only) |
| `WbxmlBuf` | `wap_gateway/wbxml.h` | Compiled WBXML page (`data`, `len`) |
| `CmsEntry`, `CmsContentBlock` | `db/cms_entries.h` | An entry resolved to one language, for rendering |
| `CmsThemeColors` | `db/cms_themes.h` | The theme's 34 color tokens |
| `CmsLogoConfig` / `CmsLogoMode` | `db/cms_themes.h` | Per-epoch logo: mode (unset/text/image), text, font, images |
| `CmsFont` | `db/cms_fonts.h` | Uploaded font: id, name, filename |
| `CodeHighlightPalette` | `utils/code_highlight.h` | Colors and flags for epoch 0-2 highlighting |
| per-request `__thread` state | `utils/request_*.c` | Language, path, theme, user name, charset, code-lines, WML target |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
