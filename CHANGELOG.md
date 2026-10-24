# Changelog

All notable changes to Boat Rudder, newest first. Each `## vX.Y.Z` section is one release:
`scripts/release.sh` publishes it as that release's notes on GitHub, and the dashboard's
Updates page shows them. Changes not released yet go under `## Unreleased

## v0.0.7
### 2026-10-24

### Navbar: animated title on scroll

**Changed**
- On the home page (dark and light themes, epoch 3) the big site title over the banner now
  slides up and shrinks into the navbar as you scroll, while the menu glides to the right to
  make room for it; scrolling back to the top plays it in reverse. It used to jump between the
  two positions. Below 960px, where the menu lives behind the hamburger, the title stays centred
  and only moves up and shrinks.

**Fixed**
- In maintenance mode the navbar has no menu items, but the title was still pushed off-centre
  by the room reserved for them. It is now centred, both over the banner and in the bar.

## v0.0.6
### 2026-10-21

### Theme customizer: View images

**Added**
- A *View images* button in the theme customizer's sidebar, under *Data colors*, opens the media
  library in a modal - browse, view, upload, and *Copy URL* to paste an image into the banner or
  footer markup. Browse-only (no *Use selected*); closes with ×, Escape or a click outside.

### Permanent usernames

**Added**
- Every user has a permanent **username** (`users.username`): set when the account is created
  (3-32 characters of a-z, 0-9, dot, dash, underscore; unique) and read-only afterwards. It names
  the user's media folder, `html/content/posts/<username>/`. Login stays by email.

**Fixed**
- The media folder used to be derived from the email, so changing a user's email moved it: every
  thumbnail, *Copy URL* and new upload pointed at a folder that did not exist, and two emails
  with the same prefix shared one folder. Now the email can change freely.

**Upgrade**
- At startup, accounts without a username get one: the folder under `html/content/posts/` that
  already holds their media directories (so existing media URLs keep working), else the email's
  prefix, lowercased and made unique. The log names each one. A unique index is created on
  `users.username`. Nothing to run by hand: `boat-rudder-update` backs up the database and
  restarts the service, and the migration runs on that start (a rollback to an older version
  simply ignores the new field).
- One case needs a hand: two users whose emails shared a prefix (`juan@a.com`, `juan@b.com`)
  used to share one media folder. The older account keeps it; the other gets `juan-2`, and the
  log warns (`WARN: users: ... move them to html/content/posts/juan-2/ ... by hand`) that its
  directories are still in `posts/juan/`.

### Media library: Copy URL

**Added**
- A *Copy URL* button in the media library and the editor's picker copies the full URL - domain
  included - of each selected photo's `_full` variant, one per line. The domain is `public_url`
  (as for QR codes), else the address the dashboard was opened at. Disabled with nothing
  selected; works over plain HTTP too.

### Dashboard text in English

**Fixed**
- The user roles read *Administrador* / *Autor* / *Observador (solo lectura)* in the Users list
  and form, and several dashboard messages were in Spanish (user, language and site settings
  errors, the "new password" label), while the rest of the dashboard is in English. They now
  read *Administrator* / *Author* / *Viewer (read-only)*, and every message is in English.

### Media library: no "Use selected" outside the picker

**Fixed**
- `/dashboard/media` showed the *Use selected* button, which only does something in the entry
  editor's media picker (it hands the selection to the entry). It is now hidden on the media
  library page and kept in the picker.
- *Use selected*, *Move to…* and *Delete selected* are disabled while no photo is selected (in
  the library and the picker alike).

### Maintenance mode

**Added**
- A *Maintenance mode* panel on `/dashboard/settings`: pick a page entry (drafts included) and
  turn the mode on, and every visitor gets the home page - its navigation bar and banner kept,
  without the menu items or the blog list - with that page's content where the home content goes, on the home page, pages
  and blog (`/`, `/page/*`, `/blog*`, `/menu`) in every epoch. Only a signed-in Administrador
  sees the real site; `/login`, the dashboard and static files stay reachable. Stored as
  `site_settings.under_construction` / `under_construction_page`; served with
  `Cache-Control: no-store`.
- While it is on, only Administrators can sign in: any other role gets "The site is under
  maintenance: only administrators can sign in right now.", and a session of theirs already open
  is ended on its next dashboard request.

### Hide the home page's latest blog posts

**Added**
- A *Hide latest blog posts* checkbox on `/dashboard/settings` (*Home page* panel) removes the
  "Latest Blog Posts" list from `/` in every epoch, without running its query. Stored as
  `site_settings.hide_home_blog` (absent means shown); `/blog` is unaffected.

### HTTPS sends the whole certificate chain

**Fixed**
- `ssl_cert` was loaded with `SSL_CTX_use_certificate_file()`, which takes only the first
  certificate of the file: with Let's Encrypt's `fullchain.pem` the intermediate was never sent.
  Chrome fetches it by itself, so the site looked fine there, but curl, Android and other clients
  that don't refused the connection. Now the whole chain is loaded and sent
  (`SSL_CTX_use_certificate_chain_file()`), and a private key that doesn't match the certificate
  stops the server at startup instead of failing every handshake.

### Read-only role: Observador

**Added**
- A third role, **Observador** (`users.role: "viewer"`), for a public demo account: it opens
  every dashboard page an Administrador opens and sees every value - menus, analytics, users,
  settings, the entry editor - and can't change anything. Every dashboard write is a `POST`
  through `require_dashboard_session()`, which refuses it for a viewer (`403`; JSON under
  `/dashboard/api/`), so routes added later are covered too; the block preview and the
  browser-rules dry run, which store nothing, stay open. Fails closed when the role can't be
  read. Every dashboard page a viewer opens starts with a read-only notice
  (`dashboard/readonly-notice_epoch3.html`). Chosen in the Users form as "Observador (solo
  lectura)".

### Running behind a front

What a site needs to run behind a layer-4 front (HAProxy in `mode tcp`) - several sites on one
machine, each its own install that knows nothing of the others, as the separate **multi-br**
project sets them up. Nothing here is about several sites itself.

**Added**
- `listen_address`: the IPv4 address both listeners bind to (every address when empty).
- `proxy_protocol`: reads a PROXY protocol v1/v2 header before TLS and HTTP
  (`src/web_server/proxy_protocol.c`) and takes the client's address from it - for logging,
  analytics and the per-IP rate limit. Accepted only from `trusted_proxies`; a loopback
  connection without one (WAP gateway, updater health check) is served as direct; any other
  connection without one, or with a malformed one, is closed. Unit tests in
  `tests/proxy_protocol_test.c`.
- WAP gateway: `wap_upstream` fetches pages from another HTTP server (a front routing by
  `Host`), `wap_upstream_proxy_protocol` sends it the phone's address in a PROXY v1 header, and
  `boat-rudder --wap-only` runs the gateway alone - one gateway for every site on a machine.
- `install.sh --dir <path>`: installs the files into another directory, without a unit,
  logrotate rule or updater link, for a tool that runs the install itself. Refuses the default
  install's own directory, system directories and a non-empty directory holding no Boat Rudder
  install.
- Updater: `BR_UPDATE_UNIT` names the dashboard's transient update unit (so installs sharing a
  machine can update at the same time), `BR_SERVICE_LOG` the log its messages point to.
- `rundebug@<name>` runs with `./configs/<name>.conf`; `mongodb_dump.sh` and `mongodb_restore.sh`
  take a `settings.conf` path.

**Changed**
- The WAP gateway sends the requested URL's host as `Host` (was `127.0.0.1`), and an absolute
  redirect's host for the next hop. With `proxy_protocol=1` and its listener's address trusted
  it also sends a PROXY v1 header with the phone's address.
- The per-IP rate limit table holds IPv6 addresses too; with `proxy_protocol=1` a trusted peer
  (the front) is not counted at `accept()` - the visitor's address from the header is, on the
  connection thread.
- The updater's health check connects to `listen_address`.
- `rundebug` stops only a server running on the same config, not every `boat-rudder`.


### 2026-10-15

### Updates never delete a site's own files

**Fixed**
- An update deleted every file of the installed manifest the new version no longer ships - and
  a site installed from its own repository has its additions in that manifest too (uploaded
  fonts, its own theme files), so updating it would have deleted them. Now a file is deleted
  only if the installed version's official, signed package shipped it: the updater fetches that
  release and checks. The site's own files are kept, listed as *this site's own*, and leave the
  manifest; if the release can't be had, nothing is deleted.
- `uninstall.sh` left `/usr/local/sbin/boat-rudder-update` (the updater's link) and
  `/etc/logrotate.d/boat-rudder` behind; it removes both now.

### Build warnings

**Fixed**
- The Release build (what `install.sh` and the updater compile) printed two `-Wformat-truncation`
  warnings: in `tests/browser_detection_bson_test.c` (an error message, now capped to its
  buffer) and in `updates_admin.c` (a backup name longer than a version is now skipped instead of
  cut). Release and Debug builds are warning-free again.

## v0.0.5
### 2026-10-15

### Update system, phase 4: updates from the dashboard

**Added**
- `/dashboard/settings/updates` (admin): the installed version, whether a newer signed release
  exists, and every release's notes, newest first, marked *Installed*, *New*, *Pre-release* or
  *No signed package*. The list comes from the GitHub API (through `curl`), is kept for an hour,
  and *Check now* fetches it again.
- *Update to X* and *Roll back to X* on that page, each asking for the admin's password again.
  The server starts `boat-rudder-update run-request` as a transient systemd unit (`systemd-run`)
  and the page follows it live - stages, message, log - across the restart, then shows the
  result. Same safeguards as the console: signature, build before touching the site, backup,
  health check, automatic rollback.
- `boat-rudder-update run-request`, `.boat-rudder/progress.json` and `last-run.log`; `history`
  records who ran each update (`by`).
- Site settings shows the running version under its title, with a link to *Updates*, and an
  *Updates* card under *More settings*.
- `update_repo` in `settings.conf`: the GitHub repository of the releases, for the page and for
  `boat-rudder-update` (default `jpablotoledom/boat-rudder`).
- `src/utils/markdown_lite.c`: the Markdown subset of `CHANGELOG.md` to escaped HTML, for the
  release notes.


## v0.0.4
### 2026-10-14

### Update system, phase 1: versions and manifests

**Added**
- `VERSION` at the root of the source; `boat-rudder --version` prints it, and the startup log line
  names it.
- `scripts/manifest.sh`: the manifest of a version's system files (binary, `scripts/`,
  `html/templates`, `html/themes`, `html/assets`, the default favicons) with their SHA-256, in
  `sha256sum` format - `create` from a built source tree, `adopt` for an existing install (reports
  files edited on the host, missing, or not part of the version), `verify` an install against it.
  Never lists `html/content/`, `configs/`, `ssl/`, `data/` or the uploaded favicons and fonts.
- `install.sh` writes `.boat-rudder/VERSION` and `.boat-rudder/manifest.sha256` into the install,
  and installs `scripts/manifest.sh`. Groundwork for updates with rollback
  ([update-system-plan.md](develop_docs/plans/update-system-plan.md)).

### Update system, phases 2 and 3: signed releases and the updater

**Added**
- `scripts/release.sh <version>` (maintainer): packages `HEAD` with `git archive` as
  `dist/boat-rudder-<version>-src.tar.gz`, signs it with minisign, verifies the signature against
  `scripts/minisign.pub`, tags `v<version>` and, with `--publish`, creates the GitHub release with
  both files and the version's changelog section.
- `scripts/boat-rudder-update`, installed as `/usr/local/sbin/boat-rudder-update`: `status`,
  `check`, `install <version>|latest` or `install --file … --sig …`, `rollback [--restore-db]`,
  `history`. Verifies the signature against the installed key, extracts safely, compiles on the
  host, shows the files to replace/add/delete (and which were edited on the host), backs up those
  files and the database, applies, health-checks `/`, `/login` and the theme stylesheet, and rolls
  back by itself when the check fails. Only files in the installed or new manifest are touched.
- `install.sh` installs the updater and `.boat-rudder/minisign.pub`.

**Changed**
- `install.sh` copies the repository's `html/content/` (sample content) only on the first install;
  a reinstall no longer overwrites a live site's uploads with same-named samples.

### Media uploads on an installed server

**Fixed**
- `install.sh` didn't copy `scripts/image-optimizer.sh`, which the server runs on every media
  upload, so on an installed server every upload failed with `415`. It is installed now, and the
  installer warns when `magick`, `jpegoptim` or `gifsicle` is missing.
- A media upload rejected for its type is logged with its name and first bytes, which tells a
  mislabeled file (a WebP, AVIF or HEIC saved as `.jpg`) from a real problem.


## v0.0.3
### 2026-10-14
### Gallery block editing

**Changed**
- Uploaded file names: spaces and dots before the extension become `_`, and anything outside
  `[A-Za-z0-9_-]` is dropped (`Mi foto (1).jpg` → `Mi_foto_1.jpg`). A name already used in the
  directory gets `_2`, `_3`... (`photo.png` and `photo.jpg` count as the same name, since they'd
  share their variants).
- The gallery block shows its photos as numbered tiles: each can be moved with ← / → or dragged,
  and removed with ×. *Add photos* (header button and the last tile) appends the picked photos
  after the existing ones, skipping duplicates, instead of replacing the whole gallery.

**Fixed**
- Reordering a gallery's thumbnails was erratic after picking photos again: every re-render
  added another set of drag listeners, so one drag moved a thumbnail several times.
- Media library: deleting (or moving) photos while no directory was selected - how the picker
  opens, on the latest uploads - left them in the grid until it was reopened; the grid now
  reloads whatever it is showing.
- Media uploads: a file whose extension didn't match its content (a WebP saved as `.jpg`) was
  refused with `415`. The content now decides: any JPEG/PNG/GIF/WebP is accepted and stored under
  its real extension; anything else is still refused.
- An author's first upload with no directory selected failed with `500` when they had no folder
  under `html/content/posts/` yet: the default directory was created before its parent.
- The optimizer no longer leaves a base-name symlink to `_half` (`photo.jpg → photo_half.jpg`)
  next to the variants: it looked like an undeleted original, and nothing requests the bare
  name - every page asks for a variant. After an upload only `_full`, `_half`, `_small`,
  `_medium` and `_micro` remain. Deleting or moving an older upload still removes its link.
- Media uploads are now written with `O_CREAT | O_EXCL | O_NOFOLLOW`: an upload can never write
  through an existing base-name symlink (into another upload's `_half` variant) or over any
  existing file, even if the unique-name step were bypassed.
- Uploading a file whose name already existed in the directory overwrote the earlier upload's
  `_half` variant (written through its base-name symlink) and added a second media record for
  the same files, so deleting either removed both pictures.

## v0.0.2
### 2026-10-11
### Browser detection from the dashboard

**Added**
- `/dashboard/settings/browsers` (Site → Browser detection): the rules that pick each browser's
  epoch, editable without recompiling - ordered rules of *contains* / *excludes* tokens, the
  token a version follows, version tiers (epoch, dashboard access), a fallback epoch - plus a
  *Try a User-Agent* box and test cases that re-run on every edit and show what a change would
  move. Stored in the new `browser_detection` collection, held in memory (loaded at startup and
  on save). Until saved, the factory rules apply: the same chain as before, now as data in
  `detect_epoch.c`.
- Dashboard browser floor: `/login` and `/dashboard*` now need the `dashboard` flag the rules
  give the browser (factory: Chrome 111, Firefox 121, Safari / iOS 16.2 - the admin stylesheet's
  `:has()` and `color-mix()`), not just epoch 3. Read from the real `User-Agent`, so
  `?preview_epoch=3` can't lift an older browser in; other epoch 3 browsers see "needs a more
  recent browser".
- `tests/browser_detection_bson_test.c` (rules to/from BSON and the page's JSON).
- `develop_docs/reference/epochs.md`: each epoch's technical specification - protocol, encoding,
  markup, CSS and JS level, full-fidelity floor, the browsers sent to it - and what keeps it true.
- CSS compatibility check (`src/utils/css_compat.c`): ~20 CSS features with their first Chrome,
  Firefox and Safari versions (`:has()`, `color-mix()`, container queries, nesting, `dvh`, ...)
  checked against the detection rules by classifying the version just below each - so it works
  with whatever rules are saved. Shown on the theme CSS editor (with the active rules) and as a
  *Stylesheet check* on the Browser detection page (with the rules being edited, every theme,
  public and admin). `tests/css_compat_test.c` also keeps the shipped stylesheets within the
  factory floors.

**Changed**
- Genuine HTTP/0.9 clients (a version-less request with no headers: WorldWideWeb, the early Line
  Mode Browser, ViolaWWW) get pages as an RFC 1945 Simple-Response - no status line or headers,
  which they would print as the top of the page - in epoch 0 and Latin-1, instead of epoch 1 with
  HTTP/1.1 headers. Static files keep their headers (Cello's version-less image fetches need
  Content-Type).
- The CERN Line Mode Browser is recognized by its real User-Agent (`LineMode/2.15 libwww/2.17`,
  `W3CLineMode/5.4.0`), not only "Line Mode Browser": epoch 0 in Latin-1, was epoch 1.
- UP.Browser 3.x (HDML only, never WML) is no longer sent WML: excluded from the WAP rule.
- `styles_epoch3.css`: `-webkit-user-select` next to `user-select`, so code line numbers stay
  out of a copy on Safari too.

**Security**
- Saving detection rules that would cut the saving browser off the dashboard is refused (`409`).

### 2026-10-11
### Epoch detection by real capability

**Changed**
- `detect_epoch()` sends a browser to epoch 3 only from the versions that render its stylesheet:
  Chrome/Edge 87, Firefox 66, Safari 14.1, iOS 14.5 (flex `gap` and `inset`). Firefox 4-65,
  Chrome 10-86, older Safari and Android WebViews used to get epoch 3 and lose its layout and
  colors; they now get epoch 2. Also to epoch 2: EdgeHTML (it carries a `Chrome/` token),
  Konqueror and any other `Mozilla/5` string no engine claims (legacy crawler UAs included),
  MSIE 4 (was epoch 1) and Opera 5-12 under its own name (was epoch 1). iOS is decided by the
  OS version, since CriOS/FxiOS carry no `Chrome/`, `Firefox/` or `Version/` token. Analytics
  per-epoch counts shift accordingly from this date.

**Added**
- `tests/detect_epoch_test.c`, the project's first test (CTest): ~50 real `User-Agent`s, one per
  browser family and version boundary, pinned to their epoch.

### 2026-10-11
### Retro DOCTYPEs, charset and the epoch 2 blog item color

**Added**
- Customizer, Home blog, epoch 2: *Show the background color behind the image* checkbox
  (`themes.home_blog_item_color.epoch2`, shown by default). Epoch 2 has no alpha, so an item
  image with transparent areas always showed the color through them; unticked, the item
  `<table>` gets no `bgcolor` and the list's background shows instead.

**Fixed**
- Epoch 2 home and blog listing: the page backdrop was a constant in `orchestrator.c` - markup
  in C, always the dark theme's path, and a file that was never shipped (a 404 on every such
  page). It is now the theme's `layout/body-backdrop_epoch2.html`, requested by the fragments
  that want it with `{{BODY_BACKDROP}}`; `page_layout_wrap()` loses its `body_background`
  argument.
- Epoch 2 blog list items: the theme's "Blog list item" background color was never painted
  (the template carried it as a disabled `bgcolor-x`). The item `<table>` now has a real
  `bgcolor`, under the item image (the theme's own or an uploaded one), as on epoch 3.
- Epoch 1 gallery page (`gallery-page_epoch1.html`): dropped its `charset=UTF-8` meta, which
  contradicted the Latin-1 body every epoch 1 page is transcoded to (accents broke in any browser
  honoring it), and its HTML 2.0 DOCTYPE - no other epoch 1 page carries one, and the markup is
  HTML 3.2 (tables, `<font>`).

**Changed**
- Epoch 2 pages (`layout_epoch2.html` in both themes, `gallery-page_epoch2.html`,
  `redirect_epoch2.html`) declare HTML 4.01 Transitional, without a system identifier, instead
  of HTML 3.2 (or HTML5 on the redirect page): it is what the markup actually uses (`style`,
  `font face`, cell `bgcolor`), and browsers still render it in quirks mode, as before. The
  redirect page now declares its charset with `http-equiv` instead of HTML5's `<meta charset>`.

### 2026-10-08 … 2026-10-09
### Site favicons

**Added**
- `/dashboard/settings` uploads the site's favicons under the site name: one for epoch 2
  (`favicon2.ico`, must contain a 16x16 image) and one for epoch 3 (`favicon3.ico`, multi-size),
  saved in the site root. Pages then link them with a cache-buster; without them each epoch keeps
  its theme's own favicon. The page previews the current icon of each epoch, and *Use the
  theme's* removes an upload.

### Theme customizer

**Added**
- `/dashboard/settings/themes/<key>/customize`: one page to customize a theme. Pick an epoch
  (−1…3) first; the sidebar - Logo, Navbar, Banner, Content, Home blog, Footer, Data colors, an
  animated accordion - shows only what that epoch supports (epoch 1 gets its ~10 colors, not all
  40), beside a live preview of the site in that theme and epoch with screen-size presets.
  Every panel saves in place (fetch, same endpoints as before) and reloads the preview; on epoch
  3 color changes and CSS show in the preview before saving. Opening a section scrolls the
  preview to it (top, middle or bottom of the page, the same in every epoch). The site's own
  theme switcher is hidden in the preview, and epoch 0 pages get the admin theme's colors there.
- A WAP preview for epoch −1: the WML deck is fetched and translated into a phone-sized screen -
  text, links, soft keys, cards, and images through the PNG next to each WBMP. A deck that isn't
  well-formed XML is still shown, under a warning.
- A lightweight code editor for every raw-code field (theme CSS, banner and footer markup):
  line numbers, syntax colors from the theme's code palette, current line, Tab/Shift+Tab indent,
  smart Enter, Ctrl+S save, Ctrl+/ comment, Ctrl+G go to line, unsaved-changes warning. No
  library: `html/assets/js/code-editor.js` lays the textarea over a colored copy of its text.
  Brackets and quotes are never auto-closed.
- Home blog background images for epoch 2 and 3: one for the list and one for each item (over
  its background color). `themes.home_blog_background` / `home_blog_item_background`.
- Uploaded images show as a file list - thumbnail, name, *Copy URL*, *Delete* - on banner and
  footer panels and on the single-image fields (logo, home blog); deleting a field's image also
  clears and saves the field.

**Changed**
- An epoch 3 stylesheet is no longer replaced: the dashboard saves a **customization layer**
  appended after the theme's file (`css_custom_epoch3` / `css_admin_custom_epoch3`), so it holds
  only what changed and theme updates still reach the site. The CSS page shows the layer and,
  read-only, the original; *Restore original* is now *Discard my changes*.
- Theme uploads moved from the theme's `assets/` - overwritten by an update - to
  `html/content/themes/<key>/<banner|footer|logo|home-blog>/epoch<N>/`. The upload widgets no
  longer list or delete the images a theme ships.
- Epoch −1 uploads (every component, not just the logo) take a PNG only; the server keeps it and
  writes its WBMP twin (`logo.png` → `logo.wbmp`) for real devices.
- The color form is split by section in the customizer (Navbar; Content: body, links, home
  content; Home blog; Footer; Data colors: table and code blocks), each field shown only for the
  epochs that use it.
- `/dashboard/settings/themes/<key>` redirects to the customizer; the themes list links
  *Customize*. Saving colors and *Set active* return there.
- Banner/footer panels: *Save markup* under the editor, then an *Images* block with its own
  *Upload*.

**Fixed**
- The epoch −1 logo `<img>` was not self-closed, so `/menu` and every page with a footer logo
  were invalid WML.
- An epoch −1 upload was named `logo.png.wbmp` and its PNG deleted.
- Transparent pixels came out black in a WBMP; transparency is now laid over white.
- Epoch 1/2 logo images (navbar and footer) get `border="0"`, so old browsers draw no link border.
- The color picker's popover was clipped by scrolling containers; it is now fixed to the window.

**Removed**
- The `css_epoch3` / `css_admin_epoch3` fields (full replacements) are no longer read.
- The one-theme page (`settings-theme_epoch3.html`, `site_settings_theme_page()`), now the
  customizer.

**Docs**
- [themes.md](develop_docs/reference/themes.md): home blog backgrounds, the CSS layer, the code
  editor, theme uploads and WBMP twins, the customizer. Routes, dashboard, data model, templates
  catalog, security and README updated to match.

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
