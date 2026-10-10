# Boat Rudder - Themes and Site Settings

Reference for the theme system **as implemented**: how a request picks a theme, how templates
are resolved, what an admin can customize per theme from `/dashboard/settings`, and how those
customizations reach each epoch. The design history lives in the plans
([theme-system-plan.md](../plans/theme-system-plan.md),
[site-settings-plan.md](../plans/site-settings-plan.md),
[site-personalization-plan.md](../plans/site-personalization-plan.md),
[theme-scoped-personalization-plan.md](../plans/theme-scoped-personalization-plan.md)); this
document is what to read to understand or change the code today.

Diagrams: [diagrams/theme-resolution.puml](../diagrams/theme-resolution.puml),
[diagrams/site-settings-components.puml](../diagrams/site-settings-components.puml)

---

## 1. Concepts

| Concept | What it is | Stored in |
|---|---|---|
| **Theme** | A directory `html/themes/<key>/` holding the visual parts of the site (layout, menu, banner, home sections, assets, epoch 3 CSS). Ships with `dark` and `light`. | Filesystem |
| **Shared templates** | Everything theme-agnostic (content blocks, dashboard, login, errors, entry pages…) | `html/templates/` |
| **Theme overrides** | Colors, per-epoch banner/footer/logo, home blog background images, logo font, a customization layer over each epoch 3 stylesheet | `themes` collection, one sparse document per customized theme |
| **Theme uploads** | The images those overrides use (banner, footer, logo, home blog) | `html/content/themes/<key>/` - site content, never the theme's own `assets/` |
| **Site settings** | Site name, which theme new visitors get | `site_settings` singleton |
| **Font library** | Uploaded font files usable by the epoch 3 logo | `fonts` collection + `html/assets/fonts/` ([fonts.md](fonts.md)) |

Themes are **discovered**, not registered: `list_theme_keys()` (`src/utils/theme_catalog.c`)
reads `html/themes/` and every directory is a theme. `theme_key_is_valid(key)` (the directory
exists) guards every place a key arrives from a cookie, query string or form.

---

## 2. Which theme renders a request

Resolved once per request by `request_theme_set(Cookie, ?theme=)` in `http_router.c` and stored
per thread (`__thread`), read everywhere via `request_theme()` - the same pattern as
`request_lang()`.

| Priority | Source | Set by |
|---|---|---|
| 1 | `?theme=<key>` query parameter, if valid | Theme links on epoch 1 (no cookies through redirects there); epoch 1/2 links are re-tagged with `?theme=` by the response layer so the choice survives navigation |
| 2 | `theme` cookie, if valid | `GET /theme/set?key=<key>&return=<path>` (1-year cookie) - the epoch 3 navbar drop-down and epoch 2's `/theme` page |
| 3 | `site_settings.active_theme`, if its directory exists | `POST /dashboard/settings/themes/<key>/activate` |
| 4 | `theme=` in `settings.conf` | [configuration.md](configuration.md) |

Visitor-facing controls (`menu.c`'s `theme_selector()`): shown only when at least two themes
exist; epoch 3 gets a navbar drop-down, epochs 1/2 a link to the `/theme` page
(`theme_page.c`), epochs 0 and −1 get no theme control at all.

---

## 3. Template resolution

Every template is loaded through `generate_url_theme(subpath_fmt, epoch)`
(`src/utils/generate_url_theme.c`):

1. `./html/themes/<request_theme()>/<subpath>` - if the file exists, the theme overrides it.
2. Otherwise `./html/templates/<subpath>` - the shared copy.

So a theme only contains the files it wants to look different. Today both shipped themes override
the same set (`layout/`, `menu/`, `mainbanner/`, `home-blog/`, `home-content/`,
`category-menu/`, `page/page-home_*`, `styles_epoch3.css`); everything else is shared. The
complete inventory is in [templates-catalog.md](templates-catalog.md).

Paths are relative to the **working directory**, not the static root passed on the command line.

---

## 4. Colors

### 4.1 The palette

`CmsThemeColors` (`src/db/cms_themes.h`) holds **40 tokens**, one shared palette for every epoch
that has a notion of color (1, 2, 3). Stored under `themes.colors` with hyphenated names
(`navbar-background`, …), which are also the admin form's field names and, prefixed with
`--br-color-`, the epoch 3 CSS custom properties.

| Group | Tokens | Notes |
|---|---|---|
| Navbar | `navbar-background`ᵅ, `navbar-menu-normal`, `navbar-menu-hover`, `navbar-menu-active`, `navbar-logo` | |
| Page | `body-background`ᵅ, `body-background-epoch1`, `page-content-background`ᵅ, `home-content-background`ᵅ, `home-content-text` | `body-background-epoch1` is the one per-epoch exception: epoch 1's `<body bgcolor>`, normally one of the 16 VGA-safe colors so indexed-color displays don't dither it. `page-content-background` is the background of `.boat-rudder-page-content` (login, dashboard, error, language and theme pages) and `.boat-rudder-page-entry` (blog posts and pages), epoch 3 only, transparent by default |
| Admin (in the panel's *Body* group) | `body-text`, `body-text-muted`, `body-surface`, `body-surface-raised`, `body-border` | Epoch 3 login and dashboard only: text, secondary text (labels, hints), panel and card background, field/popover/menu background, borders. Read by `styles_admin_epoch3.css` through its `--br-admin-*` tokens; the admin's accent is `navbar-menu-active`. See [§6.1](#61-admin-tokens-and-components) |
| Blog list | `blog-list-item-background`ᵅ, `-border`, `-author`, `-categories`, `-categories-hover`, `-date` | Also used for bylines and category tags |
| Footer | `footer-logo`, `footer-logo-background`ᵅ | Epoch 3 only |
| Links | `link-normal`, `link-hover`, `link-visited`, `link-active` | In-content links, independent of the menu colors. `link-hover` applies to epochs 2/3 only |
| Table block | `table-header`, `table-border`, `table-row-a`, `table-row-b` | Plain `#rrggbb` - epoch 2 uses them as `bgcolor` |
| Code block | `code-background`, `code-text`, `code-keyword`, `code-string`, `code-comment`, `code-number`, `code-variable`, `code-tag`, `code-line-number` | Syntax highlighting palette ([code-highlighting.md](code-highlighting.md)). Defaults: CGA/Turbo C on navy |

ᵅ The six `*-background` tokens accept `#rrggbbaa`. The admin form shows a color picker plus an
opacity slider (`*-alpha` field, 0-100) joined by `cms_join_hex_alpha()`; alpha only has an
effect on epoch 3 (CSS), since epoch 1/2 `bgcolor` attributes have no alpha.

**Defaults.** A theme with no `themes` document (or a missing value) uses `THEME_DEFAULTS` in
`cms_themes.c` (Figma "Color palette Dark"/"Light"); a theme key without an entry there falls
back to `dark`'s palette.

### 4.2 How colors reach each epoch

| Epoch | Mechanism | Where |
|---|---|---|
| 3 | `{{THEME_COLORS}}` in `layout_epoch3.html` → inline `<style>:root{--br-color-…}</style>` plus the logo font's `@font-face`. Every rule in `styles_epoch3.css` (and the few admin rules that follow the site palette) reads `var(--br-color-x, <default>)` | `splice_theme_colors()` in `page_layout.c` |
| 2 | `{{COLOR_*}}` markers in the layout → plain hex in `<body>` attributes and a small CSS1 block (`a:hover`, menu item ids, category tag id) | `splice_retro_colors()` |
| 1 | Same markers, but only HTML attributes (`<body bgcolor text link vlink alink>`, `<font color>`) - no CSS | `splice_retro_colors()`, `menu.c`, `blog_list.c`, `category_tags.c`, `entry_page.c` |
| 0, −1 | No color model | - |

Several modules also read the palette directly for per-block colors on epochs 1/2 (paragraph
text, byline, table rows, code tokens) via `cms_get_theme_colors(request_theme(), …)`.

---

## 5. Banner, footer, logo and home blog (per theme, per epoch)

All of it is edited from the theme customizer ([§8](#8-theme-customizer-site-name-and-preview));
the single-purpose pages below still exist and post to the same endpoints.

| Part | Admin page | Storage | Fallback when unset |
|---|---|---|---|
| Home banner | `/dashboard/settings/themes/<key>/banner` | `themes.banner_html.<epoch>` (raw markup) | `mainbanner/mainbanner_epoch<N>.html` of that theme |
| Footer | `…/<key>/footer` | `themes.footer_html.<epoch>` | `layout/footer_epoch<N>.html` |
| Logo | `…/<key>/logo` | `themes.logo.<epoch>` (structured `CmsLogoConfig`) | legacy `themes.logo_html.<epoch>`, then `menu/menu-logo_epoch<N>.html` |
| Home blog backgrounds | customizer only | `themes.home_blog_background.<epoch>`, `themes.home_blog_item_background.<epoch>` (epoch 2/3, filenames) | the theme's own (CSS rule / `<table background>`) |

Banner and footer are one code editor per epoch (−1…3) plus the epoch's uploaded images (§7).
An empty value restores the on-disk file. Raw markup is inserted with `str_replace_*`, never
through `printf`, so `%` in CSS is safe. The epoch 3 footer's `{{SITE_NAME}}` token becomes the
site name.

### Logo modes (`CmsLogoConfig`)

| Epoch | Allowed mode (enforced server-side) | Rendering |
|---|---|---|
| −1 (WML) | image only | navbar/footer images are **WBMP**: the upload is a PNG and the server writes its WBMP twin (see §7); the stored name is the `.wbmp` |
| 0 | text only | `text` (or the site name) as plain text in the nav bar and footer |
| 1, 2 | image only | `navbar_image` / `footer_image` from `html/content/themes/<key>/logo/epoch<N>/`, rendered with `border="0"` |
| 3 | text **or** image (radio) | Text: `text` (or site name) in `font` = `system:<name>` (Arial, Times New Roman, Courier New, Georgia, Verdana, Comic Sans MS, Impact, Trebuchet MS) or `uploaded:<name>` from the font library. Image: same fields as epochs 1/2 |

The image `<img>` is built in C (`menu.c` for the navbar, `splice_footer_logo()` for the footer),
not from a template: `/content/themes/<key>/logo/epoch<N>/<file>`, self-closed (`/>`) on WML -
an unclosed `<img>` makes the deck invalid XML - and with `border="0"` on epochs 1/2, whose
browsers otherwise draw a link border around it.

On the home page the banner stands in for the logo, so the menu omits it there. The footer logo
uses the `{{FOOTER_LOGO}}` marker (`splice_footer_logo()`), falling back to the theme's
`layout/footer-logo_epoch<N>.html`.

### Home blog background images

Two images for epoch 2 and 3, the list's and each item's (drawn over the "Blog list item"
background color), both uploaded to `content/themes/<key>/home-blog/epoch<N>/` and saved by
`POST /dashboard/settings/themes/<key>/home-blog/<epoch>` (fields `background`,
`item-background`; `""` = the theme's own). The route sanitizes them with the upload's filename
rules, since they are written unescaped:

| Epoch | List | Each item |
|---|---|---|
| 3 | `.boat-rudder-home-blog{background-image:url(…)}` | `.boat-rudder-home-blog-item{background-image:url(…)}` |
| 2 | the first `<table>`'s `background="…"` of `home-blog_epoch2.html` | the first `<table>`'s `background="…"` of `home-blog-item_epoch2.html` |

Epoch 3's rules are appended to the `{{THEME_COLORS}}` block (`splice_theme_colors()`), after the
stylesheet, so they win. Epoch 2's attribute is swapped - or added when the template has none -
by `set_table_background()` in `blog_list.c`; the theme's template keeps its own default, so an
updated template, or a theme without the setting, renders as shipped.

### Epoch 3 logo font

Two controls, in priority order:

1. The Logo panel's epoch 3 `font` when its mode is **text**.
2. Otherwise the Colors panel's **Logo font** picker (`themes.logo_font`, an uploaded font name).
3. Otherwise **Milonga**, hardcoded in `styles_epoch3.css` (`var(--br-font-navbar-logo, Milonga)`).

`build_logo_font_css()` emits `@font-face{src:url('/assets/fonts/<file>') format('<hint>')}` for
uploaded fonts and only the CSS variable for system fonts.

---

## 6. Editable epoch 3 stylesheets

Each theme ships **two** epoch 3 stylesheets:

| File | Contents | Loaded on | Override |
|---|---|---|---|
| `styles_epoch3.css` | The public site: reset, navbar, banner, home, blog list, entry page and every content block, footer, language/theme pages, lightbox | Every epoch 3 page, linked by `layout_epoch3.html` | `themes.css_custom_epoch3` |
| `styles_admin_epoch3.css` | Login, dashboard, entry editor, media library, settings, analytics and charts | `/login` and `/dashboard*` only, **after** the public one | `themes.css_admin_custom_epoch3` |

Admin pages load both: they use the public navbar and footer, and the entry editor previews the
public block markup. The admin `<link>` isn't in the layout: `page_layout_wrap()`
(`splice_admin_styles()`) adds it before `</head>` when the request path is `/login`,
`/dashboard` or below. It decides by path, not by session, so a signed-in admin browsing the
public site doesn't download the admin rules, and a theme's layout needs no marker for it.

Both are **served dynamically**: `GET /themes/<key>/styles_epoch3.css` and
`GET /themes/<key>/styles_admin_epoch3.css` are intercepted by `match_theme_css_url()` before the
static file server and answer `cms_get_theme_css(key, sheet)`:

1. the theme's file on disk - for the admin stylesheet, if the theme ships none, the admin
   stylesheet of `configs/settings.conf`'s `theme` (`cms_get_theme_css_original()`), so a new
   theme with only public CSS still gets a styled dashboard;
2. then, if the admin saved one, a `/* ---- Customizations (dashboard) ---- */` comment and the
   theme's **customization layer** (`css_custom_epoch3` / `css_admin_custom_epoch3`).

The layer is *added*, never a replacement: appended last, a customized rule wins over the
original's at equal specificity, so it holds only what changed and every update of the shipped
file still reaches the site. The consequences:

- A customization can override a rule (same or a more specific selector; `!important` only where
  the original uses it; `revert`/`unset` to cancel a property) but not delete one. If an update
  renames a class, a rule aimed at the old name silently stops applying.
- The `css_epoch3` / `css_admin_epoch3` fields of earlier versions (full replacements) are no
  longer read.
- Colors are still injected through `{{THEME_COLORS}}`, so the original keeps reacting to the
  color panel.

The public layer is edited in the customizer's CSS drawer (§8); both have a page at
`/dashboard/settings/themes/<key>/css` and `…/admin-css` (a tab each):

- **Your changes**: the layer only, empty for a fresh theme, in the code editor (§6.2), 128 KiB
  limit (`THEME_ASSET_HTML_MAX`). `POST …/css` or `…/admin-css` saves it.
- **Original stylesheet**: the theme's file, read-only, folded below, to find and copy rules from.
- *Discard my changes* (`POST …/restore`) saves an empty layer: the plain file again.
- A broken public layer doesn't take the dashboard down with it: the editor itself is styled by
  the admin stylesheet.

### 6.1 Admin tokens and components

`styles_admin_epoch3.css` has three parts; the rules are the same in every theme that ships
today, only part 1 differs.

1. **Tokens** - a `:root` block with every color, radius and size the admin uses. No rule outside
   it carries a literal color (`scripts/check_css_bem.py` enforces it).

   | Token | Value |
   |---|---|
   | `--br-admin-text`, `-text-muted`, `-surface`, `-surface-raised`, `-border`, `-hover` | `var(--br-color-body-*)` - the palette's *Body* admin colors |
   | `--br-admin-accent` / `-on-accent` | `var(--br-color-navbar-menu-active)` / the panel background: primary buttons, focus, active state, switches, tabs, chart accents. One accent for the whole admin |
   | `--br-admin-success`, `-danger`, `-on-danger` | Fixed per theme: saved/published/upload done; delete and errors |
   | `--br-admin-overlay`, `-hover-overlay`, `-shadow`, `-thumb-filter` | Effects, fixed per theme |
   | `--br-admin-series-1…5`, `--br-admin-block-<type>` | Categorical palettes, fixed per theme: one color per epoch (charts, stat cards) and per block type (editor labels) |
   | `--br-admin-radius(-lg)`, `-font-xs/sm/md`, `-pad-sm/md/card` | Shape and type scale |

2. **Components** - generic BEM blocks, mixed with a page's own element class:

   | Block | Modifiers / elements | Replaces |
   |---|---|---|
   | `boat-rudder-btn` | `--primary`, `--danger`, `--ghost`, `--sm`, `--icon`, `--active` | `dashboard__button*`, the login button and ~20 editor, media and analytics buttons |
   | `boat-rudder-input` | `--sm` | `dashboard__input`, the login fields and every editor field (bare `<input>`s inside `__meta-group` and block `<textarea>`s share the rule) |
   | `boat-rudder-card` | | Login/dashboard forms, option groups, stat cards, report blocks, period bar, move picker |
   | `boat-rudder-caption` | | The small uppercase headings (option groups, meta sections, report blocks, stat labels) |
   | `boat-rudder-badge` | `--accent` | "Draft", "Page"/"Blog", "Theme: …" |
   | `boat-rudder-alert` | `--error` | `login__error`, `dashboard__error` |
   | `boat-rudder-empty` | | "No data", "No entries", "No other directories" |
   | `boat-rudder-switch` | `__knob`, `--on` | The editor's publish and autosave switches |
   | `boat-rudder-tabs` | `__tab`, `__tab--active` | The CSS editor's stylesheet tabs |

3. **Pages** - login, dashboard, entry editor, media library, analytics: layout only.

Entry previews and the rich-text editor show the entry as the public site does, so they read the
public palette directly (`home-content-text`, `link-normal`, `table-*`, `code-*`).

### 6.2 Code editor

Every raw-code field of the theme settings - the CSS layers, the banner and footer markup - is a
plain `<textarea data-code-editor="css|html|javascript">` (optional `data-indent`, default 2
spaces) that `html/assets/js/code-editor.js` turns into a code editor: line numbers, syntax
colors, a current-line band and a status bar. No library and no server code: the textarea stays
the form field, made transparent over a colored copy of its text, so forms post as before and
undo, selection and find are the browser's own.

- Colors: the same `boat-rudder-code-token--*` classes as `src/utils/code_highlight.c` (its rules
  are mirrored line by line, plus `<style>`/`<script>` inside HTML), so the theme's code colors
  apply. Its layout rules are the "Code editor" component of each `styles_admin_epoch3.css`; a
  sheet without them lacks `--br-code-editor-ready` and the plain textarea stays.
- Keys: Tab / Shift+Tab indent and outdent (the selected lines, if any); Esc then Tab leaves;
  Enter keeps the indentation, one level more after `{ ( [` or an opening tag; `}` on a blank
  indented line outdents it; Ctrl+S submits the form (`requestSubmit()`, so `csrf.js` adds the
  token); Ctrl+/ toggles comments; Ctrl+G goes to a line. Brackets and quotes are never
  auto-closed.
- A `readonly` textarea gets the same view with only Ctrl+G.
- Unsaved changes are flagged and leaving asks first. A page that saves the form by fetch
  cancels the submit and fires `code-editor:saved` on the textarea afterwards.
- Only the edited lines are re-tokenized, plus the following ones while the state carried across
  lines (open comment, string, tag) differs; past 20,000 lines it falls back to plain text.

---

## 7. Theme uploads and WBMP conversion

The banner, footer, logo and home blog editors upload images through
`/dashboard/api/theme-assets/*` ([routes.md](routes.md#theme-assets)) into
`html/content/themes/<key>/<component>/epoch<N>/`, served as `/content/themes/...`, where
`component` ∈ `banner`, `footer`, `logo`, `home-blog`. That is site content, like the media
library's `html/content/posts/`: the theme's own `assets/` ship with Boat Rudder and an update
overwrites them, so uploads never go there, and the widgets neither list nor delete the theme's
images. The theme key always comes from the request, never from the admin's own active theme, so
editing one theme can't write into another.

- Allowed uploads: `.png .jpg .jpeg .gif`; for epoch −1, `.png` only.
- Filenames are sanitized to `[A-Za-z0-9._-]` (spaces → `-`), no leading `.`, no `..`.
- **Epoch −1, every component**: the PNG is kept - the source for later edits, and what the
  customizer's WAP preview shows, since browsers can't read WBMP - and its WBMP twin with the same
  base name (`logo.png` → `logo.wbmp`) is written next to it by
  `image_convert_to_wbmp(path, out, 200)` (`src/utils/image_convert/`): decoded with the vendored
  `stb_image.h`, transparency laid over white (a WAP screen's background), scaled down
  nearest-neighbor to fit 200×200 (never upscaled), grayscale, flat 50 % threshold to 1-bit (no
  dithering), written by `write_wbmp()` (`src/utils/wbmp_writer.c`, shared with the QR
  generator). The `.wbmp` name is returned to the form; deleting either file removes both.

The widgets (`html/assets/js/theme-assets.js`) show uploads as a file list - thumbnail, name,
*Copy URL* (the path to paste into markup; on epoch −1 the `.wbmp`), *Delete*. Banner and footer
list their directory; single-image fields (logo navbar/footer, home blog backgrounds) show their
current file, and their *Delete* removes it, empties every field of the panel that used it and
saves the form at once, so a setting never points at a missing file.

---

## 8. Theme customizer, site name and preview

### 8.1 Theme customizer

`/dashboard/settings/themes/<key>/customize` (`site_settings_customize_page()`,
`settings-customize_epoch3.html`, driven by `html/assets/js/theme-customizer.js`) is the one page
for customizing a theme; the themes list links it as *Customize*, and the old per-theme page
(`/dashboard/settings/themes/<key>`) redirects to it.

- **Epoch first.** A −1…3 picker on top. The sidebar sections - Logo, Navbar, Banner, Content,
  Home blog, Footer, Data colors - hold every epoch's panel (`data-epoch`) and every color row
  (`data-epochs`, the epochs that read that color); only the chosen epoch's are shown, and a
  section with nothing for it says so. The sections are an animated accordion. Epoch, open
  section and drawer state are remembered per browser.
- **Colors by section.** The color form's groups carry `data-section` and are moved into their
  section - Navbar; Content (Body, Generic links, Home content); Home blog (blog list items);
  Footer - each with its own *Save colors*; the fields stay in the one form via `form=""`. Data
  colors keeps the table and code blocks. Each picker (`html/assets/js/color-field.js`) opens a
  popover fixed to the window, so the scrolling sidebar can't clip it.
- **Preview.** An iframe of the site with `?theme=<key>&preview_epoch=<N>` (§2, §8.2), kept on
  every link followed inside it, with screen-size presets. The site's own theme switcher is
  removed from it (it would switch themes, and `/theme/set` would change the admin's own cookie),
  and epoch 0 - which has no colors - gets the admin theme's. Opening a section, and every reload,
  scrolls the preview to that section's place (`data-scroll`: top, middle, bottom), the same in
  every epoch, and only inside the preview.
- **WAP preview.** Epoch −1 is WML, which no iframe renders: the deck is fetched and translated
  into a phone-sized screen - text, links, `do` soft keys, cards, and images through the PNG next
  to each WBMP. A deck that isn't well-formed XML is still shown, read by the HTML parser, under a
  warning that a real phone may reject it.
- **Saving.** Each panel keeps its own form and endpoint; the script posts it with fetch and
  reloads the preview. Sections with unsaved edits are marked, and leaving asks first.
- **Live, before saving (epoch 3).** A color change sets its `--br-color-*` variable inside the
  preview; the CSS drawer's text is injected there as a `<style>`.
- **CSS drawer.** The public stylesheet's customization layer and, beside it, the original
  read-only (§6). The admin stylesheet keeps its own page.

### 8.2 Site name and preview

- `/dashboard/settings` edits `site_settings.site_name` (default `"Boat Rudder"`). Every
  `{{SITE_NAME}}` in titles and templates is replaced, HTML-escaped, by `page_layout_wrap()`.
- `/dashboard/settings/preview` shows the public site in an iframe with an epoch picker and
  screen-size presets. It works because every route honors `?preview_epoch=N`
  (`resolve_epoch()`); the page's script re-appends the parameter to each same-origin link after
  every navigation. `preview_epoch` changes which templates render, **not** the charset
  decision (`request_charset` still follows the real User-Agent).

All settings routes are epoch 3 and **admin only**.

---

## 9. Creating a new theme

1. Copy an existing theme: `cp -r html/themes/dark html/themes/<key>` (`<key>` = a lowercase
   slug; it becomes the URL segment, the cookie value and the `themes.key`).
2. **Rewrite hardcoded paths.** Theme templates reference their own assets by absolute path,
   e.g. `layout_epoch3.html` links `/themes/dark/styles_epoch3.css` and banners use
   `/themes/dark/assets/...`. Replace `/themes/dark/` with `/themes/<key>/` throughout. (Images
   uploaded from the dashboard live in `html/content/themes/<key>/`, not in the theme.)
3. Keep the markers the code fills in: `{{CONTENT}}`, `{{PAGE_TITLE}}`, `{{THEME_COLORS}}`
   (epoch 3 layout), `{{COLOR_*}}` (epoch 1/2 layouts), `{{BODY_BACKGROUND}}`, `{{FOOTER}}`,
   `{{FOOTER_LOGO}}`, `{{LIGHTBOX}}`, `{{SITE_NAME}}`, and the `%s` slots of
   each fragment in the order its module passes them ([templates-catalog.md](templates-catalog.md)).
4. Delete any file you don't need to change - it will fall back to `html/templates/`. Files that
   exist only in themes today (layout, menu, banner, home sections, category menu, home page
   wrapper) have no shared copy, so a theme must provide them.
5. Optionally add default colors for `<key>` to `THEME_DEFAULTS` in `cms_themes.c`; without an
   entry, epochs 1/2 use `dark`'s palette until the theme is saved once from the dashboard.
6. `styles_admin_epoch3.css` is optional: copy it to restyle the dashboard for this theme, or
   leave it out to use the one of `configs/settings.conf`'s `theme`.
7. Restart is **not** needed: the theme appears in `/dashboard/settings/themes` and the visitor
   selector immediately.
7. Remove site-specific text (e.g. `alt="…"` naming a particular site) - see
   [style-guide.md](style-guide.md).

---

## 10. Source map

| File | Role |
|---|---|
| `src/utils/theme_catalog.c/h` | Discovery and validation of `html/themes/<key>/` |
| `src/utils/request_theme.c/h` | Per-request theme resolution |
| `src/utils/generate_url_theme.c/h` | Theme → shared template fallback |
| `src/db/cms_themes.c/h` | `themes` collection, defaults, hex/alpha helpers |
| `src/db/cms_site_settings.c/h` | `site_settings` singleton |
| `src/modules/site_settings_admin/` | All `/dashboard/settings*` pages, the theme customizer |
| `src/modules/theme_page/` | Public `/theme` page |
| `src/modules/menu/menu.c` | Navbar logo and theme selector |
| `src/html_builder/page_layout.c` | Color, font, footer logo, home blog (epoch 3) and site-name splicing |
| `src/modules/blog_list/blog_list.c` | Home blog backgrounds on epoch 2 (`set_table_background()`) |
| `src/utils/image_convert/`, `src/utils/wbmp_writer.c` | WBMP conversion |
| `html/templates/dashboard/settings/` | Admin templates |
| `html/assets/js/theme-customizer.js` | The customizer: epochs, sections, preview, WAP translation, fetch saves, live colors/CSS |
| `html/assets/js/theme-assets.js` | Upload widgets and file lists |
| `html/assets/js/color-field.js` | Color pickers (popover, VGA palette, opacity) |
| `html/assets/js/code-editor.js` | The code editor (§6.2) |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
