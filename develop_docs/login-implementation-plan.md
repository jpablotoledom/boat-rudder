# Login Feature - Implementation Plan

> **Status**: Planning document only. No source code, templates, CMake, or config files are
> changed by this document. It describes the work required to add a login flow, a protected
> `/dashboard` page, and centralized epoch-aware error pages to Boat Rudder.

---

## 1. Goals and Constraints

- Add a login flow (`GET /login`, `POST /login`) backed by **MongoDB**.
- Login is functional **only for `EPOCH_MODERN` (epoch 3)**, for security reasons. For
  `EPOCH_WML`, `EPOCH_PRESTANDARD`, `EPOCH_EARLY` and `EPOCH_MIDDLE` (epochs `-1, 0, 1, 2`),
  `/login` must render a "this functionality is not available" message in that epoch's own
  markup style - no form, no credential handling.
- On successful login, redirect (HTTP `302`) to a static `/dashboard` page that for now simply
  shows **"Welcome to dashboard"**.
- Introduce **centralized, epoch-aware HTTP error/response templates** (`3xx`/`4xx`/`5xx`),
  following the same per-epoch template convention already used for `container`, `menu`,
  `slider`, etc.
- Everything below follows the conventions documented in
  [architecture.md](architecture.md) and [boat-rudder.md](boat-rudder.md): malloc'd-string
  modules, `generate_url_theme()` + `read_file_to_string()` + `render_template()`, one
  `http_router.c`, AddressSanitizer-clean.
- Reference project: `../the-retro-center` (same author, more mature sibling project) is used
  for MongoDB driver choice, session/auth patterns, and HTML ideas. Its `html/themes/dark/error/`
  directory is **empty** - the error-template system below is a new design for both projects,
  not a direct port.

---

## 2. High-Level Design Summary

```
GET  /login      -> epoch 3: render login form
                     epoch -1/0/1/2: render "not available" message
POST /login      -> epoch 3 only: validate credentials against MongoDB
                       success -> create session, Set-Cookie, 302 -> /dashboard
                       failure -> re-render login form with error (200, epoch 3)
                     epoch -1/0/1/2: same "not available" message (no auth attempted)
GET  /dashboard  -> requires valid session cookie
                       valid   -> static "Welcome to dashboard" page (epoch-aware shell)
                       invalid -> 302 -> /login
GET  /logout     -> destroys session, clears cookie, 302 -> /
*    error cases -> centralized epoch-aware error templates (replaces send_simple())
```

New architectural pieces:

1. **`src/db/`** - new top-level layer: MongoDB connection pool, password hashing/verification,
   session management. Mirrors `the-retro-center/src/api/` but named `db/` to avoid implying a
   REST API surface that doesn't exist here.
2. **A generic "page" template shell** (`page_epoch<N>.html`) - `container_epoch<N>.html` is
   hard-wired to the home page (4 slots: menu/slider/home_content/home_blog, plus the
   home-blog lightbox modal). Login, dashboard and error pages need a simpler 2-slot shell
   (menu + content). This is a new template family, not a change to `container`.
3. **Three new modules**: `src/modules/login/`, `src/modules/dashboard/`, `src/modules/error/`,
   following the exact pattern of `src/modules/home_content/`.
4. **Extended `build_epoch_response()`** to support arbitrary status lines, `Set-Cookie`/
   `Location` headers, and a new redirect-response helper.
5. **New orchestrator entry points** alongside `buildHomeWebSite()`.

---

## 3. New Layer: `src/db/` (MongoDB, Auth, Sessions)

### 3.1 Dependencies

Boat Rudder is currently dependency-light (OpenSSL + pthreads only). Adding MongoDB support
means accepting two new dependencies, both available via `pkg-config` on Debian/Ubuntu and
already used successfully in `the-retro-center`:

| Dependency | Package(s) | Purpose |
|---|---|---|
| **mongo-c-driver** | `libmongoc-1.0`, `libbson-1.0` | MongoDB wire protocol client |
| **libsodium** | `libsodium` | Argon2id password hashing/verification (`crypto_pwhash_str*`) |

This is a deliberate trade-off documented here explicitly: writing a hand-rolled MongoDB
client or password-hashing routine would violate "don't build infrastructure you don't need",
and both libraries are mature, widely-packaged, and already proven in `the-retro-center`.

`CMakeLists.txt` additions (see §10 for the full diff description):

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
...
target_link_libraries(base-http-server
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MONGOC
    PkgConfig::SODIUM
)
```

### 3.2 `src/db/mongodb_manager.c/h`

Mirrors `the-retro-center/src/api/mongodb_manager.c`:

- `int mongodb_manager_init(const char *uri, const char *db_name)` - creates a thread-safe
  `mongoc_client_pool_t` from `uri`, called once from `main.c` after `load_config()` and before
  `server_start()`. Returns `0` on success, `-1` on failure (logged via `LOG_ERROR`, server
  still starts so static pages and the home CMS keep working even if MongoDB is down -
  `/login` and `/dashboard` would then degrade to `503`/error pages, see §9).
- `void mongodb_manager_cleanup(void)` - destroys the pool, called from `main.c` during
  shutdown alongside existing cleanup.
- `mongoc_client_t *mongodb_manager_acquire(void)` / `void mongodb_manager_release(mongoc_client_t *)`
  - thin wrappers around `mongoc_client_pool_pop`/`push`, used by `auth.c` and
    `session_manager.c` so every DB-touching code path follows the same
    acquire/use/release pattern (kept short to bound lock hold time).
- Collection name constants: `#define USERS_COLLECTION "users"`, `#define SESSIONS_COLLECTION "sessions"`.

### 3.3 Collections & Schema

**`users`** (seeded manually for the MVP - no signup flow is in scope):

```jsonc
{
  "_id": ObjectId,
  "email": "user@example.com",   // UTF8, unique index
  "password": "$argon2id$...",   // libsodium crypto_pwhash_str() output
  "name": "Display Name",        // UTF8, optional, shown on /dashboard later
  "created_at": ISODate
}
```

**`sessions`**:

```jsonc
{
  "_id": ObjectId,
  "user_id": ObjectId,           // ref into users._id
  "token": "64-hex-chars",       // sodium_bin2hex(randombytes_buf(32))
  "created_at": ISODate,
  "expires_at": ISODate          // created_at + session_ttl_seconds
}
```

A TTL index on `sessions.expires_at` (`db.sessions.createIndex({expires_at: 1}, {expireAfterSeconds: 0})`)
lets MongoDB garbage-collect expired sessions automatically; this is a one-time DB-side setup
step, documented here but not part of the C code.

### 3.4 `src/db/auth.c/h`

Mirrors `the-retro-center/src/api/auth/auth.c`:

```c
// Returns a malloc'd ObjectId hex string (24 chars + NUL) of the matching user
// on success, or NULL if the email/password pair is invalid or on DB error.
char *auth_login_user(const char *email, const char *password);
```

Implementation: query `users` by `email` (UTF8 exact match), read the `password` field, and
verify with `crypto_pwhash_str_verify(stored_hash, password, strlen(password))`. No new-user
registration is in scope - matches the user's instruction that this plan only covers login.

### 3.5 `src/db/session_manager.c/h`

Mirrors `the-retro-center/src/api/utils/session_manager.c`:

```c
// 64 lowercase-hex chars + NUL, malloc'd. Caller frees.
char *generate_session_token(void);

// Inserts {user_id, token, created_at, expires_at = now + ttl_seconds} into
// `sessions`. Returns 0 on success, -1 on error.
int create_session(const char *user_id_hex, const char *token, int ttl_seconds);

// Looks up `token` in `sessions`, checks expires_at > now, and on success
// writes the 24-char hex user_id into user_id_out (caller-provided buffer,
// >= 25 bytes). Returns 1 if valid, 0 if not found/expired, -1 on DB error.
int validate_session(const char *token, char *user_id_out);

// Convenience used by route handlers: extracts the `session=` cookie value
// from a raw "Cookie:" header value and calls validate_session(). Returns
// the same codes as validate_session(); writes "" to user_id_out if no
// cookie is present.
int validate_session_cookie(const char *cookie_header, char *user_id_out);

// Deletes the session document matching `token` (used by /logout).
int destroy_session(const char *token);
```

Cookie name: `session` (matches `the-retro-center`). Format on login success:

```
Set-Cookie: session=<64-hex-token>; HttpOnly; Secure; Path=/; Max-Age=<session_ttl_seconds>; SameSite=Lax
```

`Secure` is appropriate because Boat Rudder's TLS is enabled by default
(`ssl_enabled=1` in `configs/settings.conf`); if `ssl_enabled=0` the `Secure` attribute should
be omitted so the cookie still works over plain HTTP in local/dev setups - `session_manager.c`
reads `ssl_enabled` from `config_loader` to decide.

---

## 4. New "Page" Template Shell for Non-Home Routes

### 4.1 Why a new shell is needed

`container_epoch<N>.html` (see [architecture.md §2.3](architecture.md)) is the home page's
shell: it has exactly **4** `%s` slots (menu, slider, home_content, home_blog), a hardcoded
footer, and - on epoch 3 - the home-blog lightbox `<div id="homeBlogModal">` + `<script>`.
Reusing it for `/login`, `/dashboard` and error pages would force those pages to fill 4
unrelated slots and ship dead modal markup/JS that references home-blog-only DOM elements.

Instead, add a **second, smaller shell template family**, `page_epoch<N>.html`, with **2**
`%s` slots: menu + content. It keeps the same `<head>` (stylesheet links, `{{PAGE_TITLE}}`,
favicon) and footer branding as `container_epoch<N>.html` for visual consistency, but omits
the slider/home-content/home-blog/modal pieces entirely.

### 4.2 `page_epoch<N>.html` design (per epoch, mirroring `container_epoch<N>.html`)

```
html/themes/dark/
└── page/
    ├── page_epoch-1.html   # WML <card>: %s (menu), %s (content)
    ├── page_epoch0.html    # bare <html>: %s (menu), %s (content), footer text
    ├── page_epoch1.html    # HTML3.2 <table>: %s (menu) row, %s (content) row, footer row
    ├── page_epoch2.html    # HTML4+CSS1 .br-page: %s (menu), %s (content), .br-footer
    └── page_epoch3.html    # HTML5: %s (menu), %s (content), <footer> (no modal/script)
```

Example `page_epoch3.html`:

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
{{PAGE_TITLE}}
<link rel="icon" href="/favicon.ico">
<link rel="stylesheet" href="/themes/dark/styles_epoch3.css">
</head>
<body>
<div class="boat-rudder__main-container">
%s
<main class="boat-rudder__page-content">
%s
</main>
<footer class="boat-rudder__footer">
<div class="boat-rudder__footer-title">BOAT RUDDER</div>
</footer>
</div>
</body>
</html>
```

A small `.boat-rudder__page-content` rule (max-width container + padding, similar to
`.boat-rudder__home-content__container`) is added to `styles_epoch3.css`/`styles_epoch2.css`
so login/dashboard/error content isn't flush against the navbar/footer. Epoch 3's
`.boat-rudder__navbar-space` spacer still applies since `menu_epoch3.html` is reused as-is.

### 4.3 New orchestrator entry point

`src/html_builder/orchestrator.c` gains a second function alongside `buildHomeWebSite()`:

```c
// Generic non-home page: loads page_epoch<N>.html, renders {{PAGE_TITLE}},
// fills the menu slot via menu("/", epoch) [or menu(current_url, epoch) once
// menu() becomes active-link aware] and the content slot with `html_content`
// (already-rendered, e.g. from login()/dashboard()/error_page()).
// Frees html_content. Returns malloc'd page body, or NULL on error.
char *buildPageWebSite(int epoch, const char *page_title, char *html_content);
```

This keeps `buildHomeWebSite()` untouched and gives `/login`, `/dashboard`, and the error
templates a shared, minimal composition path - one new function, not three.

---

## 5. Login Feature

### 5.1 Routes

- `GET /login` (and `HEAD /login`) - renders the login page.
- `POST /login` - processes the login form.

Both are new branches in `src/web_server/http_router.c`'s routing block, alongside the
existing `"/"` check, before the fallback to `serve_static_file()`.

### 5.2 Epoch Restriction Logic

Both `GET` and `POST /login` compute `epoch` exactly as `"/"` does today
(`force_epoch` override, else `detect_epoch(User-Agent)`). Then:

```c
if (epoch != EPOCH_MODERN) {
    // login(epoch) returns the epoch-appropriate "not available" fragment
    char *content = login(epoch);              // "not available" message
    char *body = buildPageWebSite(epoch, "Boat Rudder - Login", content);
    // 200 OK, epoch-aware Content-Type (build_epoch_response)
}
```

`POST /login` for non-modern epochs takes the **same branch** - no body parsing, no DB call.
This guarantees epochs `-1, 0, 1, 2` can never reach `auth_login_user()`, satisfying "el login
de momento solo estará disponible para epoch3, por motivos de seguridad".

### 5.3 Templates: `login_epoch<N>.html`

```
html/themes/dark/login/
├── login_epoch-1.html   # WML: "<p>Login is not available on this device.</p>"
├── login_epoch0.html    # plain text equivalent
├── login_epoch1.html    # HTML3.2 equivalent
├── login_epoch2.html    # HTML4+CSS1 equivalent
└── login_epoch3.html    # full form (this is the only one with a real <form>)
```

`login_epoch3.html` (HTML/CSS ideas ported from
`the-retro-center/html/themes/dark/login/login_epoch3.html`, classes prefixed
`boat-rudder__login__*` to match this project's BEM-ish naming):

```html
<section class="boat-rudder__login__container">
  <h1 class="boat-rudder__login__header">Sign in</h1>
%s
  <form class="boat-rudder__login__form" method="POST" action="/login">
    <label for="user">Email</label>
    <input class="boat-rudder__login__input" type="email" id="user" name="user" required>

    <label for="password">Password</label>
    <input class="boat-rudder__login__input" type="password" id="password" name="password" required>

    <div class="boat-rudder__login__button-container">
      <button class="boat-rudder__login__button" type="submit">Sign in</button>
    </div>
  </form>
</section>
```

The single `%s` is an **error-message slot**: empty string on `GET /login` or first load,
filled with `<p class="boat-rudder__login__error">Invalid email or password.</p>` (or similar)
when `login(epoch3, error_message)` is called after a failed `POST /login`. This keeps the
template's placeholder count fixed regardless of success/failure, consistent with the
"`%s` count must match across epochs... and across calls" rule in
[architecture.md §2.4](architecture.md).

The `-1/0/1/2` variants have **zero** placeholders (they're static "not available" messages),
so `login(epoch)` for those epochs just reads and returns the file content directly - no
`render_template()` call needed (matching `slider_epoch<N>.html`'s "static per epoch, no
placeholders" pattern).

Reusable form-element partials (`html/themes/dark/elements/form/...`,
`elements/input/...`, `elements/button/...`, as in `the-retro-center`) are **not** introduced
in this MVP - a single self-contained `login_epoch3.html` is simpler and there is currently
only one form on the whole site. If a second form (e.g. a future contact page) is added,
extracting shared partials becomes worthwhile and can be revisited then.

### 5.4 Module: `src/modules/login/login.c/h`

```c
// epoch == EPOCH_MODERN: loads login_epoch3.html and fills its single %s with
//   `error_message` (may be "" / NULL for no error).
// epoch != EPOCH_MODERN: loads login_epoch<N>.html and returns it verbatim
//   (error_message is ignored - no form exists to show an error on).
// Returns malloc'd HTML/WML fragment, or NULL on error.
char *login(int epoch, const char *error_message);
```

Same structure as `home_content()`/`slider()`: `generate_url_theme()` ->
`read_file_to_string()` -> (epoch 3 only) `render_template()` -> return.

### 5.5 Handler Flow in `http_router.c`

```c
if (strcmp(decoded_url, "/login") == 0) {
    int epoch = resolve_epoch(&req);   // existing force_epoch / detect_epoch logic, factored out

    if (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0) {
        char *content = login(epoch, NULL);
        char *body = buildPageWebSite(epoch, "Boat Rudder - Login", content);
        // build_epoch_response(body, "", epoch) -> write -> free
    }
    else if (strcmp(req.method, "POST") == 0) {
        if (epoch != EPOCH_MODERN) {
            char *content = login(epoch, NULL);
            char *body = buildPageWebSite(epoch, "Boat Rudder - Login", content);
            // 200 OK, same as GET
        } else {
            // 1. Parse req.body as application/x-www-form-urlencoded:
            //    extract "user" and "password" fields (url-decode each).
            // 2. char *user_id = auth_login_user(user, password);
            // 3a. user_id != NULL:
            //       token = generate_session_token();
            //       create_session(user_id, token, session_ttl_seconds);
            //       build_redirect_response("/dashboard", epoch,
            //           "Set-Cookie: session=<token>; HttpOnly; ...\r\n");
            //       free(user_id); free(token);
            // 3b. user_id == NULL:
            //       content = login(epoch, "Invalid email or password.");
            //       body = buildPageWebSite(epoch, "Boat Rudder - Login", content);
            //       build_epoch_response(body, "", epoch);  // 200, form re-shown with error
        }
    }
    else {
        // 405, via the new error-page system (see §9)
    }
}
```

Body parsing: `req.body` / `req.body_length` are already populated by
`parse_http_request()` (see `src/web_server/http_request_parser.h`) for whatever was read in
the initial `RAW_REQUEST_SIZE` (32 KiB) chunk. A `user=...&password=...` form body is at most
a few hundred bytes, so this is sufficient - no change to the request-reading loop is needed
for this feature. A small new helper, `src/utils/url_parser.c`'s existing `url_decode()` plus
a new `parse_urlencoded_body(body, "user", out, sizeof(out))`-style helper (or inline
`strtok`/`strchr` parsing local to the handler) extracts the two fields.

---

## 6. Session Cookies & Redirects: `build_epoch_response` Extensions

`build_epoch_response()` (`src/utils/build_epoch_response.c`) currently hardcodes
`HTTP/1.1 200 OK` and takes an `extra_headers` string - which already covers `Set-Cookie` on
the success path. Two additions are needed:

1. **Status-line parameter**, so error pages (§9) can emit `400`, `401`, `403`, `404`, `405`,
   `431`, `500`, etc. with an epoch-correct `Content-Type` and `SECURITY_HEADERS`:

   ```c
   // New: explicit status line. build_epoch_response() becomes a thin wrapper:
   //   build_epoch_response(body, extra_headers, epoch)
   //     == build_epoch_response_status(body, extra_headers, epoch, "200 OK")
   char *build_epoch_response_status(const char *body, const char *extra_headers,
                                      int epoch, const char *status_line);
   ```

2. **Redirect helper**, for `302` after login/logout. WML (`epoch -1`) clients may not all
   honor `Location` headers reliably, so the redirect body itself contains a minimal
   epoch-appropriate "click here to continue" fallback:

   ```c
   // Builds "HTTP/1.1 302 Found" with a Location header (+ optional extra
   // headers, e.g. Set-Cookie) and a tiny epoch-aware HTML/WML body containing
   // a link to `location` for clients that don't auto-follow redirects.
   char *build_redirect_response(const char *location, const char *extra_headers, int epoch);
   ```

Both are additive - no existing call sites change behavior (`build_epoch_response()` keeps its
current signature and `200 OK` default).

---

## 7. Dashboard Feature

### 7.1 Route: `GET /dashboard`

New branch in `http_router.c`, before the static-file fallback:

```c
if (strcmp(decoded_url, "/dashboard") == 0 &&
    (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0)) {

    int epoch = resolve_epoch(&req);
    char user_id[25];
    const char *cookie = get_header_value(&req, "Cookie");

    if (validate_session_cookie(cookie, user_id) != 1) {
        // build_redirect_response("/login", "", epoch);
    } else {
        char *content = dashboard(epoch);
        char *body = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
        // build_epoch_response(body, "", epoch);
    }
}
```

Even though login is epoch-3-only, `/dashboard` itself is rendered through the normal
epoch-aware pipeline (so directly visiting `/dashboard` from any browser gets a correctly
styled page) - it's just that, in practice, only epoch-3 clients can ever obtain a valid
session cookie.

### 7.2 Templates: `dashboard_epoch<N>.html`

```
html/themes/dark/dashboard/
└── dashboard_epoch{-1,0,1,2,3}.html   # static, no placeholders: "Welcome to dashboard"
```

Per the user's instruction ("de momento será un html estatico 'Welcome to dashboard'"), each
variant is a static fragment in that epoch's idiom, e.g. epoch3:
`<section class="boat-rudder__page-content"><h1>Welcome to dashboard</h1></section>`; epoch0:
`<h1>Welcome to dashboard</h1>`; epoch-1 (WML): `<p>Welcome to dashboard</p>`.

### 7.3 Module: `src/modules/dashboard/dashboard.c/h`

```c
// Static per-epoch fragment, no placeholders - same shape as slider().
char *dashboard(int epoch);
```

---

## 8. Logout

Small addition that completes the session lifecycle (not explicitly requested, but a login
flow without a way to end the session is incomplete and `destroy_session()` is already needed
for hygiene):

- `GET /logout` - reads the `session` cookie, calls `destroy_session(token)` (no-op if absent
  or already expired), responds with `build_redirect_response("/", "Set-Cookie:
  session=; Max-Age=0; Path=/\r\n", epoch)`.
- No epoch restriction needed - logging out is always safe and only meaningful if a session
  cookie exists.

---

## 9. Centralized Epoch-Aware Error Pages

### 9.1 Template family: `error_epoch<N>.html`

```
html/themes/dark/error/
├── error_epoch-1.html   # WML <card>: %s (code), %s (message)
├── error_epoch0.html    # bare <html>: %s, %s
├── error_epoch1.html    # HTML3.2 table: %s, %s
├── error_epoch2.html    # HTML4+CSS1 .br-page: %s, %s
└── error_epoch3.html    # HTML5: %s (code), %s (message)
```

One **generic** template per epoch (not one per status code) with two `%s` placeholders -
status code and human-readable message - rendered through `buildPageWebSite()` like any other
page. Example `error_epoch3.html` content:

```html
<section class="boat-rudder__page-content boat-rudder__error">
  <h1>Error %s</h1>
  <p>%s</p>
  <p><a href="/">Return to home</a></p>
</section>
```

### 9.2 Module: `src/modules/error/error.c/h`

```c
// Renders error_epoch<N>.html with (status_code, message) and wraps it via
// buildPageWebSite(epoch, "Boat Rudder - Error <code>", ...).
// Returns a malloc'd full response body (HTML/WML), or NULL on error
// (callers fall back to a hardcoded minimal string in that case).
char *render_error_page(int epoch, int status_code, const char *message);
```

A small static lookup table maps common codes to default messages (`400` -> "Bad Request",
`401` -> "Unauthorized", `403` -> "Forbidden", `404` -> "Not Found",
`405` -> "Method Not Allowed", `431` -> "Request Header Fields Too Large",
`500` -> "Internal Server Error"), so call sites can pass just the code, or override the
message (e.g. the login failure case could reuse this for a `401` if it were a separate page -
though §5.5 keeps login failures as `200` + re-rendered form, which is the more common UX
pattern for login forms).

### 9.3 New router-level helper: `send_error_response()`

`http_router.c` gains a small static helper that replaces `send_simple()`:

```c
static void send_error_response(void *ctx, int status_code, const char *status_line, int epoch) {
    char *page = render_error_page(epoch, status_code, NULL /* default message */);
    char *response = page
        ? build_epoch_response_status(page, "", epoch, status_line)
        : NULL;
    if (!response) {
        // last-resort fallback: today's hardcoded send_simple() body, for
        // when even template loading fails (disk error, etc.)
        send_simple(ctx, status_line, "<html><body><h1>Error</h1></body></html>");
        free(page);
        return;
    }
    connection_write(ctx, response, strlen(response));
    free(page);
    free(response);
}
```

`send_simple()` is **kept**, unchanged, purely as the last-resort fallback described above -
it is not removed, just demoted from "the error path" to "the error path's error path".

### 9.4 Refactoring Existing `send_simple()` Call Sites

Every current `send_simple()` call in `http_router.c` is updated to call
`send_error_response()` instead, using `detect_epoch()`/`force_epoch` where a `User-Agent` is
available, or `EPOCH_PRESTANDARD` (the safest universally-renderable epoch) where it isn't yet
(e.g. the `400 Bad Request` for a malformed request line, before headers can be trusted):

| Call site | Status | Epoch source |
|---|---|---|
| Headers > `RAW_REQUEST_SIZE` | `431 Request Header Fields Too Large` | `EPOCH_PRESTANDARD` (request unparsed) |
| `parse_http_request()` fails | `400 Bad Request` | `EPOCH_PRESTANDARD` (request unparsed) |
| Malformed request line | `400 Bad Request` | `EPOCH_PRESTANDARD` (request unparsed) |
| `buildHomeWebSite()` returns `NULL` | `500 Internal Server Error` | resolved epoch |
| `build_epoch_response()` returns `NULL` | `500 Internal Server Error` | resolved epoch |
| Unknown method | `405 Method Not Allowed` | resolved epoch |
| New: unauthenticated `/dashboard`, etc. | `302`/`401` | resolved epoch |

This directly satisfies "me gustaría que estas fueran plantillas centralizadas y estandar con
sus respectivas épocas" - every error response a client can receive is now templated per
epoch, with a single hardcoded fallback only for the pathological case where template loading
itself fails.

---

## 10. `CMakeLists.txt` Changes

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
```

New `SOURCES` entries:

```cmake
# Database / auth / sessions
src/db/mongodb_manager.c
src/db/auth.c
src/db/session_manager.c

# New CMS pages
src/modules/login/login.c
src/modules/dashboard/dashboard.c
src/modules/error/error.c
```

New link libraries:

```cmake
target_link_libraries(base-http-server
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MONGOC
    PkgConfig::SODIUM
)
```

`src/html_builder/orchestrator.c`/`.h` are modified (not added) to expose `buildPageWebSite()`
alongside `buildHomeWebSite()`; `src/utils/build_epoch_response.c`/`.h` are modified to add
`build_epoch_response_status()` and `build_redirect_response()`.

---

## 11. Configuration (`configs/settings.conf`) Additions

```ini
# MongoDB connection (login/sessions). If mongodb_connect fails at startup,
# /login and /dashboard serve a 503 error page (see error_epoch<N>.html);
# the rest of the site (epoch CMS + static files) is unaffected.
mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder

# Session cookie lifetime, in seconds (default: 24h).
session_ttl_seconds=86400
```

`src/utils/config_loader.c/h` gains two new globals (`char mongodb_uri[256]`,
`char mongodb_db[64]`, `int session_ttl_seconds`), parsed the same way as the existing
`public_url`/`force_epoch` keys.

---

## 12. New File Inventory

```
src/db/
├── mongodb_manager.c/h
├── auth.c/h
└── session_manager.c/h

src/modules/login/login.c/h
src/modules/dashboard/dashboard.c/h
src/modules/error/error.c/h

html/themes/dark/
├── page/
│   └── page_epoch{-1,0,1,2,3}.html
├── login/
│   └── login_epoch{-1,0,1,2,3}.html
├── dashboard/
│   └── dashboard_epoch{-1,0,1,2,3}.html
└── error/
    └── error_epoch{-1,0,1,2,3}.html
```

Modified files:

```
CMakeLists.txt                          (deps + new sources)
configs/settings.conf                   (mongodb_uri, mongodb_db, session_ttl_seconds)
src/main.c                              (mongodb_manager_init/cleanup)
src/utils/config_loader.c/h             (new config keys)
src/utils/build_epoch_response.c/h      (status-line + redirect helpers)
src/html_builder/orchestrator.c/h       (buildPageWebSite)
src/web_server/http_router.c            (/login, /dashboard, /logout routes;
                                          send_error_response() refactor)
html/themes/dark/styles_epoch2.css      (.br-page-content rule)
html/themes/dark/styles_epoch3.css      (.boat-rudder__page-content,
                                          .boat-rudder__login__* rules)
develop_docs/architecture.md            (new src/db/ layer, new modules,
                                          page/login/dashboard/error templates)
develop_docs/boat-rudder.md             (mention /login, /dashboard routes)
develop_docs/diagrams/*.puml            (cms-components, sequence diagrams)
```

---

## 13. Security Considerations

- **Argon2id** (libsodium default for `crypto_pwhash_str`) for password hashing - no MD5/SHA1/
  plain-text storage.
- **`HttpOnly`, `Secure` (when TLS enabled), `SameSite=Lax`** session cookies - mitigates XSS
  cookie theft and basic CSRF on the login form (the login form itself is a simple POST with
  no state-changing side effects beyond authentication, so CSRF risk is low; a CSRF token can
  be added later if a "change password" or similar feature is introduced).
- **Constant-time comparison**: `crypto_pwhash_str_verify()` is constant-time by design, and
  `auth_login_user()` returns the same generic "Invalid email or password" message whether the
  email doesn't exist or the password is wrong - no user enumeration via error messages.
- **Epoch-3-only enforcement happens server-side**, on every request (`GET` and `POST`), not
  just by hiding the form in older-epoch templates - a crafted `POST /login` claiming an old
  `User-Agent` (or with `force_epoch` set to a non-3 value) is rejected before any DB call.
- **Session tokens**: 32 random bytes (256 bits) from `randombytes_buf()` (libsodium's CSPRNG),
  hex-encoded - not guessable, not derived from user data.
- **MongoDB connection**: `mongodb_uri` should point at a `localhost`-only or otherwise
  network-isolated MongoDB instance; this plan does not add MongoDB authentication
  configuration (out of scope), but production deployments should enable it
  (`mongodb://user:pass@host/...` in `mongodb_uri`).
- All new responses continue to include `SECURITY_HEADERS`
  (`X-Content-Type-Options: nosniff`, `X-Frame-Options: SAMEORIGIN`) via
  `build_epoch_response_status()`/`build_redirect_response()`.
- **`MAX_BODY_SIZE`/`RAW_REQUEST_SIZE`** limits already in `http_constants.h` bound the size of
  the `POST /login` body; no changes needed since login form bodies are tiny.

---

## 14. Documentation Updates

After implementation, update:

- **`architecture.md`**: add `src/db/` to the directory tree and component descriptions (§
  "Component Descriptions"); add `login`, `dashboard`, `error` to the modules table; add
  `page_epoch<N>.html` to the template-family list in §2.3; document
  `build_epoch_response_status()`/`build_redirect_response()` in §2.6.
- **`boat-rudder.md`**: mention `/login`, `/dashboard`, `/logout` routes in §3 (Web Server
  routing bullet list) and the epoch-3-only restriction in §2 (epoch table or a new callout).
- **`diagrams/cms-components.puml`**: add `login`/`dashboard`/`error` modules and the `db`
  package (`mongodb_manager`, `auth`, `session_manager`).
- **`diagrams/sequence-home-route.puml`**: optionally add a sibling
  `sequence-login-route.puml` showing the `POST /login` -> `auth_login_user()` ->
  `create_session()` -> `302 /dashboard` flow.
- **`data-flow.md`**: add the login/session data flow alongside the existing `/` flow.

---

## 15. Suggested Implementation Order

1. `src/db/mongodb_manager.c/h` + CMake/config plumbing + `main.c` init/cleanup. Verify with a
   minimal "ping" log line at startup (no routes depend on it yet).
2. `build_epoch_response_status()` + `build_redirect_response()` in
   `src/utils/build_epoch_response.c` (additive, low risk, unblocks everything else).
3. `page_epoch<N>.html` templates + `buildPageWebSite()` in the orchestrator. Verify by
   temporarily wiring a trivial static page (can be removed/replaced once `/dashboard` exists).
4. `error_epoch<N>.html` templates + `src/modules/error/error.c` + `send_error_response()`,
   refactor existing `send_simple()` call sites. Verify all existing error paths (`400`,
   `405`, `431`, `500`) still work, now epoch-aware.
5. `src/db/auth.c` + `src/db/session_manager.c`. Seed one test user document directly in
   MongoDB (`mongosh`) for manual testing.
6. `dashboard_epoch<N>.html` + `src/modules/dashboard/dashboard.c` + `GET /dashboard` route
   (initially always redirecting to `/login`, since no session exists yet).
7. `login_epoch<N>.html` (all 5 epochs) + `src/modules/login/login.c` + `GET`/`POST /login`
   routes, wiring everything from steps 1-6 together.
8. `GET /logout`.
9. Documentation updates (§14).

---

## 16. Verification / Testing Plan

- `cmake -B build_test -DCMAKE_BUILD_TYPE=Debug && cmake --build build_test` - clean build,
  no new warnings under `-Wall -Wextra -Wpedantic`, AddressSanitizer enabled.
- Manual MongoDB setup: run `mongod` locally, create `boat_rudder` DB, insert one user with an
  Argon2id hash generated via a small `libsodium` snippet or `mongosh` + a one-off helper.
- Per-epoch checks via `force_epoch` in `configs/settings.conf` (restarting between values, as
  documented in `boat-rudder.md`):
  - `force_epoch=3`: `GET /login` shows the form; `POST /login` with wrong credentials
    re-shows the form with an error and `200`; `POST /login` with correct credentials returns
    `302 /dashboard` + `Set-Cookie`; `GET /dashboard` with that cookie shows "Welcome to
    dashboard"; `GET /logout` clears the cookie and `GET /dashboard` afterwards redirects to
    `/login`.
  - `force_epoch in {-1, 0, 1, 2}`: `GET /login` and `POST /login` (with any body) both render
    the "not available" message in that epoch's markup, with **no** MongoDB query performed
    (verifiable via `verbose_level=4` debug logs - no `auth_login_user`/session log lines).
  - All epochs: trigger `404` (unknown route), `405` (e.g. `DELETE /`), and `400`/`431`
    (malformed/oversized request via `curl`/raw socket) and confirm the epoch-appropriate
    `error_epoch<N>.html` renders with correct `Content-Type`.
- Confirm `/dashboard` directly (without a session cookie) redirects to `/login` regardless of
  epoch.
- `curl -I` checks for `X-Content-Type-Options`/`X-Frame-Options` on all new response types
  (login, dashboard, error, redirect).
