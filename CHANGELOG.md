# Changelog

All notable changes to Boat Rudder. Dates are commit dates; commit hashes are given for
reference. The project has no tagged releases yet - everything below is the work leading up to
**v0.0.1** (the current branch).

Format inspired by [Keep a Changelog](https://keepachangelog.com/). Sections: *Added*,
*Changed*, *Fixed*, *Removed*, *Security*, *Docs*.

---

## v0.0.1

### 2026-10-06
### One page per theme

**Changed**
- `/dashboard/settings/themes` is now a list of the themes (name, *Active* or *Set active*,
  *Edit*); each theme's colors, logo font and editor links moved to its own page,
  `/dashboard/settings/themes/<key>`, so editing a theme no longer means scrolling past the
  others. Saving colors returns to that page; *Set active* returns to whichever page it was
  clicked on. The banner, footer, logo and CSS editors link back to their theme in the
  breadcrumb.

### Page content background

**Added**
- `page-content-background` theme color (with opacity), in the panel's *Body* group: the
  background of `.boat-rudder-page-content`, the wrapper of the login, dashboard, error and
  language/theme pages (epoch 3). Transparent by default, so nothing changes until it's set.
  40 theme colors in all.
- `.boat-rudder-page-entry` (blog posts and pages) uses the same background, with a
  `24px 32px` padding (`16px` on phones) so its text doesn't touch the edge of the box.

### Admin design tokens and components

**Added**
- Five theme colors for the admin, in the panel's *Body* group: text, secondary text, panel
  background, field background and borders (`body-text`, `body-text-muted`, `body-surface`,
  `body-surface-raised`, `body-border`) - 39 theme colors in all. Each groups what used to be a
  dozen near-identical grays per role.
- A `--br-admin-*` token block at the top of every `styles_admin_epoch3.css`: palette colors
  (the new *Body* ones, and `navbar-menu-active` as the single accent), fixed states (success,
  danger), effects, the categorical palettes (charts, editor block types) and a type/shape scale.
- Generic admin components: `boat-rudder-btn` (`--primary`, `--danger`, `--ghost`, `--sm`,
  `--icon`, `--active`), `-input`, `-card`, `-caption`, `-badge`, `-alert`, `-empty`, `-switch`,
  `-tabs`, mixed into the templates next to each element's own class.
- `scripts/check_css_bem.py` also fails on a literal color outside an admin sheet's `:root`.

**Changed**
- The admin's ~90 (dark) and ~100 (light) distinct colors are now tokens; outside `:root` no
  rule carries a literal color, and the rules are identical in both themes. The admin follows
  the theme palette and has one accent instead of three (violet dashboard, blue editor, green
  media library). Entry previews and the rich-text editor use the public palette, as the site
  shows the entry.
- Buttons, fields, cards, badges, alerts, switches and tabs look the same on every admin page
  (26 button styles with 16 paddings and 6 font sizes before).

**Removed**
- Dead CSS: the editor's per-block language tabs, title input, toolbar select, image block
  preview, header tabs (`entry-editor__tabs`, `__tab-btn`, `__tab-content`) and the unused
  `switchHeaderTab()` script.

**Fixed**
- The HTML source editor's colors never applied (`.boat-rudder-entry-editor-block textarea` won
  on specificity); it now uses the code block's colors.
- Links styled as buttons took the public `a:link` color instead of the button's.

### Public and admin stylesheets per theme

**Changed**
- Each theme's epoch 3 CSS is split in two: `styles_epoch3.css` (public site, every page) and
  the new `styles_admin_epoch3.css` (login, dashboard, entry editor, media library, settings,
  analytics), linked only on `/login` and `/dashboard*`, after the public one. Public pages no
  longer download the ~55 KB of admin rules. Every rule kept its text and order; screenshots
  of every public and admin page are pixel-identical before and after.
- The CSS editor (`/dashboard/settings/themes/<key>/css`) has a tab per stylesheet; the admin one
  lives at `…/admin-css` and is stored in `themes.css_admin_epoch3`. A broken public override no
  longer breaks the dashboard.
- A theme without `styles_admin_epoch3.css` uses the one of `configs/settings.conf`'s `theme`.

**Removed**
- The epoch 3 home thumbnail modal (`layout/home-modal_epoch3.html`, `{{HOME-MODAL}}`, its CSS):
  nothing ever opened it.

**Fixed**
- Unescaped `%` in five templates rendered through printf (`home-content_epoch2.html`, the
  image block's size options, two editor scripts) - undefined behavior that ASan flagged.

### BEM CSS class names

**Changed**
- Every CSS class is now BEM with the `boat-rudder-` prefix
  (`boat-rudder-<block>__<element>--<modifier>`), replacing the `boat-rudder__block__element`
  namespace: 416 names renamed across the stylesheets, templates, C and JS. Elements nested two
  levels deep became blocks of their own (`boat-rudder__entry-editor__block__header` →
  `boat-rudder-entry-editor-block__header`), and `_` inside names became `-`.
- Bare state classes became modifiers: `active`, `open`, `on`, `selected`, `saved`, `error`,
  `done`, `disabled`, `drag-over`, `dragging`, `is-dragging`, `is-expanded`, `menu--open`,
  `lang--open`, `theme--open`, the editor's `type-*` labels, the navbar's `navbar__toggle`/`bar`
  and the gallery page's `gallery-page*`. The `.none` utility is gone: the media library's
  directory rename uses the `hidden` attribute.
- **Run `scripts/migrations/2026-10-06-bem-class-names.js`** on every site: saved theme CSS
  overrides and stored markup still use the old names.

**Removed**
- Class attributes from epoch −1/0/1/2 templates and the C that emitted them for those epochs
  (`br-*`, the epoch 2 menu, footer, home, page and list classes, the separator variant, the
  active language/theme marker). Those epochs have no stylesheet. Epoch 2 keeps
  `boat-rudder-paragraph`, which its inline `<style>` uses.

**Added**
- Section 15 of `style-guide.md`: CSS class naming rules.
- `scripts/check_css_bem.py`: fails on non-BEM classes, leftover `boat-rudder__` names, or class
  attributes on epochs without a stylesheet.

### Dashboard usability

**Added**
- Charts in the dashboard's analytics summary, drawn as inline SVG on the server (no JavaScript,
  no chart library - `analytics_charts.c`): visits per day over the last 7 days (columns, today
  highlighted), visits by epoch (donut with legend), and horizontal bars for the top blog
  articles, browsers and countries. Each block keeps its Today / 7 days table under a "Table"
  toggle.

**Changed**
- Each epoch has one fixed chart color in both themes, also used on the analytics report's
  epoch stat cards.
- Delete/Remove in entries, categories, users, menu, fonts and languages now asks for
  confirmation, and is styled as a destructive (red) button; create/save buttons are styled as
  primary.
- Entries tables: the title opens the editor (the public page moved to a **View** action),
  the type is a badge next to the title instead of a column, the summary is clamped to two
  lines, smaller thumbnails, and an empty table offers **+ New entry**.
- Dashboard tables use horizontal separators, a header row and row hover instead of full cell
  borders.
- Maintainer pages show a breadcrumb above the title instead of "Back to ..." links at the
  bottom; categories, users and menu put their "New ..." button next to the title.
- The dashboard home's option groups (Content / Site / Administration) are now a **Menu**
  dropdown in the header, next to **Log out** (moved there from an "Account" option group), and the
  analytics summary now comes before **Entries pending publication**.

**Fixed**
- An `author` was shown a Delete button on their entries that the server always refused; it is
  now only shown to admins.
- Light theme: dashboard forms had a near-black background behind dark labels.
- Menu list rows closed `</td>` before `</div>`.

### Dashboard security fixes

**Added**
- New dashboard home: option groups (Content / Site / Administration / Account, with a link to the
  media library that was missing), **Entries pending publication** (unpublished entries only, with
  **+ New entry** and **View all**) and, for admins, an analytics summary - visits today and over
  the last 7 days, per epoch, and the top 5 countries, browsers and blog articles.
- `GET /dashboard/entries`: every entry, published or not, with a **Draft** badge on unpublished
  ones.
- **Log out** button in the dashboard's option list under "Welcome back", for both roles (an
  `author` gets a list of its own, `dashboard/nav-author_epoch3.html`); it posts to `/logout`.

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
- Logout is `POST /logout` with the session's CSRF token (`GET /logout` now answers `405`), so a
  third-party page can no longer sign a visitor out.

**Fixed**
- Deleting/moving a media item now also handles the optimizer's original-name symlink
  (e.g. `photo.png -> photo_half.gif`), which was left behind.
- `media.author_username` is sanitized the same way as the upload directory name.

### WAP gateway, code highlighting, QR short links (`7c55aef`)

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
