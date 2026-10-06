# Boat Rudder - Security

One place for every defensive mechanism in the server, from the socket to the dashboard, plus the
gaps that are known and accepted (or still open). The non-negotiable coding rules that keep these
properties true are in [style-guide.md](style-guide.md); configuration keys are in
[configuration.md](configuration.md).

---

## Threat model in one paragraph

Boat Rudder is meant to be exposed directly to the internet, often on small hardware, serving
browsers that can't do TLS or JavaScript. The public surface is read-only (GET pages, static
files, a UDP WAP gateway); everything that writes is behind a session restricted to modern
browsers. The main concerns are therefore resource exhaustion (floods, slow clients, UDP
amplification), path traversal and injection through URLs and uploads, and session/role
enforcement in the dashboard.

---

## Connection-level defenses

Implemented in `src/web_server/server_listener.c` and `connection_thread.c`, tuned by the
`ddos_*` and `connection_io_timeout_secs` keys.

| Defense | Mechanism | Default |
|---|---|---|
| **Global connection cap** | Mutex-protected `active_connections` counter checked at `accept()` (`try_register_connection()`) | `ddos_max_connections` = 200 |
| **Per-IP rate limit** | `too_many_connections()`: connections from one IPv4 address less than `ddos_rate_window_secs` apart are counted; above `ddos_rate_limit` the IP is **blocked for 2 × window** | 500 per 5 s → 10 s ban |
| **IP table** | `ip_entry_t[ddos_max_ips]`, `calloc`'d in `server_start()`, guarded by `ip_table_mutex` (accept thread + cleanup thread). Full table → least-recently-seen entry evicted (LRU) | 1024 entries |
| **Cleanup thread** | `ip_table_cleanup_thread()` wakes every `ddos_cleanup_interval_secs`, lifts expired bans and frees entries idle for `ddos_ip_stale_secs`, so a long-running process doesn't fill the table with dead IPs | 60 s / 300 s |
| **`429` on rejection** | Rejected **plain-HTTP** connections get a best-effort `429 Too Many Requests` (`Retry-After: 10`) before close. Rejected **HTTPS** connections are just closed: answering would require completing the TLS handshake - exactly the CPU cost the rejection exists to avoid | - |
| **Slow-loris** | `SO_RCVTIMEO`/`SO_SNDTIMEO` on every client socket. Per read/write call, not per transfer, so slow-but-steady uploads still work | `connection_io_timeout_secs` = 5 s |
| **Header cap** | Header block read into `RAW_REQUEST_SIZE` = 32 KiB; larger → `431` | - |
| **Body cap** | `Content-Length` > `MAX_BODY_SIZE` = 10 MiB → `413` before reading | - |
| **Legacy-client wait** | A versioned request line without a terminating blank line gets one 400 ms `poll()` window (plain HTTP only) before being treated as complete - bounded, so it can't be used to park a thread | 400 ms |
| **Thread stack** | 2 MiB per connection thread; no thread pool, so the connection cap is the memory bound | - |
| **SIGPIPE** | Ignored process-wide and per thread; `SO_NOSIGPIPE` where available | - |

The per-IP limiter only sees the **socket peer address**. Behind a reverse proxy every client
shares the proxy's IP, so put rate limiting in the proxy too (or raise the limits).

---

## Client IP and proxies

`X-Real-IP` / `X-Forwarded-For` are honored **only** when the TCP peer is listed in
`trusted_proxies` (`is_trusted_proxy()`); otherwise the socket address is used. The resulting
`client_ip` feeds logging and the analytics country lookup. The connection-level limiter always
uses the socket address, never a header.

The WAP gateway's loopback fetches carry the phone's address in `X-Forwarded-For`; it is honored
only if `127.0.0.1` is a trusted proxy.

---

## TLS

`tls_context.c`: TLS 1.2 minimum, hardened cipher suites for TLS 1.2 (ECDHE + AES-GCM / ChaCha20)
and TLS 1.3, no RC4/3DES/export ciphers. The handshake runs on the connection thread after the
accept-time checks. Retro browsers that can't do modern TLS use plain HTTP - by design, both
listeners serve the same site.

---

## Request handling

| Concern | Protection |
|---|---|
| Path traversal (static files) | `sanitize_path()`: `realpath()` of root + URL must stay inside the root directory |
| Theme keys (cookie, query, admin forms, asset API) | `theme_key_is_valid()`: charset `[A-Za-z0-9_-]` checked **before** touching the filesystem, then the directory must exist under `html/themes/` - blocks `../` and CRLF headed for `Set-Cookie` |
| Open redirects (`return`, `back` parameters) | `language_sanitize_return()`: must start with a single `/` (no `//`, no `/\`), no control characters, `"`, `<`, `>`; anything else becomes `/` |
| Uploaded file names (media, theme assets, fonts) | Whitelist `[A-Za-z0-9._-]`, spaces → `-`; theme assets and fonts also reject a leading `.` and any `..` |
| Upload types | Theme assets: `.png/.jpg/.jpeg/.gif` (epoch −1: PNG/JPEG only, converted to WBMP). Fonts: `.ttf/.otf/.woff/.woff2`. Media library: `.jpg/.jpeg/.png/.gif/.webp` **and** matching magic bytes (`media_upload_is_allowed_image()`), else `415`; if `image-optimizer.sh` does not report a variant it wrote, the raw upload is deleted and `415` returned - the original is never published as-is |
| Media directory names | `media_dir_name_valid()`: 3-60 chars `[A-Za-z0-9_-]` (no `/`, no `.`), on create **and** rename; names read back from the DB are re-checked before they reach the filesystem or the optimizer's command line |
| Media paths on disk | Built only from DB records - directory name + the **owner's** username (`media_username_valid()` rejects `""`, `.`, `..`, leading dots) - never from a client-supplied name or path. A legacy record that fails the checks is left alone on disk |
| Optimizer invocation | `popen()` with single-quoted arguments; every component (username, directory, file name) is restricted to `[A-Za-z0-9._-]`, so the quoting can't be broken |
| Font names | `[A-Za-z0-9 _-]` only - the value is injected into CSS and stored in MongoDB |
| Theme raw markup (banner/footer/CSS) | Admin-only; inserted with `str_replace_*`, never as a `printf` format; capped at `THEME_ASSET_HTML_MAX` = 128 KiB |
| Template rendering | User content passed as `render_template()` *arguments*, never as the format string |
| Output encoding | `html_encode()` for site name, titles, alt text, analytics keys; `json_escape_alloc()` for JSON |
| MongoDB keys from user data | Analytics keys sanitized (`.`/`$` → `_`) before becoming `$inc` paths |
| Response headers | `X-Content-Type-Options: nosniff`, `X-Frame-Options: SAMEORIGIN` on pages and static files |

---

## Authentication and sessions

| Property | Implementation |
|---|---|
| Password storage | libsodium `crypto_pwhash_str()` (Argon2id); verified with `crypto_pwhash_str_verify()` |
| No user enumeration | `auth_login_user()` returns the same `NULL` for unknown email, wrong password and DB error |
| Session token | 32 bytes from libsodium's CSPRNG, hex-encoded, stored server-side in `sessions` with `expires_at` |
| Cookie | `session=<token>; HttpOnly; Path=/; SameSite=Lax; Max-Age=<ttl>` plus `; Secure` when `ssl_enabled=1` |
| Logout | `GET /logout` deletes the session document and clears the cookie |
| Expiry | Checked on every lookup (`validate_session()`). At startup `session_manager_ensure_indexes()` creates a TTL index `{expires_at: 1}, {expireAfterSeconds: 0}` (MongoDB then purges expired sessions about once a minute), a unique index on `token`, and deletes the backlog of already-expired sessions |
| CSRF | Every non-`GET`/`HEAD`/`OPTIONS` request through `require_dashboard_session()` (i.e. every dashboard write) must carry the session's CSRF token, else `403`. See [CSRF protection](#csrf-protection) |
| Modern browsers only | `/login` POST and every `/dashboard*` route require epoch 3 (server-side, not just hidden links) |
| MongoDB down | Dashboard and login answer `503`, never "logged in" |

### Roles

| Role | Can |
|---|---|
| `admin` (also: missing `role` field) | Everything |
| `author` | Dashboard home, create **blog** entries, edit/preview **their own** blog entries; in the media library, browse everything but upload into, rename, delete and move **only their own** directories and images |

Guards are enforced per route in `http_router.c` (`require_admin_session()`,
`require_dashboard_session_role()` + `can_edit_entry()`); the full matrix is in
[routes.md](routes.md#guards). The last admin can't be deleted or demoted, and nobody can delete
their own account.

### Media ownership

`media_can_manage()` in `http_router.c`: an `admin` manages every author's media; an `author`
only records whose `author_id` is theirs. Applied to directory rename/delete (`403`), image
delete/move (per item; `403` when nothing was allowed) and upload (the target directory must be
the caller's own). Images move only between directories of **their own author** - files live under
`html/content/posts/<author>/`, so another author's directory is not a valid target, for admins
either. A directory is deleted only when it has no `media` records (`409` otherwise) and the
record is removed only after `rmdir()` succeeds (or the directory never existed on disk).

### CSRF protection

- **Token**: `derive_csrf_token()` (`session_manager.c`) = keyed BLAKE2b (libsodium
  `crypto_generichash`) of the session token, hex-encoded. Bound to the session, stable for its
  lifetime, nothing stored, and not computable without the `HttpOnly` cookie.
- **Delivery**: `page_layout_wrap()` adds `<meta name="csrf-token">` and
  `/assets/js/csrf.js` before `</head>` on epoch-3 pages served to a signed-in user
  (`request_csrf_token()`, resolved per request in `request_user_set()`). The script adds an
  `X-CSRF-Token` header to same-origin `fetch()`/`XMLHttpRequest` writes and a hidden
  `csrf_token` field to `<form method="post">` on submit - templates don't carry the token
  themselves.
- **Check**: `require_dashboard_session()` reads the header, else the `csrf_token` field of an
  urlencoded or multipart body, and compares in constant time (`verify_csrf_token()`).
  Failures are logged (`CSRF check failed`) and answered `403`.
- `SameSite=Lax` on the cookie stays as a second layer. `/login` (no session yet) and
  `GET /logout` are not covered - see gaps.

### Drafts

Unpublished entries (`enabled: false`) are served only to a visitor with a valid dashboard
session (any role), marked `[Draft]` and sent with `Cache-Control: no-store`; everyone else gets
`404`.

---

## WAP gateway (UDP)

UDP has no handshake, so a spoofed request makes the gateway send its (~20× larger) reply to a
victim. `rate_allow()` caps answers at `wap_gateway_rate_limit` per minute per source IP (default
60) in a 256-entry table; excess requests are dropped silently after one log line. The gateway
only answers GET-family PDUs, only on the addresses in `wap_gateway_ips`, and fetches pages
through the HTTP listener (so the HTTP limits apply too). Details:
[wap-gateway.md](wap-gateway.md#abuse-protection).

---

## Privacy

Analytics stores no IP addresses and no per-visit records (see
[analytics.md](analytics.md#privacy)). The log file can contain request lines, and at
`verbose_level=4` client IPs and User-Agents; it is rotated weekly by `logrotate` and kept for 8
rotations.

---

## Known gaps

Observed in the code as of this revision; listed so they are visible, not because they are
accepted forever.

| Gap | Impact | Possible fix |
|---|---|---|
| **Service runs as `root`** (`scripts/boat-rudder.service`) | Any future file-write or command-injection bug gets full control of the host | Dedicated user owning `html/content` and `html/assets`, `AmbientCapabilities=CAP_NET_BIND_SERVICE` for ports 80/443, systemd sandboxing (`ProtectSystem=strict`, `ReadWritePaths=`); check what the WAP gateway's sockets need first |
| Login CSRF and `GET /logout` | A third-party page can sign a visitor out, or (only from a browser that ignores `SameSite`) into an attacker's account | Origin check or a pre-session token on `/login`; make logout a `POST` |
| No login throttling | Online password guessing is limited only by the connection rate limit | Per-account/IP failure counter |
| Analytics writes are synchronous | Each counted page view costs two MongoDB round-trips on the request thread | Batch or async writes |
| Repository hygiene | `db_backup*/` track `users.bson` (password hashes) and `sessions.bson` (tokens); `data/GeoLite2-Country.mmdb` is tracked despite its license | Remove from version control and rotate affected credentials |

### Resolved

| Former gap | Fix |
|---|---|
| Media directory rename/delete built paths from the client's `oldname`/`newname`/`dirname` (`../` reached outside `html/`, with the service running as root) | Paths come from the `media_directories` record and its owner; `newname` must pass `media_dir_name_valid()`; duplicates `409`; non-empty directories `409`; the record is deleted only after a successful `rmdir()` - see [Request handling](#request-handling), [Media ownership](#media-ownership) |
| Media uploads accepted any extension (stored XSS via `.html`/`.svg`) | Extension + magic-byte allowlist, and only optimizer output is kept |
| No ownership checks in the media library | `media_can_manage()` on every media write |
| No CSRF tokens (relied on `SameSite=Lax` only) | Session-bound token checked in `require_dashboard_session()` - [CSRF protection](#csrf-protection) |
| Expired sessions never purged | TTL index + startup purge (`session_manager_ensure_indexes()`) |
| Optimizer `popen()` quoting could be broken by a `'` in a path component | All components are charset-restricted (usernames were already mapped by `cms_get_username_by_id()`; directory names are now validated on rename too) |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
