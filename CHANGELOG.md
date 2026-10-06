# Changelog

All notable changes to Boat Rudder. Dates are commit dates; commit hashes are given for
reference. The project has no tagged releases yet - everything below is the work leading up to
**v0.0.1** (the current branch).

Format inspired by [Keep a Changelog](https://keepachangelog.com/). Sections: *Added*,
*Changed*, *Fixed*, *Removed*, *Security*, *Docs*.

---

## [Unreleased] - v0.0.1

### 2026-10-06 - Dashboard security fixes

**Security**
- Media directory rename/delete no longer build paths from client-supplied names: the path comes
  from the `media_directories` record and its owner, new names must match `[A-Za-z0-9_-]{3,60}`
  (previously `../` let any signed-in user rename directories outside `html/` as root).
  Non-empty directories can't be deleted, and the record is removed only after `rmdir()`.
- Media uploads accept only JPEG/PNG/GIF/WebP (extension and magic bytes); a file the optimizer
  can't process is deleted instead of being published.
- Media ownership: an `author` can only upload into, rename, delete and move their own
  directories and images; images move only between directories of their author.
- CSRF tokens on every dashboard `POST` (session-bound, checked in
  `require_dashboard_session()`, attached client-side by `/assets/js/csrf.js`).
- Expired sessions are purged: TTL and unique indexes on `sessions`, created at startup.

**Fixed**
- Deleting/moving a media item now also handles the optimizer's original-name symlink
  (e.g. `photo.png -> photo_half.gif`), which was left behind.
- `media.author_username` is sanitized the same way as the upload directory name.

### 2026-10-06 - WAP gateway, code highlighting, QR short links (`7c55aef`)

**Added**
- WAP 1.x gateway (`src/wap_gateway/`): UDP listener with Palm Rover WTP (49300) and
  connectionless WSP (9200) framings, WML → WBXML compiler, single-packet (860-byte) pages,
  per-IP rate limit; `wap_gateway_*` config keys. Requires the external `trc-wap-relay`.
- WML pagination for every epoch −1 response in the response layer (`?__page=N`,
  `?wml_pages=all`), compact `[Menu]` / `[Categories]` links with `/menu` and `/blog/categories`
  pages.
- Server-side syntax highlighting for `code-text` (8 languages, theme code palette, line
  numbers, `?code_lines=off`); 9 new `code_*` theme colors.
- Short links for QR codes (`short_links` collection, `/qr/<code>`), `/image-qr/<code>` and
  `/youtube-qr/<id>` epoch-0 pages, QR codes on the `/gallery/<id>` page for epochs −1/0, ASCII
  block QR for Cello.
- Per-request charset (`request_charset`): Latin-1 transcoding for Cello in epoch 0.
- Drafts: unpublished entries viewable by signed-in users (`[Draft]`, `no-store`).
- `POST /dashboard/api/block-preview`: server-rendered editor previews; table grid editor,
  paragraph split/merge.
- Media library: move and bulk delete, per-author `default` directory.
- `scripts/boat-rudder.logrotate`; `scripts/migrations/` with `2026-10-01-merge-image-paragraph.js`
  and `2026-10-03-merge-analytics.js`.
- `mongodb_manager_aggregate()` working around libmongoc 1.30.4-1+deb13u3.

**Changed**
- `image-paragraph` block merged into `image` (`float-left`/`float-right` alignment).
- `install` keeps the host's `settings.conf` (writes `settings.conf.dist`) and certificates,
  merges `html/`, installs logrotate and GeoLite2, restarts the service.

### 2026-09-17 - DDoS hardening and theme settings (`4fe591c`, `367c71e`)

**Added**
- Configurable anti-DDoS (`ddos_*`) and `connection_io_timeout_secs` (default 5 s); IP-table
  cleanup thread; `429` for rejected plain-HTTP connections; accept loop on its own thread.
- Structured per-epoch theme logos (`themes.logo`), PNG/JPEG → WBMP conversion for epoch −1
  (vendored `stb_image.h`), editable epoch 3 stylesheet with restore, background opacity.

**Changed**
- Blog-card theme images moved from `assets/home-content/` to `assets/blog-list/`.

**Security**
- IP table now mutex-protected (shared by accept and cleanup threads).

### 2026-09-11 - Analytics and GeoIP (`b4f3347`, `13dc9dc`, `fbbaa64`)

**Added**
- Analytics (ported from TRC): `page_visits_daily` / `entry_visits_daily` day buckets,
  `/dashboard/analytics` report, User-Agent parser.
- Optional GeoLite2 country detection (libmaxminddb, `HAVE_MAXMINDDB`).
- Font library (`/dashboard/settings/fonts`, `fonts` collection) for the epoch 3 logo.
- `body_background_epoch1` theme color.

### 2026-09-10 - Theme system (`45c20fc`, `e80bad8`, `d47508c`)

**Added**
- `html/templates/` (shared) vs `html/themes/<theme>/` split; second theme `light`;
  per-visitor theme selection (`?theme=`, cookie, `/theme`, `/theme/set`); per-theme
  colors, banner and footer; `site_settings.active_theme`.

**Fixed**
- Epoch 1/2 colors, image loading and theme navigation on old browsers.

### 2026-08-14 … 2026-08-28 - Retro epochs, preview, colors

- Scripts and docs clean-up (`4601197`); epoch 3 editor fixes and language selector
  (`65a4fb5`, `b4a9598`); epoch 2 styles and gallery fixes (`1f7fd55`, `5de3df6`); WML and
  epochs 0-2 normalization (`fc3cfa7`); dashboard epoch preview and signed-in user menu
  (`b85b009`); centralized color settings (`be8e8d5`).

### 2026-06-04 … 2026-07-01 - Rewrite: MongoDB CMS and dashboard

- Code rewrite (`d77d597`), epoch strategy, login with Argon2id sessions, MongoDB `entries`
  model, categories, menu, languages and users maintainers, entry editor, media library and
  galleries, blocks migrated from the legacy CMS.

### 2024-02-25 … 2026-02-12 - Origins

- `base-http-server` static server with TLS, modularization, Google-Sheets-backed prototype
  CMS (later replaced by MongoDB), first epoch templates, code highlighting prototype, logo.

---

For the narrative of what each feature does, see
[develop_docs/boat-rudder.md §8](develop_docs/boat-rudder.md#8-implemented-since-initial-release).
