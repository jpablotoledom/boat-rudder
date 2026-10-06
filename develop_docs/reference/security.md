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
| Upload types | Theme assets: `.png/.jpg/.jpeg/.gif` (epoch −1: PNG/JPEG only, converted to WBMP). Fonts: `.ttf/.otf/.woff/.woff2`. Media library: **no extension check** (see gaps) |
| Media directory names | 3-60 chars `[A-Za-z0-9_-]`, no `..` |
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
| Modern browsers only | `/login` POST and every `/dashboard*` route require epoch 3 (server-side, not just hidden links) |
| MongoDB down | Dashboard and login answer `503`, never "logged in" |

### Roles

| Role | Can |
|---|---|
| `admin` (also: missing `role` field) | Everything |
| `author` | Dashboard home, create **blog** entries, edit/preview **their own** blog entries, the whole media library |

Guards are enforced per route in `http_router.c` (`require_admin_session()`,
`require_dashboard_session_role()` + `can_edit_entry()`); the full matrix is in
[routes.md](routes.md#guards). The last admin can't be deleted or demoted, and nobody can delete
their own account.

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
| **No CSRF tokens** on dashboard `POST` routes | Mitigated by `SameSite=Lax` (cross-site POSTs don't carry the cookie in modern browsers); still relies on browser behavior | Per-session token in forms and AJAX headers |
| **Media library has no ownership checks**: any signed-in user (including `author`) can delete, move, rename or delete directories of any author's media | An author can destroy others' media | Check `media.author_id` / `media_directories.author_id` against the session for non-admins |
| **Media uploads accept any extension** (only the filename charset is sanitized); files are served from the site's own origin | A dashboard user could upload `.html`/`.svg` with script - stored XSS against other dashboard users | Extension/content-type allowlist matching what `image-optimizer.sh` handles |
| Optimizer is invoked through `popen()` with paths in single quotes; the username part comes from the user's email | An email containing `'` (created by an admin) could break out of the quoting | Validate emails or `exec` without a shell |
| **Media directory rename/delete trust client-supplied names**: `/dashboard/api/media/directory/rename` builds both paths from the body's `oldname`/`newname`, and `/directory/delete` from `dirname`, joined to the *caller's* username with no validation; delete also removes the DB record even when `rmdir()` fails | Any signed-in user (including `author`) can `rename()` arbitrary directories reachable through `../` - and the shipped systemd unit runs as **root** - or `rmdir` empty ones; deleting a non-empty directory orphans its files and `media` records | Resolve paths from the `media_directories` record (name + author), apply the same `[A-Za-z0-9_-]` rule as creation to `newname`, reject `/` and `..`, only delete the record after a successful `rmdir()`; run the service as an unprivileged user |
| Expired sessions are never purged | `sessions` grows; old tokens remain in the DB (unusable) | TTL index on `expires_at` - see [data-model.md](data-model.md#index-recommendations) |
| No login throttling | Online password guessing is limited only by the connection rate limit | Per-account/IP failure counter |
| Analytics writes are synchronous | Each counted page view costs two MongoDB round-trips on the request thread | Batch or async writes |
| Repository hygiene | `db_backup*/` track `users.bson` (password hashes) and `sessions.bson` (tokens); `data/GeoLite2-Country.mmdb` is tracked despite its license | Remove from version control and rotate affected credentials |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
