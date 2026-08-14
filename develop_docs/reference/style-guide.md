# C Style Guide

This guide adapts the parts of the **Google C++ Style Guide** that translate
cleanly to C (naming, formatting, file/include organization, comments) and
combines them with the **SEI CERT C Coding Standard** rules that matter most
for a server that parses raw bytes from the network. It documents the
conventions already used in `src/` so new code stays consistent, and
highlights the security rules that are non-negotiable for this project.

Where the two sources disagree (e.g. Google avoids `goto`, CERT recommends it
for centralized cleanup), the CERT/C-idiomatic choice wins - this is C, not
C++, and `goto cleanup` is the standard way to avoid resource leaks across
many error paths.

---

## 1. File Organization

### Header guards

```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H
...
#endif // MODULE_NAME_H
```

Macro name = path-derived, all caps, `_H` suffix. Always close with a
trailing `// MODULE_NAME_H` comment so a long file's `#endif` is traceable.

### Include order

Group includes top to bottom, separated by a blank line, each group sorted
alphabetically:

1. The file's own header (`.c` files only)
2. Other project headers, relative paths (`"../connection.h"`)
3. C standard library (`<stdio.h>`, `<string.h>`, ...)
4. POSIX / system headers (`<sys/stat.h>`, `<unistd.h>`, ...)
5. Third-party (`<openssl/ssl.h>`)

```c
#include "static_file_server.h"
#include "../connection.h"
#include "../../utils/http_utils.h"
#include "../../utils/log.h"

#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <openssl/ssl.h>
```

### One module = one `.c`/`.h` pair

Each module owns a name prefix that acts as an informal namespace:
`tls_*` in `tls_context.c`, `url_*` in `url_parser.c`, `server_*` in
`server_listener.c`. New public functions in a module should start with
that prefix. Internal helpers are `static` and do not need the prefix.

---

## 2. Naming Conventions

| Element | Convention | Example |
|---|---|---|
| Functions, variables, file-scope statics | `snake_case` | `serve_static_file`, `ip_table` |
| Macros, compile-time constants | `UPPER_SNAKE_CASE` | `MAX_PARAMS`, `RATE_WINDOW` |
| Struct typedefs (new code) | `snake_case_t` | `connection_ctx_t`, `ip_entry_t` |
| Function-like macros | `UPPER_SNAKE_CASE`, parenthesize args/result | `SECURITY_HEADERS` |

`HttpRequest`, `HttpHeader` and `QueryParam` use `PascalCase` from before
this guide existed. Don't change them gratuitously (churn for no benefit),
but don't introduce new `PascalCase` types - use `snake_case_t`.

Boolean-returning functions read as predicates: `too_many_connections()`,
`is_trusted_proxy()`. Functions that allocate and return ownership to the
caller should make that obvious from the name or a one-line comment (see
§6).

---

## 3. Formatting

- **Indentation:** 4 spaces, no tabs.
- **Braces:** K&R style - opening brace on the same line as the
  function/control statement.
  ```c
  void http_route(read_func_t read_func, void *ctx, const char *root_directory) {
      if (!ctx) {
          LOG_ERROR("http_route: ctx is NULL");
          return;
      }
  }
  ```
- **Line length:** soft limit ~100 columns. Break long parameter lists by
  aligning continuation lines under the first argument.
- **Pointers:** `*` binds to the variable name, not the type:
  `char *raw_request`, not `char* raw_request`.
- **Early returns / single-statement guards** may stay on one line:
  `if (bytes_read == 0) goto cleanup;`
- **One declaration per line**, declared close to first use (not all at the
  top of the function, C89-style).

---

## 4. Comments

- Use `//` for all normal comments; reserve `/* */` for nothing in
  particular (keep it consistent - this codebase already uses `//`
  exclusively).
- Document the **contract** in the header, not the implementation: what the
  function expects, who owns/frees what it returns, and what the return
  codes mean.
  ```c
  // Serve a static file or directory.
  // if_modified_since: value of the client's If-Modified-Since header (may be NULL).
  // Responds 304 Not Modified when the file has not changed since that date.
  void serve_static_file(void *ctx, const char *root_directory,
                         const char *decoded_url, const char *if_modified_since);
  ```
- Comment *why*, not *what* - if a line needs a comment to explain what it
  does, prefer renaming variables/functions until it doesn't. Reserve
  comments for non-obvious constraints (platform quirks, RFC references,
  security rationale), e.g. the `timegm()`/`_GNU_SOURCE` portability note in
  `static_file_server.c`.

---

## 5. Functions & Control Flow

- Keep functions focused on one responsibility. Extract `static` helpers
  (`send_error`, `format_http_date`) for repeated or logically distinct
  steps - `static` linkage signals "module-private" and lets the compiler
  warn about unused helpers.
- **Centralized cleanup with `goto`:** when a function has multiple resources
  to release (buffers, file descriptors, SSL handles) and multiple error
  exits, use a single `cleanup:` label at the end. This is the established
  pattern in `http_route()` and is preferred over duplicating cleanup code
  at every `return`.
  ```c
  char *raw_request = malloc(RAW_REQUEST_SIZE);
  if (!raw_request) { LOG_ERROR("malloc failed"); goto cleanup; }
  ...
  cleanup:
      free(raw_request);
      connection_close(ctx);
  ```
- **Avoid global mutable state.** The two exceptions in this codebase
  (`ip_table`, `active_connections` in `server_listener.c`) are
  intentionally `static` to the file, guarded by a mutex, and documented -
  follow that template if a new global is unavoidable. Don't add new
  globals without the same treatment.

---

## 6. Memory Management (CERT MEM)

- **Check every `malloc`/`calloc`/`realloc` return value** before use
  (`MEM32-C`). On failure, log and take the cleanup path - never dereference
  a possibly-NULL allocation.
- **Free exactly once** (`MEM30-C`/`MEM31-C`). If a pointer might be freed on
  more than one cleanup path, set it to `NULL` immediately after `free()` so
  a later `free(NULL)` is a safe no-op.
- **State ownership in the header comment.** When a function returns
  `malloc`'d memory (e.g. `buildHomeWebSite()`, `read_file_to_string()`),
  say so explicitly and say the caller must `free()` it. This is the #1
  source of leaks/double-frees in C and the comment is cheap insurance.
- **Match allocator/deallocator pairs** (`MEM12-C`): everything from
  `malloc`/`calloc`/`strdup` is freed with `free`; don't mix with
  `OPENSSL_malloc`/`OPENSSL_free` etc.

---

## 7. Strings & Buffers (CERT STR)

This is the highest-value section for an HTTP server: almost every string in
the request path originates from the client.

- **Never use `strcpy`, `strcat`, `sprintf`, or `gets`.** Use `snprintf`,
  `strncpy` (with explicit termination), or `memcpy` with a checked length.
- **`STR31-C` - size buffers correctly**, including the null terminator.
  When copying into a fixed-size buffer (`char route[2048]`), always pass
  `sizeof(buffer)` (or `sizeof(buffer) - 1` if you'll append the
  terminator yourself) - never a hardcoded length that can drift from the
  declaration.
- **`STR32-C` - guarantee null-termination.** `strncpy` does *not*
  null-terminate if the source is >= the destination size. After any
  `strncpy`, explicitly set `buf[sizeof(buf) - 1] = '\0'`, as
  `http_router.c` does for `client_ip` and `url_copy`.
- **Treat all network-derived strings as untrusted**: request line, headers
  (`User-Agent`, `Cookie`, `X-Forwarded-For`, `If-Modified-Since`), URL,
  decoded URL, query params, and body. Validate length *before* copying, not
  after.

---

## 8. Integers & Arithmetic (CERT INT)

- **Use `size_t` for sizes, lengths, and array indices** - never `int`.
  `-Wextra` (already enabled) flags signed/unsigned comparison mismatches;
  don't silence these with casts unless you've proven the value can't be
  negative.
- **`INT30-C`/`INT32-C` - check for overflow before arithmetic** that
  computes buffer sizes or offsets, e.g. `base_len + sizeof("/index.html")`
  in `static_file_server.c` is checked against `sizeof(safe_path)` *before*
  the `memcpy`. Apply the same pattern anywhere a length from the client
  (`Content-Length`, query param count) feeds into a size calculation.
- **Validate client-supplied sizes against the configured maxima**
  (`MAX_BODY_SIZE`, `MAX_PARAMS`, `MAX_PARAM_LENGTH` in `http_constants.h`)
  *before* allocating or looping, and reject with the appropriate HTTP error
  (`413`, `431`) rather than truncating silently.

---

## 9. Arrays, Pointers & Bounds (CERT ARR / EXP)

- **`ARR30-C`/`ARR38-C` - never index outside array bounds.** Any path built
  from `root_directory + decoded_url` must be checked to still resolve
  *inside* `root_directory` (no `../` traversal) before `open()` - this is
  already done in `static_file_server.c` and must be preserved in any new
  code that touches the filesystem from a URL.
- **`EXP34-C` - never dereference a possibly-NULL pointer.** Check
  `get_header_value()`, `malloc()`, `fopen()`, `getenv()` results before use.
  The codebase's `real_ip && real_ip[0]` pattern (check non-NULL *and*
  non-empty before using a header value) is the right template.

---

## 10. Error Handling (CERT ERR)

- **`ERR33-C` - check return values** of `read`/`write`/`malloc`/`pthread_*`
  /`SSL_*`/`fopen` etc. A function that can fail and whose failure is
  ignored is a latent bug.
- **Consistent return-code convention:** `0` = success, `-1` = error (as in
  `url_parse`, `tls_create_context`, `server_start`). Don't invent new
  conventions (e.g. `1` = success) in new modules.
- **Log before failing.** Use `LOG_ERROR`/`LOG_WARN` with enough context
  (function name, the value that failed) so a production log line is
  actionable without a debugger - see the `"http_route: malloc failed"`
  style.

---

## 11. Concurrency (CERT POS / CON)

- **Document shared state.** Any data structure touched from more than one
  `pthread_create`d thread (currently `ip_table` / `active_connections`)
  must be guarded by a mutex and have a comment stating *what* it protects.
- **Keep critical sections small** - do the minimum work under the lock,
  then release it before I/O or logging.
- **Detach or join explicitly.** Connection threads are `pthread_detach`'d
  immediately after creation; if a future thread needs a result back, prefer
  `pthread_join` and document the lifetime instead of leaking a detached
  thread that outlives its data.

---

## 12. Resource Management / I/O (CERT FIO)

- **`FIO42-C` - close every file descriptor / `SSL*` / socket on every exit
  path**, including error paths. This is what the `cleanup:` label pattern
  (§5) exists for.
- **Set socket timeouts** (`SO_RCVTIMEO`/`SO_SNDTIMEO`, currently 30 s - raised from 5 s so large uploads can complete) on every
  new connection so a slow/hostile client can't hold a thread forever.
- **TLS context lifetime:** `tls_create_context()` /
  `tls_free_context()` must remain a matched pair with a single owner
  (`server_listener.c`); don't call `SSL_CTX_free` directly elsewhere.

---

## 13. Security: Untrusted Input Checklist

Everything below originates from the client and must be validated before
use. When adding a feature that touches any of these, re-read §7-§9:

- HTTP method, URL, protocol version (request line)
- Headers: `User-Agent`, `Cookie`, `Content-Type`, `Content-Length`,
  `X-Real-IP`, `X-Forwarded-For`, `If-Modified-Since`
- Request body (bounded by `MAX_BODY_SIZE`)
- Decoded URL / route used to build filesystem paths
- Query parameters (`QueryParam[]`, bounded by `MAX_PARAMS` /
  `MAX_PARAM_LENGTH`)

`X-Real-IP`/`X-Forwarded-For` are additionally only trusted when the peer is
in `trusted_proxies` - don't add new "trust this header" logic without the
same proxy check.

---

## 14. Tooling

- Build with `-Wall -Wextra -Wpedantic` (already on for all builds) - fix
  warnings rather than silencing them.
- `compiledebug` builds with **AddressSanitizer**; run `rundebug` regularly
  during development, especially after touching memory management or
  parsing code - ASan catches most CERT MEM/STR violations at runtime.
- For periodic audits, `cppcheck --enable=all` or `clang-tidy` with the
  `cert-*`/`bugprone-*`/`clang-analyzer-*` checks complement ASan by finding
  issues on paths that aren't exercised by a given test run.

---

## Quick Checklist (for review)

- [ ] Header guard matches filename, includes grouped/ordered correctly
- [ ] Names follow `snake_case` / `UPPER_SNAKE_CASE` / `snake_case_t`
- [ ] 4-space indent, K&R braces, `*` binds to variable
- [ ] Every `malloc` checked; ownership of returned pointers documented
- [ ] No `strcpy`/`strcat`/`sprintf`/`gets`; buffers sized with `sizeof`,
      explicitly null-terminated after `strncpy`
- [ ] Sizes/lengths are `size_t`; arithmetic on client-supplied lengths is
      overflow-checked and validated against `http_constants.h` maxima
- [ ] Filesystem paths derived from URLs stay within `root_directory`
- [ ] All error paths go through `cleanup:` and release every resource
- [ ] New shared state is mutex-protected and documented
- [ ] Builds clean with `-Wall -Wextra -Wpedantic`; tested under
      `compiledebug`/ASan

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
