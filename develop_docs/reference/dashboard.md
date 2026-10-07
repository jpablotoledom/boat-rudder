# Boat Rudder - Authentication and Dashboard

Everything behind `/login` and `/dashboard`: the session layer, the two roles, and each
maintainer (Entries, Categories, Languages, Menu, Users, Media, Settings, Analytics). The public
rendering side is in [rendering.md](rendering.md); the server foundation in
[architecture.md](architecture.md). The complete route list with guards is
[routes.md](routes.md); the security model (CSRF, ownership, known gaps) is
[security.md](security.md).

The largest features have their own documents and are only summarised here:
[entry-editor.md](entry-editor.md) for `/dashboard/entries/<id>/edit`,
[media-admin.md](media-admin.md) for `/dashboard/media`, [themes.md](themes.md) and
[fonts.md](fonts.md) for `/dashboard/settings*`, and [analytics.md](analytics.md) for
`/dashboard/analytics`.

---

## Login, Dashboard and Logout

A small authentication slice sits alongside the CMS, sharing the same epoch/template
infrastructure via a new generic page shell.

### `html_builder/orchestrator.c`: `buildPageWebSite()` / `buildPageWebSiteAtUrl()`

```c
char *buildPageWebSite(int epoch, const char *page_title, char *html_content);
char *buildPageWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                             const char *current_url);
char *buildBlogListWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                                 const char *current_url, char *category_menu_html);
char *buildEntryWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                              const char *current_url, char *category_menu_html);
```

All four wrap an arbitrary content fragment in a page shell (head + menu + footer), resolving `{{PAGE_TITLE}}` to `<title>page_title</title>`, and take ownership of `html_content`:

| Function | Shell template | Used by |
|---|---|---|
| `buildPageWebSite()` | `page/page_epoch<N>.html` | login, dashboard, error pages (always passes `"/"` as the active menu URL) |
| `buildPageWebSiteAtUrl()` | `page/page_epoch<N>.html` | same, but highlights the nav item matching `current_url` |
| `buildBlogListWebSiteAtUrl()` | `page/page-blog_epoch3.html` (epoch 3), else `page_epoch<N>.html` | `/blog`, `/blog/category/<slug>` - full-width listing, no page-content width constraint |
| `buildEntryWebSiteAtUrl()` | `page/page-entry_epoch{2,3}.html`, else `page_epoch<N>.html` | `/blog/<link>`, `/page/<link>` - article wrapper |

The last two also take ownership of `category_menu_html` (may be `NULL`) and insert it right after the navbar. `/` keeps using `buildHomeWebSite()`, which always passes `"/"`.

`page_epoch3.html` also includes the gallery lightbox overlay (`#galleryLightbox`) and its inline JS (`openGalleryLightbox`, `closeGalleryLightbox`, `galleryLightboxNav`) - present on every page but invisible until a gallery image is clicked.

### `src/db/mongodb_manager.c`
- `mongodb_manager_init(uri, db_name)`: calls `sodium_init()` (required once, before any
  libsodium call) then `mongoc_init()` and creates a `mongoc_client_pool_t` from `mongodb_uri`/
  `mongodb_db` (`configs/settings.conf`). Called once from `main()`, after `load_config()` and
  before `server_start()`. Returns `0`/`-1`; on failure the server keeps running but `/login`
  and `/dashboard` degrade to a `503` error page.
- `mongodb_manager_is_ready()`: `1` if init succeeded.
- `mongodb_manager_get_client()` / `mongodb_manager_get_collection(name)`: per-thread client
  from the pool (mongoc clients are not thread-safe), and a convenience collection handle
  against the configured database.
- `mongodb_manager_cleanup()`: destroys the pool and calls `mongoc_cleanup()`. Called once from
  `main()` during shutdown.

### `src/db/auth.c`
- `auth_login_user(email, password)`: looks up `email` in the `users` collection and verifies
  `password` against its `crypto_pwhash_str()` (Argon2id) hash via libsodium. Returns a
  malloc'd 24-char ObjectId hex string on success, or `NULL` for unknown email, wrong password,
  *or* a DB error - all three are indistinguishable to the caller, to avoid user enumeration.

### `src/db/session_manager.c`
- `generate_session_token()`: 32 random bytes (libsodium CSPRNG), hex-encoded (64 chars).
- `create_session(user_id_hex, token, ttl_seconds)`: inserts `{user_id, token, created_at,
  expires_at}` into the `sessions` collection.
- `validate_session_cookie(cookie_header, user_id_out)` (= `extract_session_token()` +
  `validate_session()`): `1` if the `session` cookie names a non-expired session, `0` if
  missing/invalid/expired, `-1` on DB error. `/dashboard` treats `0` and `-1` the same way
  (redirect to `/login`); a `mongodb_manager_is_ready()` check before this handles the
  "DB never connected" case separately with a `503`.
- `destroy_session(token)`: deletes the session document (`/logout`).
- `derive_csrf_token(session_token, out)` / `verify_csrf_token(cookie_header, supplied)`: the
  dashboard's CSRF token - keyed BLAKE2b of the session token, compared in constant time.
  `require_dashboard_session()` calls `verify_csrf_token()` on every `POST`; the token reaches
  the page through `request_csrf_token()` and `page_layout_wrap()` (meta tag +
  `/assets/js/csrf.js`). See [security.md](security.md#csrf-protection).
- `session_manager_ensure_indexes()`: run once at startup; unique index on `token`, TTL index on
  `expires_at` (expired sessions purged by MongoDB), and an immediate purge of expired sessions.
- `build_session_cookie_header(token, ttl_seconds, ...)` /
  `build_session_clear_cookie_header(...)`: build `Set-Cookie: session=<token>; HttpOnly;
  Path=/; Max-Age=<n>; SameSite=Lax` (+ `; Secure` when `ssl_enabled=1`), or the same with
  `Max-Age=0` to clear it.

### `src/modules/login/login.c`
```c
char *login(int epoch, const char *error_message);
```
Loads `login_epoch<N>.html`. For `EPOCH_MODERN`, fills the single `%s` with an
`error_message` block (or empty string if `NULL`/`""`). For all other epochs, the template has
no placeholders and is returned verbatim - login is restricted to `EPOCH_MODERN` (see below),
so the other epochs only ever show "this functionality is not available".

### `src/modules/dashboard/dashboard.c`
```c
char *dashboard(int epoch, const char *lang, const char *user_id, const char *role);
```
For `EPOCH_MODERN`, loads `dashboard_epoch<N>.html` and fills its placeholders in page order -
the options menu, the analytics summary (admin only, `""` for an Autor) and the pending-entries
rows - based on `role` (see "Roles and privileges" below). Other epochs return the static
`dashboard_epoch<N>.html` fragment unchanged - `lang`/`user_id`/`role` are ignored.

### `src/modules/error/error.c`
```c
char *error_content(int epoch, int status_code, const char *message);
```
Loads `error_epoch<N>.html` and fills its 2 `%s` placeholders (status code, message). If
`message` is `NULL`, a default message for `status_code` is used (a static table covering
`400`/`403`/`404`/`405`/`431`/`500`/`503`); unrecognized codes (e.g. `413`) fall back to `"Error"`.

### Routes

| Route | Method | Behavior |
|---|---|---|
| `/login` | `GET` | If `mongodb_manager_is_ready()` and the request carries a valid session cookie, `302 /dashboard`. Otherwise renders `login_epoch<N>.html` via `buildPageWebSite()`. For `EPOCH_MODERN`, a real form; other epochs show "not available". |
| `/login` | `POST` | **`EPOCH_MODERN` only.** Other epochs re-render the "not available" page without any DB access. If `mongodb_manager_is_ready()` is false, `503`. Otherwise `auth_login_user()`; on success, `generate_session_token()` + `create_session()` + `Set-Cookie` + `302 /dashboard`; on failure, re-renders `/login` (`200`) with "Invalid email or password." |
| `/dashboard` | `GET` | If `!mongodb_manager_is_ready()` → `503`. Else `validate_session_cookie()`: valid → looks up the user's role (`cms_get_user_role()`, defaulting to `"admin"` on error) and calls `dashboard(epoch, content_lang, user_id, role)` via `buildPageWebSite()`; otherwise → `302 /login`. For `EPOCH_MODERN` the page opens with "Welcome back" and, on its right, a **Menu** dropdown and a **Log out** button, then has two parts (three for an Administrador). (1) **Option groups**, inside the Menu dropdown (a `<details>`, so it works without JS; a small inline script in `dashboard_epoch3.html` closes it on an outside click or Escape) - an Administrador gets `dashboard/nav-admin_epoch3.html`: *Content* (Entries, Categories, Media library, Menu), *Site* (General, Themes, Fonts, Languages, Epoch preview), *Administration* (Users, Analytics report); an Autor gets `nav-author_epoch3.html`: *Content* (Entries, Media library). (2) For an Administrador only, the **analytics summary** (`analytics_summary()`, see [analytics.md](analytics.md#dashboard-summary)). (3) **Entries pending publication** - unpublished entries only (`enabled != true`), with **+ New entry** and **View all** (`/dashboard/entries`) next to the heading: every type for an Administrador (`entries_admin_rows(epoch, lang, NULL, NULL, true, true)`), only their own `type:"blog"` entries for an Autor (`entries_admin_rows(epoch, lang, "blog", user_id, true, false)` - no Delete button). Rows: see [Entries listing](#entries-listing-srcmodulesentries_admin-embedded-in-dashboard) below. Other epochs render the static "Welcome to dashboard" fragment unchanged (role is ignored). |
| `/dashboard/categories` | `GET` | **`EPOCH_MODERN` only** (other epochs `302 /dashboard`). `require_admin_session()` - Administrador only, an Autor session gets `302 /dashboard`. Renders `categories_admin_list(epoch, content_lang)`: every `entry_categories` document, `name` resolved to the current default content language. |
| `/dashboard/categories/new` | `GET` | Same guards. Renders `categories_admin_form()` with one empty field per active content language. |
| `/dashboard/categories/new` | `POST` | Same guards. Reads `name_<code>` for each active language from the form body, `cms_create_category()`, `302 /dashboard/categories`. |
| `/dashboard/categories/<id>/edit` | `GET` | Same guards. `404` if `<id>` is not a valid ObjectId or no matching document exists; otherwise `categories_admin_form()` pre-filled via `cms_get_category_name_values()`. |
| `/dashboard/categories/<id>/edit` | `POST` | Same guards. `cms_update_category()`, `302 /dashboard/categories`. |
| `/dashboard/categories/<id>/delete` | `POST` | Same guards. `cms_delete_category()`, `302 /dashboard/categories`. Any `entries.categories[]` referencing `<id>` simply stop resolving to a name - no cascading cleanup. |
| `/dashboard/languages` | `GET` | Same guards. Renders `languages_admin()`: active `languages` documents (default marked, "make default"/"remove" actions on the rest) plus an "add language" `<select>` of `LANGUAGE_CATALOG` entries not yet active. |
| `/dashboard/languages/add` | `POST` | Same guards. `cms_add_language(code)`; on failure (unknown or already-active code), re-renders `/dashboard/languages` (`200`) with an error message; on success, `302 /dashboard/languages`. |
| `/dashboard/languages/<code>/default` | `POST` | Same guards. `cms_set_default_language(code)`, `302 /dashboard/languages`. Takes effect immediately (no restart) - `cms_resolve_default_lang()` re-queries `languages` on every request. |
| `/dashboard/languages/<code>/remove` | `POST` | Same guards. `cms_remove_language(code)`; rejected (re-renders with an error) if `<code>` is the current default or the only remaining language; otherwise `302 /dashboard/languages`. |
| `/dashboard/menu` | `GET` | Same guards. Renders `menu_admin_list(epoch, content_lang)`: every `menu` document (any `enabled` value, sorted by `order`), `name` resolved to the current default content language, plus `link`/`order`/`enabled`. |
| `/dashboard/menu/new` | `GET` | Same guards. Renders `menu_admin_form()` with empty `link`, `order=0`, `enabled=false` and one empty name field per active content language. |
| `/dashboard/menu/new` | `POST` | Same guards. Reads `link`/`order`/`enabled` (checkbox - absent means `false`) and `name_<code>` for each active language from the form body, `cms_create_menu_item()`, `302 /dashboard/menu`. |
| `/dashboard/menu/<id>/edit` | `GET` | Same guards. `404` if `<id>` is not a valid ObjectId or no matching document exists; otherwise `menu_admin_form()` pre-filled via `cms_get_menu_item_values()`. |
| `/dashboard/menu/<id>/edit` | `POST` | Same guards. `cms_update_menu_item()`, `302 /dashboard/menu`. |
| `/dashboard/menu/<id>/delete` | `POST` | Same guards. `cms_delete_menu_item()`, `302 /dashboard/menu`. |
| `/dashboard/entries/new` | `POST` | `require_dashboard_session_role()`. `cms_create_entry(user_id, type)` - `type` is `"blog"` for an Autor and `"page"` for an Administrador - sets `created_by: ObjectId(user_id)`, then redirects to `/dashboard/entries/<new_id>/edit`; on failure, `302 /dashboard`. |
| `/dashboard/entries/<id>/edit` | `GET` | `require_dashboard_session_role()` + `can_edit_entry()`. See [entry-editor.md](entry-editor.md). |
| `/dashboard/entries/<id>/delete` | `POST` | `require_admin_session()` - Administrador only; an Autor can never delete entries, even their own. `cms_delete_entry()`, `302 /dashboard`. |
| `/dashboard/api/entries/<id>/...` | `POST` | The editor's five AJAX endpoints (`meta`, `header`, `content`, `blocks`, `blocks/<block_id>/delete`). All share the same gate: `require_dashboard_session_role()`, `404 {"ok":false,"error":"not found"}` if `<id>` doesn't resolve, `403 {"ok":false,"error":"forbidden"}` if `!can_edit_entry()`. Payloads and responses in [entry-editor.md](entry-editor.md). |
| `/dashboard/api/block-preview` | `POST` | `require_dashboard_session()` - any role. Renders one block as epoch 3 would, for the editor ([entry-editor.md](entry-editor.md#block-preview)). |
| `/dashboard/media`, `/dashboard/api/media/...` | `GET`/`POST` | `require_dashboard_session()` - any role; writes also `require_dashboard_session_role()` + `media_can_manage()` (an `author` only touches their own directories/images). See [media-admin.md](media-admin.md). |
| `/dashboard/settings`, `/dashboard/settings/themes*`, `/dashboard/settings/preview`, `/dashboard/settings/fonts*`, `/dashboard/api/theme-assets/*` | `GET`/`POST` | **`EPOCH_MODERN` only**, `require_admin_session()`. Site name, themes (activate, colors, banner, footer, logo, CSS), epoch preview, font library. See [themes.md](themes.md), [fonts.md](fonts.md) and the full list in [routes.md](routes.md#settings-themes-fonts-preview). |
| `/dashboard/analytics` | `GET` | **`EPOCH_MODERN` only**, `require_admin_session()`. Visit report by period. See [analytics.md](analytics.md#the-report). |
| `/dashboard/users` | `GET` | **`EPOCH_MODERN` only** (other epochs `302 /dashboard`). `require_admin_session()`. Renders `users_admin_list(epoch, NULL)`: every `users` document sorted by email, with its role label ("Administrador"/"Autor" - a missing `role` field reads as `"admin"`/"Administrador"). |
| `/dashboard/users/new` | `GET` | Same guards. Renders `users_admin_form(epoch, "", "", "author", NULL)`. |
| `/dashboard/users/new` | `POST` | Same guards. `parse_user_form()` reads `email`/`password`/`role` (an absent `role` defaults to `"author"`); `cms_create_user()` hashes the password and inserts `{email, password, role}`. On success, `302 /dashboard/users`; on failure (duplicate email, invalid role, DB error), re-renders `users_admin_form()` (`200`) with "No se pudo crear el usuario. Verifica el email y la contrasena." |
| `/dashboard/users/<id>/edit` | `GET` | Same guards. `404` if `<id>` is not a valid ObjectId or no matching document exists; otherwise `users_admin_form()` pre-filled via `cms_get_user_values()` (password field always empty). |
| `/dashboard/users/<id>/edit` | `POST` | Same guards. `parse_user_form()`; if `<id> == user_id` (editing yourself), the new role is not `"admin"` and `cms_count_admins() == 1`, re-renders the form with "No puedes quitarte el unico rol de administrador." Otherwise `cms_update_user(id, email, role, password)` (`$set`s `email`/`role`, and `password` only if non-empty - a blank password leaves the stored hash unchanged). On success, `302 /dashboard/users`; on failure, re-renders the form with "No se pudo actualizar el usuario. Verifica el email." |
| `/dashboard/users/<id>/delete` | `POST` | Same guards. If `<id> == user_id`, `users_admin_list(epoch, "No puedes eliminar tu propia cuenta.")`. Else if the target user's role is `"admin"` and `cms_count_admins() == 1`, `users_admin_list(epoch, "No se puede eliminar el ultimo administrador.")`. Otherwise `cms_delete_user(id)`, `302 /dashboard/users`. |
| `/dashboard/entries` | `GET` | **`EPOCH_MODERN` only** (other epochs `302 /dashboard`), `require_dashboard_session_role()`. `dashboard_entries()`: the full entries table (`dashboard/entries/list_epoch3.html`), published and unpublished, same role filter as `/dashboard`; an unpublished entry's title carries a **Draft** badge (`entries/list-draft_epoch3.html`). |
| `/logout` | `POST` | `POST` only (`GET` → `405`). When a session cookie is present the CSRF token must match it (`csrf_token` field or `X-CSRF-Token` header), else `403`; then `destroy_session()` (if MongoDB is up). Responds `302 /` with a `Set-Cookie` that clears the cookie (`Max-Age=0`) - also with no or an expired session. Posted by the **Log out** button next to "Welcome back" on the dashboard home (`dashboard/dashboard_epoch3.html`). |

### Epoch restriction (security)

The login form (and all credential handling) is restricted to `EPOCH_MODERN`, enforced
server-side on **both** `GET` and `POST /login` via `resolve_epoch()` - never inferred from
client-supplied data beyond the same `resolve_epoch()` used everywhere else (`force_epoch`,
then `?preview_epoch=`, then `User-Agent`). `?preview_epoch=3` can therefore make an old browser
*render* the epoch 3 form, but the credential check itself still only runs for an epoch-3
request, and nothing about a session depends on the epoch it was created from. Other epochs see a static "this functionality is not available" message and never reach
`auth_login_user()`.

See [diagrams/auth-components.puml](../diagrams/auth-components.puml) for the component
diagram and [diagrams/sequence-login-route.puml](../diagrams/sequence-login-route.puml) for
the `POST /login` sequence diagram.

---

## Dashboard maintainers: Entries, Categories, Languages, Menu, Users, Settings and Analytics

Admin features under `/dashboard`. The Categories, Languages, Menu, Users, Settings and
Analytics maintainers are
separate pages, **`EPOCH_MODERN` only** (other epochs `302 /dashboard`, matching the
`/login`/`/dashboard` precedent), each requiring an **Administrador** session via
`require_admin_session()` (`http_router.c`): `503` if mongodb is not ready, `302 /login` if the
session cookie is missing/invalid, `302 /dashboard` if the session belongs to an Autor,
otherwise the request proceeds. The Entries listing and editor are embedded directly in
`/dashboard` and are available to both roles, gated by ownership instead (see "Roles and
privileges" below).

### Entries listing (`src/modules/entries_admin/`, embedded in `/dashboard`)

`entries_admin_rows(epoch, lang, type_filter, created_by_hex, unpublished_only, can_delete)`
returns the `<tbody>` rows for
the table embedded in `dashboard_epoch<N>.html` (`EPOCH_MODERN` only - `dashboard()` passes them
through `render_template()`; other epochs' static templates are returned unchanged). The table
lists `entries` documents (`db.entries.find({})` - **no** `enabled` filter, so drafts are listed
too - up to `ENTRIES_LIST_LIMIT`,
newest `header.date` first - see `cms_get_admin_entries()` in `cms_entries.c`); `type_filter`
and `created_by_hex`, if non-NULL, restrict the query to `{type: type_filter, created_by:
ObjectId(created_by_hex)}` - `dashboard()` passes `(NULL, NULL)` for an Administrador (every
entry, any `type`) and `("blog", user_id)` for an Autor (only their own `type:"blog"` entries).
Each row (`dashboard/entries/list-row_epoch<N>.html`) shows the same fields as the home/blog
list - image, title (linked to the editor, `/dashboard/entries/<id>/edit`), summary (clamped to
two lines), author, date and category tags (`elements/category/category_epoch<N>.html`). The
title carries the "Draft" badge for an unpublished entry and a "Page"/"Blog" badge for its
`type`. The actions cell has "Edit", "View" (the public `/page/<link>` or `/blog/<link>`, in a
new tab - a draft is visible there to a signed-in user) and, only when `entries_admin_rows()` gets
`can_delete` (Administrador), "Delete" (`entries/list-row-delete_epoch<N>.html`, a
`POST /dashboard/entries/<id>/delete` form behind a `confirm()`). With no entries,
`list-empty_epoch<N>.html` shows the message and its own "+ New entry" button. `cms_get_admin_entries()` shares its row population
with `cms_get_blog_entries()` via the `CmsBlogListItem` struct, which now also carries `type`
and `id` (hex ObjectId). `dashboard_epoch<N>.html` also has a static "+ New entry"
`POST /dashboard/entries/new` button above the table.

### Maintainer page conventions (epoch 3)

- **Breadcrumb**: every maintainer page except the entry editor and the media library opens with
  `<nav class="boat-rudder-dashboard__breadcrumb">` listing its ancestors as links
  (`Dashboard / Site settings / Themes`); the `<h1>` below it is the current page. It replaces the
  old "Back to ..." links at the bottom.
- **Header**: lists with a "New ..." action (entries, categories, users, menu) put it on the right
  of the `<h1>` in a `boat-rudder-dashboard__panel-header`, like the dashboard home's panels.
- **Buttons**: `boat-rudder-btn` plus `--primary` for create/save actions and `--danger` for
  destructive ones (the admin components: [themes.md](themes.md#61-admin-tokens-and-components)). Every Delete/Remove form asks first with
  `onsubmit="return confirm(...)"`.

### Entry editor (`src/db/cms_entries_admin.c`, `src/modules/entry_editor/`)

`/dashboard/entries/<id>/edit` is an AJAX content editor for one `entries` document - meta
(link/type/enabled/categories), header (image, date, per-language title/summary/author) and
`content[]` blocks - with live preview and optional autosave. It's the editor counterpart to
the read-only entries listing above. Its UX is modelled on the legacy epoch3 editor that
preceded Boat Rudder, adapted to
boat-rudder's single-document `entries` schema (one collection, `$set`/`$push`/`$pull` on one
document instead of a 5-table relational model).

> See [entry-editor.md](entry-editor.md) for the full writeup, including diagrams of the
> component layout, the `GET .../edit` render pipeline and the 5 AJAX endpoints.

- `src/db/cms_entries_admin.c` (`CmsEntryEdit`/`CmsContentBlockEdit`, see
  `cms_entries_admin.h`): `cms_get_entry_for_edit()` reads one `entries` document with **no**
  `enabled` filter and **no** language fallback (`exact_lang_value()` per `langs[]` entry, "" if
  a key is absent) - admins must be able to edit disabled entries and see exactly what's
  stored. `cms_create_entry()` / `cms_delete_entry()` insert/delete the whole document.
  `cms_update_entry_meta()` and `cms_update_entry_header()` each `$set` their fields in one
  call (`header.date` parsed from `"YYYY-MM-DD"` into a BSON UTC datetime, the inverse of
  `cms_entries.c`'s `resolve_header_fields()`). `cms_update_entry_header()` writes only
  `image_url`, `date` and the per-language `title`/`summary` - `header.author_id` is not
  editable through the editor, and `CmsEntryEdit` exposes it as a single resolved
  `header_author_name` display string rather than a per-language array. `content[]` blocks are edited individually:
  `cms_add_entry_content_block()` (`$push`es an empty block, returns its new hex `_id`),
  `cms_remove_entry_content_block()` (`$pull` by `_id`), and `cms_update_entry_content()`
  (full-array `$set` replace, used for editing/reordering existing blocks - every block must
  already have a valid `_id` from `cms_add_entry_content_block()`).
- `src/modules/entry_editor/entry_editor.c`: `entry_editor_page()` renders
  `dashboard/entries/editor/container_epoch<N>.html` - a meta sidebar
  (`meta_epoch<N>.html` + `category-option_epoch<N>.html` per category, multi-select), a header
  sidebar (`header_epoch<N>.html` - image, date, a read-only author name and one
  `header-lang-tab_epoch<N>.html` per `cms_get_languages()` entry, each holding that language's
  title and summary), and the blocks section (below), plus a JSON array of language
  codes (`["en","es",...]`) injected into `data-langs` for the inline editor script.
  `lang-tab-button_epoch<N>.html` renders one language-switcher button per language, shared by
  the header sidebar and every content block.
- `src/modules/entry_editor/entry_editor_blocks.c`: `entry_editor_render_block()` renders one
  `content[]` block's edit form - move-up/down/remove buttons, one
  `blocks/lang-field_epoch<N>.html` per language (the block's `text.<lang>`), a type-specific
  `extra_data` field for `image` ("Caption / alt text") and `byline` ("Date"), and an empty
  preview `<div>`. Loads `blocks/<type>_epoch<N>.html` for the 13 types
  `entry_page.c`'s `render_block()` renders publicly, minus `gallery`-specific handling
  (unknown types render `""`, mirroring that function). Only epoch 3 editor templates exist,
  which is consistent with the editor being `EPOCH_MODERN`-only. `entry_editor_render_blocks()`
  renders every block and wraps them in `blocks_epoch<N>.html`, which also holds the
  "+ Add block" dropdown (one button per supported type) and the "Save all"/autosave controls.
- **Editor JS** (inline `<script>` in `container_epoch3.html`, no external `.js` file, following
  the convention in `layout/layout_epoch3.html` / `menu/menu_epoch3.html`): language tabs
  toggle every `.boat-rudder-entry-editor__lang-panel[data-lang="<code>"]` element (header
  sidebar + each block) via `setLang()`. `saveMeta()`/`saveHeader()`/`saveContent()` each `POST`
  one of the `/dashboard/api/entries/<id>/...` endpoints below as
  `application/x-www-form-urlencoded`; `editorSaveAll()` runs all three. Editing any field marks
  the page dirty; an "Autosave" checkbox schedules `editorSaveAll()` 3s after the last edit.
  `insertNewComponent(type)` / `removeComponent()` call the `/blocks` and
  `/blocks/<block_id>/delete` endpoints and patch the DOM without a full reload.
  `moveBlockUp()`/`moveBlockDown()` just reorder DOM nodes - the new order is only persisted on
  the next `saveContent()` (block `order` = DOM index at save time). Previews of simple blocks
  are built client-side; `code-text`, `gallery`, `table`, `youtube-embed`, `image` and
  `paragraph` are fetched from `POST /dashboard/api/block-preview`, which returns the real epoch 3
  rendering (`entry_page_render_block()`), so the preview can't drift from the public page.
- **Editor UX** (epoch 3): the editor uses a document-style layout with a fixed top bar (save-all, autosave toggle, publish toggle, language tabs), a block type toolbar, and a two-column layout (left: meta + header sidebars; right: content blocks). Blocks default to a document-like preview mode; clicking a block enters edit mode showing the full form. Paragraph blocks have a WYSIWYG rich-text toolbar. Title blocks have H1-H6 level selectors. Gallery blocks show a thumbnail preview area with drag-and-drop reordering and a "Select photos" button that opens the media picker modal. Blocks support drag-and-drop reordering.
- **Gallery block**: supported in editor (thumbnail preview, drag-drop reorder) and public view (see "Gallery block" below). Selecting photos opens the `/dashboard/api/media/modal` endpoint, which returns the media admin UI inside a modal overlay.
- **Drafts**: the publish toggle is the entry's `enabled` flag. A draft is listed in the
  dashboard and viewable at its public URL by any signed-in user (marked `[Draft]`), and is
  `404` for everyone else - see [rendering.md](rendering.md#cms-entries-get-pagelink-get-bloglink).
- **Future work**: see [entry-editor.md](entry-editor.md#7-future-work).

### Content language resolution (`src/db/cms_languages.c`, `src/db/language_catalog.c`)

A new `languages` collection (`LANGUAGES_COLLECTION`, `src/db/mongodb_manager.h`) replaces
`configs/settings.conf`'s static `lang` as the source of the *content* language - the key used
into every `map<lang,string>` field across `entries`/`menu`/`entry_categories`:

```c
typedef struct { char *code; char *name; int is_default; } CmsLanguageItem;
```

- `cms_languages_ensure_seeded()`: called once from `main()` after `mongodb_manager_init()`
  succeeds; inserts `{code:"en", name:"English", is_default:true}` iff `languages` is empty.
- `cms_resolve_default_lang(out, out_size)`: `db.languages.findOne({is_default:true}).code`,
  falling back to `iso_lang(lang)` (the global `lang` from `configs/settings.conf`) if mongodb
  is not ready or no document has `is_default:true`. It is the **site default**; the language a
  request actually renders in is resolved once per request by `request_lang_set()`
  (`src/utils/request_lang.c`): a valid `?lang=` wins, then a valid `lang` cookie (set by
  `/language/set`), then this default. The router reads it back with `request_lang()` as
  `content_lang` and passes it to `buildHomeWebSite()`, `blog_list()`, `serve_cms_entry()`, and
  `menu()` reads it itself. See [rendering.md](rendering.md#language-selection-and-persistence).
- Because callers now pass an already-resolved ISO code (which may be any `LANGUAGE_CATALOG`
  entry, not just `en`/`es`), `cms_get_entry_by_link()`, `cms_get_blog_entries()`
  (`cms_entries.c`) and `cms_get_menu_items()` (`cms_menu.c`) no longer call `iso_lang()`
  internally - `iso_lang()` is now only called from `cms_resolve_default_lang()`'s fallback path.
- `src/db/language_catalog.c`: a curated, static `LANGUAGE_CATALOG[]` of ISO 639-1 codes with
  their English name, native name and short form (`abbr`, for the compact nav-bar button), used
  to validate/name new languages, to build the "add language" `<select>` on
  `/dashboard/languages`, and by the public language picker.
- `cms_add_language(code)` / `cms_set_default_language(code)` / `cms_remove_language(code)`:
  insert/promote/remove a `languages` document. `cms_set_default_language()` takes effect
  immediately (no restart), since `cms_resolve_default_lang()` re-queries `languages` on every
  request. `cms_remove_language()` refuses to remove the current default or the last remaining
  language.

### Categories CRUD (`src/db/cms_categories.c`, `src/modules/categories_admin/`)

Basic CRUD over `entry_categories` (`ENTRY_CATEGORIES_COLLECTION`), already read by
`resolve_category_names()` in `cms_entries.c`:

- `cms_get_categories(lang, &items, &count)`: `db.entry_categories.find().sort({_id:1})`,
  `name` resolved to `lang` (`CmsCategoryItem { id, name }`, `id` = hex ObjectId).
- `cms_get_category_name_values(id_hex, langs, lang_count, out_values)`: exact (no "en"
  fallback) `name.<langs[i].code>` per active language, for the edit form.
- `cms_create_category()` / `cms_update_category()` / `cms_delete_category()`: build
  `name: {<code>: <value>, ...}` from the active-language form fields. Deleting a category
  leaves dangling `entries.categories[]` references, which `resolve_category_names()`'s `$in`
  lookup already ignores gracefully.
- `src/modules/categories_admin/categories_admin.c`:
  - `categories_admin_list(epoch, lang)` -> `dashboard/categories/list_epoch<N>.html` (+
    `list-row_epoch<N>.html` per category, or `list-empty_epoch<N>.html`).
  - `categories_admin_form(epoch, id, langs, lang_count, values, error_message)` ->
    `dashboard/categories/form_epoch<N>.html` (+ one `form-field_epoch<N>.html` per active
    language, + `form-error_epoch<N>.html` if `error_message`). `id == ""` for "new category".

### Languages admin (`src/modules/languages_admin/`)

- `languages_admin(epoch, error_message)` -> `dashboard/languages/list_epoch<N>.html`: one row
  per active `languages` document (`list-row_epoch<N>.html` for the default, with no actions;
  `list-row-actions_epoch<N>.html` for the rest, with "make default"/"remove" forms), plus
  `option_epoch<N>.html` per `LANGUAGE_CATALOG` entry not yet active, plus
  `list-error_epoch<N>.html` if `error_message`.

### Menu CRUD (`src/db/cms_menu.c`, `src/modules/menu_admin/`)

Basic CRUD over `menu` (`MENU_COLLECTION`), already read (read-only, `enabled: true` only) by
`cms_get_menu_items()` for the site's nav (`src/modules/menu/menu.c`):

- `cms_get_menu_admin_items(lang, &items, &count)`: `db.menu.find().sort({order:1})` (no
  `enabled` filter - the admin must see disabled items too), `name` resolved to `lang`
  (`CmsMenuAdminItem { id, link, name, order, enabled }`, `id` = hex ObjectId).
- `cms_get_menu_item_values(id_hex, langs, lang_count, out_values, out_link, out_link_size,
  &out_order, &out_enabled)`: exact (no "en" fallback) `name.<langs[i].code>` per active
  language plus `link`/`order`/`enabled`, for the edit form.
- `cms_create_menu_item()` / `cms_update_menu_item()` / `cms_delete_menu_item()`: build
  `{link, order, enabled, name: {<code>: <value>, ...}}` from the form fields.
- `src/modules/menu_admin/menu_admin.c`:
  - `menu_admin_list(epoch, lang)` -> `dashboard/menu/list_epoch<N>.html` (+
    `list-row_epoch<N>.html` per menu item, or `list-empty_epoch<N>.html`).
  - `menu_admin_form(epoch, id, link, order, enabled, langs, lang_count, values, error_message)`
    -> `dashboard/menu/form_epoch<N>.html` (`link`/`order`/`enabled` as a text/number/checkbox
    input, + one `form-field_epoch<N>.html` per active language, + `form-error_epoch<N>.html` if
    `error_message`). `id == ""` for "new menu item". An unchecked "enabled" checkbox sends no
    `enabled` field at all - its absence in the POST body means `false`.

### Roles and privileges (`users.role`, `entries.created_by`)

Two roles: **Administrador** (`users.role == "admin"`, full access - everything described in
this document) and **Autor** (`users.role == "author"`, can only create/edit their own
`type:"blog"` entries - no Categories/Languages/Menu/Users, no `type:"page"` entries, and can
never delete entries). A `users` document with no `role` field (the original seed account) is
treated as `"admin"` everywhere it's read - no migration is required, and an admin can open that
user in `/dashboard/users/<id>/edit` and Save to persist `role: "admin"` explicitly.

`entries` documents gained an optional `created_by: ObjectId` field, set once by
`cms_create_entry()` at creation time and exposed as a 24-char hex string
(`CmsEntryEdit.created_by`, `""` if absent) by `cms_get_entry_for_edit()`. Pre-existing entries
have no `created_by`, so they are only ever editable by an Administrador.

`can_edit_entry(role, user_id, entry)` (`http_router.c`) is the single ownership gate used by
every entry editor route: `true` if `role == "admin"`, or if `role == "author"` and
`entry->type == "blog"` and `entry->created_by == user_id`.

### Users CRUD (`src/db/cms_users_admin.c`, `src/modules/users_admin/`)

Basic CRUD over `users` (the same collection `auth_login_user()` reads):

- `cms_get_users(&items, &count)`: `db.users.find().sort({email:1}).limit(USERS_LIST_LIMIT)` ->
  `CmsUserAdminItem { id, name, email, role }` (`role` is `"admin"` if the field is absent).
- `cms_get_user_values(id_hex, out_email, ..., out_role, ...)`: `email`/`role` for the edit form.
- `cms_get_user_name_by_id(id_hex)`: malloc'd `users.name` (`""` if not found). Used both by the
  user edit form and, via `cms_entries.c`, to resolve an entry's `header.author_id` into a
  display name.
- `cms_get_user_role(id_hex, out_role, ...)`: `role` only (`"admin"` if absent or on any
  lookup error) - used by `require_dashboard_session_role()` and `/dashboard`.
- `cms_count_admins()`: `db.users.countDocuments({$or: [{role:"admin"}, {role:{$exists:false}}]})`
  - used by the router to block removing/demoting the last admin.
- `cms_create_user(name, email, password, role)`: hashes `password` with `crypto_pwhash_str()`
  (Argon2id, same as `auth.c`) and inserts `{name, email, password, role}`. Fails (`-1`) on a
  duplicate email, an invalid `role` (must be `"admin"` or `"author"`), or a DB error.
- `cms_update_user(id_hex, name, email, role, new_password)`: `$set`s `name`/`email`/`role`, plus
  a freshly hashed `password` only if `new_password != ""` (a blank password leaves the stored
  hash unchanged). Fails on the same conditions as `cms_create_user()`, plus "not found".
- `cms_delete_user(id_hex)`: `db.users.deleteOne({_id})`. Self-delete and last-admin protection are enforced by the router (`/dashboard/users/<id>/delete`), not here.
- `cms_get_username_by_id(id_hex, out, out_size)`: reads the user's `email` and returns the part before `@`, sanitized to alphanumeric/dash/underscore/dot. Used by the media admin to build the physical directory path `html/content/posts/<username>/`.
- `src/modules/users_admin/users_admin.c`:
  - `users_admin_list(epoch, error_message)` -> `dashboard/users/list_epoch<N>.html` (+
    `list-row_epoch<N>.html` per user with a "Role" column - "Administrador"/"Autor" - or
    `list-empty_epoch<N>.html`, + `list-error_epoch<N>.html` if `error_message`).
  - `users_admin_form(epoch, id, name, email, role, error_message)` -> `dashboard/users/form_epoch<N>.html`
    (name input, email input, always-empty password input, a 2-option `role` `<select>`
    ("Administrador"/"Autor"), + `form-error_epoch<N>.html` if `error_message`). `id == ""` for
    "new user"; action is `/dashboard/users/new` or `/dashboard/users/<id>/edit`.
    The `name` field is read from the POST body directly by the router
    (`parse_urlencoded_field(..., "name", ...)`), not by `parse_user_form()`.

### Media Admin (`/dashboard/media`, `src/db/cms_media.c`, `src/modules/media_admin/`)

A media library for uploading and managing images used in entries. Requires a dashboard session (any role); an `author` can browse everything but only upload into, rename, delete and move their own directories and images (`media_can_manage()`). Uploads are limited to JPEG/PNG/GIF/WebP (extension and magic bytes). Full documentation: [media-admin.md](media-admin.md).

**Collections**:
- `media` - uploaded file metadata: `{ _id, name, date, format, author_id, dir_id }`.
- `media_directories` - folder structure: `{ _id, name, parent, author_id }`. Each directory maps to a physical folder at `html/content/posts/<username>/<dirname>/`.
- `media_galleries` - gallery data for public rendering: `{ _id, content: [url, ...], entry_id }`. Created/updated automatically when saving an entry's `gallery` content block.

**Image variants**: `scripts/image-optimizer.sh` processes each upload via ImageMagick and generates 5 variants (`_full`, `_half`, `_small`, `_medium`, `_micro`) before deleting the original. The base URL (without suffix) is a symlink to `_half`. Requires `imagemagick`, `jpegoptim`, `gifsicle`.

**Routes**:

| Route | Behavior |
|---|---|
| `GET /dashboard/media` | Full media admin page |
| `GET /dashboard/api/media/contents` | Paginated photo grid (HTML fragment, `MEDIA_PAGE_SIZE` = 36) |
| `GET /dashboard/api/media/directory/item` | One directory tile (after creating it) |
| `GET /dashboard/api/media/modal` | Media picker modal (for entry editor) |
| `POST /dashboard/api/media/directory` | Create directory |
| `POST /dashboard/api/media/directory/rename` | Rename own directory + physical folder (`id`, `newname`) |
| `POST /dashboard/api/media/directory/delete` | Delete own directory (`id`); refused while it has images |
| `POST /dashboard/api/media/delete` | Delete items and every size variant |
| `POST /dashboard/api/media/move` | Move items (files + DB) to another directory |
| `POST /dashboard/api/media/upload` | Multipart upload → optimizer → DB insert; no directory → author's `default` |
| `GET /gallery/<id>` | **Public** gallery page (epoch-aware, no session required) |

**Gallery block** (`elements/gallery/`, `src/modules/entry_page/entry_page.c`): the `gallery` content block type stores semicolon-separated image URLs in `content[].text` and the `media_galleries._id` in `extra_data`. When saving an entry with gallery blocks, the router calls `cms_upsert_media_gallery()` to keep the `media_galleries` collection in sync. Public rendering:
- Epoch 3: CSS grid (3 columns), max 5 visible + "+N remaining" overlay. Click opens a full-screen lightbox with prev/next navigation and keyboard support (←/→/Esc).
- Epochs 1-2: table of thumbnails linking to `/gallery/<id>?img=N`; the gallery page shows a main image with prev/next links and a thumbnail strip.
- Epochs -1/0: text link to `/gallery/<id>`, whose page shows a QR code (through a `/qr/<code>` short link) instead of the photos.

### Routing helpers (`src/web_server/http_router.c`)

- `require_dashboard_session(ctx, req, epoch, user_id_out)`: shared by every `/dashboard/*`
  sub-route - `503` if mongodb is not ready, `302 /login` if the session cookie is
  missing/invalid, else returns `1` with `user_id_out` filled.
- `require_dashboard_session_role(ctx, req, epoch, user_id_out, role_out, role_size)`:
  `require_dashboard_session()` + `cms_get_user_role()`; `role_out` is `"admin"` or `"author"`
  (a missing `role` field, or a lookup error, defaults to `"admin"`). Used by
  `/dashboard/entries/new` and the entry editor routes.
- `require_admin_session(ctx, req, epoch, user_id_out)`: `require_dashboard_session_role()` +
  `302 /dashboard` (returning `0`) if `role != "admin"`. Used by `/dashboard/users*`,
  `/dashboard/categories*`, `/dashboard/languages*`, `/dashboard/menu*`,
  `/dashboard/settings*` (including fonts), `/dashboard/analytics`,
  `/dashboard/api/theme-assets/*` and `/dashboard/entries/<id>/delete`.
- `can_edit_entry(role, user_id, entry)`: see "Roles and privileges" above.
- `parse_user_form(req, out_email, ..., out_password, ..., out_role, ...)`: reads
  `email`/`password`/`role` from the POST body for `cms_create_user()`/`cms_update_user()`; an
  absent `role` field defaults to `"author"` (the safer default for new accounts).
- `match_id_route(decoded_url, prefix, suffix, id_out, id_size)`: matches
  `"<prefix>/<id><suffix>"` (e.g. `/dashboard/categories/<id>/edit`,
  `/dashboard/languages/<code>/remove`), extracting `<id>` into `id_out`.
- `match_block_delete_route(decoded_url, entry_id_out, entry_id_size, block_id_out,
  block_id_size)`: one-off two-id matcher for
  `/dashboard/api/entries/<entry_id>/blocks/<block_id>/delete` (the only route with two id
  segments, so not folded into `match_id_route()`).
- `parse_urlencoded_multi(body, body_length, key, out_values, max_count)`: like
  `parse_urlencoded_field()`, but collects every value for `key` (used for the entries meta
  form's `categories` multi-select, where each selected option shares `name="categories"`).
  Each returned `out_values[i]` is malloc'd; the caller frees them.
- `match_theme_epoch_route(decoded_url, segment, key_out, …, epoch_out, …)`: matches
  `/dashboard/settings/themes/<key>/<segment>/<epoch>` for the banner/footer/logo saves (the
  epoch may be `-1`).
- `match_theme_css_url()`: matches `/themes/<key>/styles_epoch3.css` and `/themes/<key>/styles_admin_epoch3.css`.
- `match_theme_css_editor()`: matches the CSS editor's `…/css` and `…/admin-css` routes (plus `/restore`).
- `theme_assets_dir()`, `sanitize_asset_filename()`, `mkdir_recursive()`: validation and
  filesystem helpers for the theme-assets and font uploads ([security.md](security.md#request-handling)).
- `parse_bg_color_field()`: joins a color picker and an opacity field into `#rrggbb[aa]`.
- `viewer_can_preview_drafts()`: any valid session - lets `serve_cms_entry()` show drafts.

---

## Settings (`/dashboard/settings`)

Admin-only, epoch 3. Summary - the reference is [themes.md](themes.md):

| Page | What it edits | Stored in |
|---|---|---|
| `/dashboard/settings` | Site name (replaces `{{SITE_NAME}}` everywhere) | `site_settings.site_name` |
| `/dashboard/settings/themes` | The list of directories under `html/themes/`: each one's name, *Active* or *Set active*, and *Edit* | `site_settings.active_theme` |
| `…/themes/<key>` | One theme: *Set active* (unless it is), the 40 colors (backgrounds with opacity), the epoch 3 logo font, links to its banner/footer/logo/CSS editors | `themes.colors`, `themes.logo_font` |
| `…/themes/<key>/banner`, `…/footer` | Raw markup per epoch (−1…3) + image upload/browse | `themes.banner_html`, `themes.footer_html`, files in `html/themes/<key>/assets/` |
| `…/themes/<key>/logo` | Per-epoch logo: text (0), image (−1, 1, 2; −1 auto-converted to WBMP) or either (3) | `themes.logo` |
| `…/themes/<key>/css`, `…/admin-css` | Full epoch 3 stylesheets, public and admin (one tab each), with *Restore original* | `themes.css_epoch3`, `themes.css_admin_epoch3` |
| `/dashboard/settings/preview` | Nothing - an iframe of the site in any epoch and screen size (`?preview_epoch=`) | - |
| `/dashboard/settings/fonts` | Uploaded font library ([fonts.md](fonts.md)) | `fonts`, `html/assets/fonts/` |

Modules: `src/modules/site_settings_admin/`, `src/modules/fonts_admin/`; templates under
`html/templates/dashboard/settings/` and `…/fonts/`.

## Analytics (`/dashboard/analytics`)

Admin-only, epoch 3. A report of the per-day visit counters recorded for every public page view:
totals per epoch, routes, browsers and operating systems (grouped by family), countries (grouped
by continent) and the top 20 entries, for a day, ISO week, month, year, last year, all time or a
custom range. Module `src/modules/analytics_view/`; reference [analytics.md](analytics.md).

---

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
