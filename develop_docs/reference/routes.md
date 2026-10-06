# Boat Rudder - Route Reference

The single table of every route `src/web_server/http_router.c` answers, with its method, guard,
allowed epochs, implementing module and response. Other documents describe *how* a route works
and link here for the full list.

How a request reaches this table (header read, body read, client IP, per-request state,
analytics) is described in [data-flow.md](data-flow.md#4-http-router). The WAP gateway's UDP
traffic is not routed here directly - it re-enters this table over loopback HTTP (see
[wap-gateway.md](wap-gateway.md)).

---

## Dispatch rules

- Routing is an ordered `if / else if` chain inside `http_route()`, matched on the
  **URL-decoded path without its query string** (`decoded_url`). The first match wins, so the
  order below matters where prefixes overlap (`/blog/category/…` and `/blog/categories` are
  tested before `/blog/<link>`; `/dashboard/settings/themes/<key>/css/restore` before
  `…/css`).
- `GET` and `HEAD` share every handler; `HEAD` responses are cut after the headers by
  `send_or_error()`.
- `OPTIONS` (any path) → `204` with `Allow: GET, HEAD, OPTIONS, POST`. Any other method → `405`.
- Unmatched `GET`/`HEAD` paths fall through to the static file server rooted at the CLI root
  directory (normally `html/`).
- Errors (`400 403 404 405 413 431 500 503`) are rendered by `send_error_response()` in the
  visitor's epoch, except the JSON/AJAX endpoints, which answer plain-text or JSON errors.

### Guards

| Guard | Meaning | On failure |
|---|---|---|
| - | Public | - |
| **E3** | `resolve_epoch() == 3` | Page routes: `302 /dashboard`. API routes (`/dashboard/api/*`): `403 Forbidden` (plain text). |
| **S** | `require_dashboard_session()`: valid `session` cookie | `503` if MongoDB is down, otherwise `302 /login` |
| **S+R** | `require_dashboard_session_role()`: session, role loaded (missing role = `admin`) | as **S** |
| **A** | `require_admin_session()`: session with role `admin` | as **S**, plus `302 /dashboard` for an `author` |
| **Own** | `can_edit_entry()`: `admin`, or `author` on a `blog` entry they created | `302 /dashboard` (page) or JSON `403` (API) |
| **Theme** | `theme_key_is_valid(<key>)`: `html/themes/<key>/` exists | `404` |

`resolve_epoch()` returns `force_epoch` if configured, else a valid `?preview_epoch=N`
anywhere in the query string, else `detect_epoch(User-Agent)`. Because `preview_epoch` is
honored on every route, the dashboard preview iframe can browse the whole site in any epoch.

---

## Public pages (`GET`/`HEAD`)

All public pages render in the visitor's epoch (−1…3) unless noted.

| Path | Query | Module / handler | Response |
|---|---|---|---|
| `/` | - | `buildHomeWebSite()` (orchestrator: banner, home content, blog list) | Home page |
| `/blog` | - | `blog_list()` + `category_menu_render()` + `buildBlogListWebSiteAtUrl()` | Blog listing (≤ `BLOG_LIST_LIMIT` = 50) |
| `/blog/category/<slug>` | - | `blog_list_category()`; `<slug>` matched against `slugify(category name)` | Filtered listing, `404` if no category matches |
| `/blog/categories` | `return` | `category-menu/category-menu-page_epoch<N>.html` | Full category menu + back link. Only epochs with that template (today: −1); others `404` |
| `/blog/<link>` | `__page`, `code_lines` | `serve_cms_entry(…, "blog")` → `entry_page()` | Entry, `404` if missing / not `blog` / draft without session |
| `/page/<link>` | `__page`, `code_lines` | `serve_cms_entry(…, "page")` | Same, for `type: "page"` |
| `/gallery/<id>` | `img` | inline in router (`cms_get_media_gallery()`) | Epoch 3: grid + lightbox. Epochs 1-2: paginated viewer (`?img=N`). Epochs −1/0: QR code to the gallery (via `/qr/<code>`). `404` if `<id>` isn't 24 chars or unknown |
| `/menu` | `return` | `menu/menu-page_epoch<N>.html` | Full nav menu + back link - the target of WML's compact `[Menu]` link. Only epochs with that template (today: −1); others `404` |
| `/language` | `return` | `language_page()` | Language picker page |
| `/language/set` | `code`, `return` | inline | `302` to `return` (sanitized, absolute URL) with `Set-Cookie: lang=<code>; Max-Age=31536000` when `code` is a configured language |
| `/theme` | `return` | `theme_page()` | Theme picker page. Templates exist for epochs 1-3 only; others get `500` |
| `/theme/set` | `key`, `return` | inline | `302` to `return` with `Set-Cookie: theme=<key>; Max-Age=31536000` when `key` is a valid theme. No epoch check |
| `/qr/<code>` | - | `short_link_resolve()` | `302` to the stored `target_path`, `404` if unknown |
| `/youtube-qr/<video-id>` | `back` | inline, `generate_qr_halfblock_text()` / `generate_qr_asciiblock_text()` | **Always epoch 0's page**, whatever the visitor's epoch: a text QR of `https://youtu.be/<id>` + back link |
| `/image-qr/<code>` | `back` | inline, same generators | **Always epoch 0's page**: a text QR of `<public_url or Host>/qr/<code>` + back link |
| `/themes/<key>/styles_epoch3.css` | - | `match_theme_css_url()` → `cms_get_theme_css()` | The theme's epoch 3 stylesheet, from the DB override or the on-disk file; `text/css`. Guard: **Theme** |
| `/login` | - | `login()` | Login form (epoch 3) or "not available" page (other epochs). `302 /dashboard` if already signed in |
| `/logout` | - | inline, `destroy_session()` | `302 /` clearing the `session` cookie |
| *anything else* | - | `serve_static_file()` | File from the root directory with `Last-Modified`/`304`; `403`/`404`/`500` rendered per epoch |

Query parameters understood on **every** route (read before routing, see
[data-flow.md](data-flow.md#4-http-router)):

| Parameter | Effect |
|---|---|
| `preview_epoch=N` | Render as epoch `N` (−1…3) - see guards above |
| `lang=xx` | Content language for this request, if `xx` is configured (epochs 0/1 and WML switch language this way, links are re-tagged to keep it) |
| `theme=key` | Theme for this request, if valid (epochs 1/2 keep it by link re-tagging) |
| `code_lines=off` | Hide code-text line numbers (epochs 0-2) |
| `wml_pages=all` | Serve a WML deck unpaginated (used by the WAP gateway) |
| `__page=N` | Page `N` of a paginated WML deck |

---

## Authentication

| Method | Path | Guard | Handler | Response |
|---|---|---|---|---|
| `POST` | `/login` | epoch 3 for the credential check | `auth_login_user()`, `create_session()` | `302 /dashboard` + `Set-Cookie: session=…` on success; form with "Invalid email or password." on failure; non-epoch-3 browsers get the "not available" page without touching the DB; `503` if MongoDB is down |

Fields: `user`, `password` (urlencoded). See [dashboard.md](dashboard.md#login-dashboard-and-logout).

---

## Dashboard pages

Every dashboard page is epoch 3 only (**E3**) and rendered through `buildPageWebSite()` with the
title `{{SITE_NAME}} - Dashboard`.

| Method | Path | Guard | Module | Notes |
|---|---|---|---|---|
| `GET` | `/dashboard` | S+R (no E3 redirect) | `dashboard()` | Entries table + role-based nav (epoch 3). Other epochs get a bare per-epoch welcome page with no admin tools |
| `POST` | `/dashboard/entries/new` | E3, S+R | `cms_create_entry()` | Creates a draft (`page` for admin, `blog` for author), `302` to its editor |
| `GET` | `/dashboard/entries/<id>/edit` | E3, S+R, Own | `entry_editor_page()` | `404` if unknown |
| `POST` | `/dashboard/entries/<id>/delete` | E3, A | `cms_delete_entry()` | `302 /dashboard` |
| `GET` | `/dashboard/categories` | E3, A | `categories_admin_list()` | |
| `GET`/`POST` | `/dashboard/categories/new` | E3, A | `categories_admin_form()` / `cms_create_category()` | Fields `name_<lang>` |
| `GET`/`POST` | `/dashboard/categories/<id>/edit` | E3, A | same / `cms_update_category()` | |
| `POST` | `/dashboard/categories/<id>/delete` | E3, A | `cms_delete_category()` | |
| `GET` | `/dashboard/menu` | E3, A | `menu_admin_list()` | |
| `GET`/`POST` | `/dashboard/menu/new` | E3, A | `menu_admin_form()` / `cms_create_menu_item()` | Fields `link`, `order`, `enabled`, `name_<lang>` |
| `GET`/`POST` | `/dashboard/menu/<id>/edit` | E3, A | same / `cms_update_menu_item()` | |
| `POST` | `/dashboard/menu/<id>/delete` | E3, A | `cms_delete_menu_item()` | |
| `GET` | `/dashboard/languages` | E3, A | `languages_admin()` | |
| `POST` | `/dashboard/languages/add` | E3, A | `cms_add_language()` | Field `code` (from `LANGUAGE_CATALOG`) |
| `POST` | `/dashboard/languages/<code>/default` | E3, A | `cms_set_default_language()` | |
| `POST` | `/dashboard/languages/<code>/remove` | E3, A | `cms_remove_language()` | Refused for the default / last language |
| `GET` | `/dashboard/users` | E3, A | `users_admin_list()` | |
| `GET`/`POST` | `/dashboard/users/new` | E3, A | `users_admin_form()` / `cms_create_user()` | Fields `name`, `email`, `password`, `role` (default `author`) |
| `GET`/`POST` | `/dashboard/users/<id>/edit` | E3, A | same / `cms_update_user()` | Empty `password` keeps the current one |
| `POST` | `/dashboard/users/<id>/delete` | E3, A | `cms_delete_user()` | Refused for yourself and for the last admin |
| `GET` | `/dashboard/media` | E3, S | `media_admin_page()` | Any signed-in user |
| `GET` | `/dashboard/analytics` | E3, A | `analytics_view()` | Query: `period` (`day`/`week`/`month`/`year`/`all`/`range`), `date`, `year`, `month`, `week`, `from`, `to`. See [analytics.md](analytics.md#the-report) |

### Settings (themes, fonts, preview)

All **E3, A**. Detailed behavior in [themes.md](themes.md) and [fonts.md](fonts.md).

| Method | Path | Handler | Notes |
|---|---|---|---|
| `GET`/`POST` | `/dashboard/settings` | `site_settings_general_page()` / `cms_update_site_name()` | Field `site_name` |
| `GET` | `/dashboard/settings/themes` | `site_settings_themes_page()` | Every directory under `html/themes/` with its colors |
| `POST` | `/dashboard/settings/themes/<key>/activate` | `cms_set_active_theme()` | Sets `site_settings.active_theme` |
| `POST` | `/dashboard/settings/themes/<key>/colors` | `cms_update_theme_colors()`, `cms_update_theme_logo_font()` | 34 color fields (hyphenated names, `*-alpha` companions for backgrounds) + `logo-font`. Guard: Theme |
| `GET` | `/dashboard/settings/themes/<key>/banner` | `site_settings_banner_page()` | Guard: Theme |
| `POST` | `/dashboard/settings/themes/<key>/banner/<epoch>` | `cms_update_theme_banner()` | Field `html` (≤ 128 KiB); `""` restores the file |
| `GET` | `/dashboard/settings/themes/<key>/footer` | `site_settings_footer_page()` | Guard: Theme |
| `POST` | `/dashboard/settings/themes/<key>/footer/<epoch>` | `cms_update_theme_footer()` | Field `html` |
| `GET` | `/dashboard/settings/themes/<key>/logo` | `site_settings_logo_page()` | Guard: Theme |
| `POST` | `/dashboard/settings/themes/<key>/logo/<epoch>` | `cms_update_theme_logo_config()` | Fields `mode`, `text`, `font`, `navbar-image`, `footer-image`; mode forced per epoch server-side |
| `GET` | `/dashboard/settings/themes/<key>/css` | `site_settings_css_page()` | Guard: Theme |
| `POST` | `/dashboard/settings/themes/<key>/css` | `cms_update_theme_css()` | Field `css` (≤ 128 KiB) |
| `POST` | `/dashboard/settings/themes/<key>/css/restore` | `cms_update_theme_css(key, "")` | Back to the on-disk file |
| `GET` | `/dashboard/settings/preview` | `site_settings_preview_page()` | Iframe + `?preview_epoch=` |
| `GET` | `/dashboard/settings/fonts` | `fonts_admin_list()` | Query `error` shows a message |
| `POST` | `/dashboard/settings/fonts/upload` | `cms_add_font()` | Multipart `name`, `file` (`.ttf/.otf/.woff/.woff2`) |
| `POST` | `/dashboard/settings/fonts/<id>/delete` | `cms_delete_font()` + file removal | |

---

## Dashboard AJAX API

All **E3** (`403` plain text otherwise). Responses are JSON (`build_json_response()`) unless noted.

### Entry editor

Guards **S+R** and **Own** on every route except `block-preview` (**S**). Detailed in
[entry-editor.md](entry-editor.md).

| Method | Path | Handler | Response |
|---|---|---|---|
| `POST` | `/dashboard/api/entries/<id>/meta` | `cms_update_entry_meta()` | `{ok}` - fields `link`, `type`, `enabled`, `categories` (repeated). For an author `type` is always forced to `blog` |
| `POST` | `/dashboard/api/entries/<id>/header` | `cms_update_entry_header()` | `{ok}` - image, date, hide-author, `title_<lang>`, `summary_<lang>` |
| `POST` | `/dashboard/api/entries/<id>/content` | `cms_update_entry_content()` + `cms_upsert_media_gallery()` | `{ok, ids:[…]}` - the block ids, including any minted for blocks sent without one |
| `POST` | `/dashboard/api/entries/<id>/blocks` | `cms_add_entry_content_block()` | `{ok, block_id, html}` - the new block's editor markup |
| `POST` | `/dashboard/api/entries/<id>/blocks/<block-id>/delete` | `cms_remove_entry_content_block()` | `{ok}` |
| `POST` | `/dashboard/api/block-preview` | `entry_page_render_block(type, text, extra, 3)` | **HTML** fragment: the real epoch 3 rendering of a `code-text`, `gallery`, `table`, `youtube-embed`, `image` or `paragraph` block (fields `type`, `text`, `extra`) |

### Media library

Guard **S** (any signed-in user, no per-item ownership check). Detailed in
[media-admin.md](media-admin.md).

| Method | Path | Handler | Response |
|---|---|---|---|
| `GET` | `/dashboard/api/media/contents` | `cms_get_media_items()` + `media_admin_render_items()` | JSON-wrapped HTML; query `directory`, `start`, `end` (page = `MEDIA_PAGE_SIZE` = 36) |
| `GET` | `/dashboard/api/media/directory/item` | `media_admin_render_directory_item()` | JSON-wrapped HTML; query `id` |
| `GET` | `/dashboard/api/media/modal` | `media_admin_modal()` | **HTML** - picker modal, newest uploads first |
| `POST` | `/dashboard/api/media/directory` | `cms_create_media_directory()` + `mkdir` | Field `newpath` (3-60 chars `[A-Za-z0-9_-]`) |
| `POST` | `/dashboard/api/media/directory/rename` | `cms_rename_media_directory()` + `rename()` | |
| `POST` | `/dashboard/api/media/directory/delete` | `cms_delete_media_directory()` + `rmdir()` | |
| `POST` | `/dashboard/api/media/delete` | `cms_delete_media()` + unlink of every size variant | |
| `POST` | `/dashboard/api/media/move` | `cms_move_media()` + `rename()` of every variant | Fields `ids` (comma-separated), `dest_dir`; plain-text result |
| `POST` | `/dashboard/api/media/upload` | file write + `scripts/image-optimizer.sh` + `cms_insert_media()` | `{ok, filename, dir_id}`; multipart `file`, `media-directory-selected` (empty → author's `default` directory) |

### Theme assets

Guard **A**. Back the banner/footer/logo editors' image widgets; files live in
`html/themes/<key>/assets/<component>/epoch<N>/`.

| Method | Path | Query | Response |
|---|---|---|---|
| `GET` | `/dashboard/api/theme-assets/list` | `theme`, `component` (`mainbanner`/`footer`/`menu`), `epoch` (`-1`…`3`) | `{"files":[…]}` (empty list on any invalid argument) |
| `POST` | `/dashboard/api/theme-assets/upload` | same | `{ok, filename}`; multipart `file` (`.png/.jpg/.jpeg/.gif`; epoch −1: `.png/.jpg/.jpeg` only, converted to `.wbmp` for `component=menu`) |
| `POST` | `/dashboard/api/theme-assets/delete` | same + `file` | Plain text `Deleted` |

---

## Response helpers

| Helper | Use |
|---|---|
| `build_epoch_response()` / `_status()` | Every HTML/WML page: epoch Content-Type, security headers, link re-tagging, WML pagination, Latin-1 transcoding (see [rendering.md](rendering.md#the-response-layer)) |
| `build_redirect_response()` | `302` with an epoch-appropriate tiny body linking to the target |
| `build_json_response()` / `_status()` | AJAX endpoints |
| `build_css_response()` | `/themes/<key>/styles_epoch3.css` |
| `send_simple()` | Plain-text errors on AJAX endpoints and last-resort fallback |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
