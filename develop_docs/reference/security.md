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
| **Per-IP rate limit** | `too_many_connections()`: connections from one address less than `ddos_rate_window_secs` apart are counted; above `ddos_rate_limit` the IP is **blocked for 2 × window**. Checked at `accept()` against the socket peer - or, with `proxy_protocol=1` and a trusted peer, on the connection thread against the PROXY header's address (`server_ip_rate_limited()`) | 500 per 5 s → 10 s ban |
| **IP table** | `ip_entry_t[ddos_max_ips]` keyed by the 16-byte IPv6 form of the address (IPv4 as `::ffff:a.b.c.d`), `calloc`'d in `server_start()`, guarded by `ip_table_mutex` (accept thread, connection threads, cleanup thread). Full table → least-recently-seen entry evicted (LRU) | 1024 entries |
| **Cleanup thread** | `ip_table_cleanup_thread()` wakes every `ddos_cleanup_interval_secs`, lifts expired bans and frees entries idle for `ddos_ip_stale_secs`, so a long-running process doesn't fill the table with dead IPs | 60 s / 300 s |
| **`429` on rejection** | Rejected **plain-HTTP** connections get a best-effort `429 Too Many Requests` (`Retry-After: 10`) before close. Rejected **HTTPS** connections are just closed: answering would require completing the TLS handshake - exactly the CPU cost the rejection exists to avoid | - |
| **Slow-loris** | `SO_RCVTIMEO`/`SO_SNDTIMEO` on every client socket. Per read/write call, not per transfer, so slow-but-steady uploads still work | `connection_io_timeout_secs` = 5 s |
| **Header cap** | Header block read into `RAW_REQUEST_SIZE` = 32 KiB; larger → `431` | - |
| **Body cap** | `Content-Length` > `MAX_BODY_SIZE` = 10 MiB → `413` before reading | - |
| **Legacy-client wait** | A versioned request line without a terminating blank line gets one 400 ms `poll()` window (plain HTTP only) before being treated as complete - bounded, so it can't be used to park a thread | 400 ms |
| **Thread stack** | 2 MiB per connection thread; no thread pool, so the connection cap is the memory bound | - |
| **SIGPIPE** | Ignored process-wide and per thread; `SO_NOSIGPIPE` where available | - |

Without `proxy_protocol`, the per-IP limiter only sees the **socket peer address**: behind an
HTTP reverse proxy every client shares the proxy's IP, so put rate limiting in the proxy too (or
raise the limits). With `proxy_protocol=1` the accept loop skips the per-IP check for trusted
peers - the front, whose address stands for every visitor - and the connection thread applies it
to the visitor's own address once the PROXY header is read; a blocked visitor gets the same
best-effort `429` (plain HTTP) or plain close (HTTPS). The global connection cap stays at
`accept()`.

---

## Client IP and proxies

`X-Real-IP` / `X-Forwarded-For` are honored **only** when the TCP peer is listed in
`trusted_proxies` (`is_trusted_proxy()`); otherwise the socket address is used. The resulting
`client_ip` feeds logging and the analytics country lookup. The connection-level limiter never
uses these headers.

**PROXY protocol** (`proxy_protocol=1`, `src/web_server/proxy_protocol.c`, read in
`connection_thread.c` before `SSL_accept()` and the HTTP parser). A site behind HAProxy in
`mode tcp` - several sites on one machine, as the multi-br project sets them up - gets the
client's bytes untouched, prefixed with a PROXY v2 header carrying the client's address.
The header's address wins over `X-Real-IP` / `X-Forwarded-For` and is what the per-IP limiter
counts.

| Connection | Outcome |
|---|---|
| PROXY header from a `trusted_proxies` peer | Accepted; its source address is the client's (v2 `LOCAL`, v1 `UNKNOWN` and non-IP families keep the socket address) |
| PROXY header from any other peer | Closed - it could claim any address |
| No header, loopback peer | Served as a direct connection (the WAP gateway, the updater's health check) |
| No header, any other peer | Closed - it bypassed the front |
| Malformed, longer than 528 bytes, or not complete within `connection_io_timeout_secs` | Closed |

Only the first header is read, so a client can't forge one through the front: the front's own
header always comes first, and a second `PROXY ...` line is just the start of a malformed HTTP
request. The header is peeked (`MSG_PEEK`) before anything is consumed, so a connection without
one reaches the HTTP parser or the TLS handshake byte for byte. A site behind a front binds `127.0.0.1`
(`listen_address`) so no outside peer can reach it directly in the first place.

The WAP gateway's loopback fetches carry the phone's address in `X-Forwarded-For`, honored only
if `127.0.0.1` is a trusted proxy; with `proxy_protocol=1` and its listener's address trusted
the gateway sends a PROXY v1 header with the phone's address instead (see
[wap-gateway.md](wap-gateway.md)).

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
| Upload types | Theme assets: `.png/.jpg/.jpeg/.gif` (epoch −1: PNG only, kept with a WBMP twin), written under `html/content/themes/<key>/`; the home blog background fields are filenames re-sanitized server-side before going into a CSS `url()` / HTML attribute. Favicons: a structurally valid `.ico` only (ICONDIR header, every entry inside the file, ≤ 256 KiB), to the fixed names `html/favicon2.ico` / `favicon3.ico`. Fonts: `.ttf/.otf/.woff/.woff2`. Media library: JPEG, PNG, GIF or WebP **by magic bytes** (`media_image_extension()`), else `415`; the stored name gets that format's extension (`media_name_with_extension()`: a WebP sent as `photo.jpg` is stored as `photo.webp`), so name and content always agree. The name itself keeps only `[A-Za-z0-9_-]` plus the extension dot (`media_sanitize_upload_name()`), and an existing name gets `_2`, `_3`... instead of overwriting another upload's files; if `image-optimizer.sh` does not report a variant it wrote, the raw upload is deleted and `415` returned - the original is never published as-is |
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
| Logout | `POST /logout` (CSRF-checked whenever a session cookie is present; `GET` → `405`) deletes the session document and clears the cookie |
| Expiry | Checked on every lookup (`validate_session()`). At startup `session_manager_ensure_indexes()` creates a TTL index `{expires_at: 1}, {expireAfterSeconds: 0}` (MongoDB then purges expired sessions about once a minute), a unique index on `token`, and deletes the backlog of already-expired sessions |
| CSRF | Every non-`GET`/`HEAD`/`OPTIONS` request through `require_dashboard_session()` (i.e. every dashboard write) must carry the session's CSRF token, else `403`. See [CSRF protection](#csrf-protection) |
| Modern browsers only | `/login` POST and every `/dashboard*` route require epoch 3 *and* the `dashboard` flag the browser detection rules give the real `User-Agent` (`dashboard_allowed()`, server-side, not just hidden links) - so `?preview_epoch=3` cannot lift an older browser in. Other epoch 3 browsers see "needs a more recent browser" |
| Detection rules lock-out | Saving browser detection rules is refused (`409`) when the saving browser would lose the dashboard under them. Last resort: delete the `browser_detection` document and restart - the factory rules apply |
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
- `POST /logout` runs the same check inline (it doesn't go through
  `require_dashboard_session()`, so an expired session can still clear its cookie).
- `SameSite=Lax` on the cookie stays as a second layer. `/login` (no session yet) is not
  covered - see gaps.

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

## Updates

Updating a site means compiling and running new code as root, so the updater
([scripts.md](scripts.md#scriptsboat-rudder-update)) trusts only the maintainer's signature:

- Every package must carry a minisign signature that verifies against the key **already
  installed** in `.boat-rudder/minisign.pub` - never a key shipped inside the package. Downloads
  come over HTTPS from GitHub, but the signature, not the transport, is what's trusted. A package
  may replace the key only by being signed with the current one.
- The secret key lives on the maintainer's machine, password-protected - not in the repository,
  on GitHub or on any server ([`scripts/release.sh`](scripts.md#scriptsreleasesh)).
- Extraction refuses anything but regular files and directories (symlinks, hard links, devices),
  absolute paths, `..`, more than one top directory and archives over 1 GB; files are extracted
  without their archived owners.
- Only paths in the installed or the new manifest are written or deleted; `html/content/`,
  `configs/`, `ssl/`, `data/` and anything else on the host are out of reach. A deletion also
  needs the installed version's official signed package to have shipped the file, so a site's
  own additions installed from its repository are never deleted.
- Every install and rollback is recorded in `.boat-rudder/history.jsonl`.
- `/dashboard/settings/updates` (admin only) checks GitHub by running `curl` with a fixed command
  line - `update_repo` is checked against `[A-Za-z0-9._-]/[A-Za-z0-9._-]` before it goes in - and
  renders the release notes through `markdown_lite.c`, which escapes everything and keeps only
  `http(s)` links.
- Installing or rolling back from it takes the admin role, the CSRF token **and the admin's
  password typed again** (`confirm_password()`; a wrong one is logged). Even then the dashboard
  can only start one of two things: install a release that is signed, newer than the running
  version and in the last check's list, or undo the last update. The server never runs the update
  itself: it writes a request that the updater, as its own systemd unit, validates field by field
  before acting. A stolen session alone gets nowhere; a stolen session and password get an
  official version, or the previous one.

---

## Known gaps

Observed in the code as of this revision; listed so they are visible, not because they are
accepted forever.

| Gap | Impact | Possible fix |
|---|---|---|
| **Service runs as `root`** (`scripts/boat-rudder.service`) | Any future file-write or command-injection bug gets full control of the host | Dedicated user owning `html/content` and `html/assets`, `AmbientCapabilities=CAP_NET_BIND_SERVICE` for ports 80/443 (not needed by a site behind a front, which listens on high ports), systemd sandboxing (`ProtectSystem=strict`, `ReadWritePaths=`); check what the WAP gateway's sockets and the updater need first |
| Login CSRF | A third-party page can (only from a browser that ignores `SameSite`) sign a visitor into an attacker's account | Origin check or a pre-session token on `/login` |
| No login throttling | Online password guessing is limited only by the connection rate limit | Per-account/IP failure counter |
| Analytics writes are synchronous | Each counted page view costs two MongoDB round-trips on the request thread | Batch or async writes |
| Repository hygiene | `db_backup*/` track `users.bson` (password hashes) and `sessions.bson` (tokens); `data/GeoLite2-Country.mmdb` is tracked despite its license | Remove from version control and rotate affected credentials |

### Resolved

| Former gap | Fix |
|---|---|
| Media directory rename/delete built paths from the client's `oldname`/`newname`/`dirname` (`../` reached outside `html/`, with the service running as root) | Paths come from the `media_directories` record and its owner; `newname` must pass `media_dir_name_valid()`; duplicates `409`; non-empty directories `409`; the record is deleted only after a successful `rmdir()` - see [Request handling](#request-handling), [Media ownership](#media-ownership) |
| Media uploads accepted any extension (stored XSS via `.html`/`.svg`) | Magic-byte allowlist that also sets the stored extension, and only optimizer output is kept |
| No ownership checks in the media library | `media_can_manage()` on every media write |
| No CSRF tokens (relied on `SameSite=Lax` only) | Session-bound token checked in `require_dashboard_session()` - [CSRF protection](#csrf-protection) |
| Expired sessions never purged | TTL index + startup purge (`session_manager_ensure_indexes()`) |
| `GET /logout` let a third-party page sign a visitor out | `POST /logout` with the session's CSRF token; `GET` → `405` |
| Optimizer `popen()` quoting could be broken by a `'` in a path component | All components are charset-restricted (usernames were already mapped by `cms_get_username_by_id()`; directory names are now validated on rename too) |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
