# ADR: Extensions

> **Status**: phase 1 implemented (2026-10-28): discovery, enable/disable, supervision,
> forwarding, the C SDK. Later phases proposed.
> Reference for extension authors: [extensions.md](../reference/extensions.md) and
> [creating-an-extension.md](../reference/creating-an-extension.md).

---

## Context

A site sometimes needs an application of its own that has nothing to do with publishing - a
catalogue its users keep, a control panel for something outside the server - while still using
the site's users, sign-in and look, and still being reachable from the computers the site is
built for.

Constraints:

- Boat Rudder stays **agnostic**: it detects an extension, lets an admin enable or disable it,
  and documents how to build one. What an extension does is not its business.
- Extensions are written in **C**, in **their own repositories**, and are installable and
  removable; Boat Rudder must update without breaking them, and the reverse.
- Signed-in use is **epoch 3 only** at first; epochs −1…2 will get public, read-only views later.
- Users are **created by an admin**, as for the rest of the site.

---

## Decisions

| # | Decision | Alternatives considered | Why |
|---|---|---|---|
| 1 | **Each extension is its own process**; Boat Rudder forwards requests to it over a **Unix socket** | Compile extensions into the binary; load them as `.so` with `dlopen` | Isolation: a crash in an extension can't take the site down (core answers 503). No C ABI to freeze: the boundary is a wire protocol, so either side can be rebuilt or updated alone. An extension may need long-lived connections (devices, other services) and threads of its own, which don't fit a thread-per-request handler |
| 2 | **Wire protocol: HTTP/1.1 over the socket**, versioned (`protocol=` in the manifest, `X-BR-Protocol`) | A binary protocol; FastCGI | Debuggable with `curl --unix-socket`; parsing exists on both sides; a version lets the core refuse what it can't speak instead of misbehaving |
| 3 | **C SDK (`sdk/brext.{h,c}`)** in the boat-rudder repo, **compiled into** each extension | Each extension hand-rolls the server; a shared library | One tested implementation of the protocol. Static: a core update never changes code inside an installed extension |
| 4 | **The core keeps identity, sessions, CSRF, roles, epoch, theme and layout**; extensions get the verified result as `X-BR-*` headers | Extensions read the session cookie | One security implementation. Only Boat Rudder can open the socket (`0600`), and it forwards no browser header, so the headers can't be forged |
| 5 | **Fixed mount `/dashboard/ext/<name>/`** | A `mount=` in the manifest | Inherits every dashboard guard (epoch 3, session, CSRF, viewer read-only) and the dashboard stylesheet with no new code; no collisions with core routes to validate |
| 6 | **Boat Rudder supervises the processes**: starts the enabled ones, restarts with backoff, gives up after 5 exits in 60 s, stops them on disable/shutdown | systemd unit per extension | "Enable" in the dashboard must mean *it runs*, with no root step; works the same on a single site and under multi-br. main.c's SIGCHLD handler reaps every child, so liveness is `kill(pid, 0)`, and `PR_SET_PDEATHSIG` ties the extension's life to the server's |
| 7 | **Discovered from `extensions/<name>/extension.conf`, disabled until an admin enables it**; the choice in the `extensions` collection | Enabled on install; a list in `settings.conf` | Installing never runs code by itself; the admin decides from the dashboard; survives restarts |
| 8 | **Data in the site's database**, collections `ext_<name>_*` | A database per extension | Site backups include extension data; no collision with core collections |
| 9 | **HTML fragments wrapped in the layout; JSON passed through; redirects only inside the mount** | Full pages from the extension; any redirect | Menu, theme, CSRF script and analytics identical to the rest of the site; an extension can't send the browser off-site |

---

## Compatibility rules

- The core never compiles, links or reads extension code - only the manifest and the socket.
- A new protocol version is a new `protocol=` value; the core lists an extension whose protocol
  it doesn't speak as unusable instead of running it.
- `boat-rudder-update` only deletes paths the installed release's signed package contained, and
  no release ships anything under `extensions/` - which must stay true. It never touches the
  database, so `ext_*` collections are safe.
- `extensions/` and `run/` are git-ignored in the boat-rudder repo.

---

## Phases

| # | Delivers | State |
|---|---|---|
| 1 | Discovery, admin page, supervision, forwarding, dashboard menu entries, SDK, reference doc and guide | done |
| 2 | Public pages and retro epochs: an extension answers **content blocks as JSON** (`application/vnd.boat-rudder.blocks+json`) and the core renders them for every epoch with its existing block renderers (title, paragraph, list, table, image, gallery, link), WML included; a `public=` list in the manifest | proposed |
| 3 | Static assets under `<mount>/assets/` served from the extension's directory | proposed |
| 4 | `member` role: signs in, sees extensions and their account, never the CMS | proposed |
| 5 | Packaging: `boat-rudder-ext install/update/remove [--purge]`, signed with minisign; multi-br per-site extensions | proposed |
| 6 | Optional sessions on epochs 1-2 (`Expires` cookies, HTML 3.2 forms), so a signed-in extension can be used from a vintage computer | proposed |

---

## Open questions

1. **Extensions that reach outside the server** (devices on another network, external services):
   the protocol assumes core and extension on the same machine; anything further is the
   extension's own concern (a VPN, an API client).
2. **Media for extensions**: an upload API onto the core media library, or each extension's own
   storage.
