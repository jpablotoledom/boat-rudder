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
| **Theme overrides** | Colors, per-epoch banner/footer/logo, logo font, a full epoch 3 CSS replacement | `themes` collection, one sparse document per customized theme |
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

`CmsThemeColors` (`src/db/cms_themes.h`) holds **34 tokens**, one shared palette for every epoch
that has a notion of color (1, 2, 3). Stored under `themes.colors` with hyphenated names
(`navbar-background`, …), which are also the admin form's field names and, prefixed with
`--br-color-`, the epoch 3 CSS custom properties.

| Group | Tokens | Notes |
|---|---|---|
| Navbar | `navbar-background`ᵅ, `navbar-menu-normal`, `navbar-menu-hover`, `navbar-menu-active`, `navbar-logo` | |
| Page | `body-background`ᵅ, `body-background-epoch1`, `home-content-background`ᵅ, `home-content-text` | `body-background-epoch1` is the one per-epoch exception: epoch 1's `<body bgcolor>`, normally one of the 16 VGA-safe colors so indexed-color displays don't dither it |
| Blog list | `blog-list-item-background`ᵅ, `-border`, `-author`, `-categories`, `-categories-hover`, `-date` | Also used for bylines and category tags |
| Footer | `footer-logo`, `footer-logo-background`ᵅ | Epoch 3 only |
| Links | `link-normal`, `link-hover`, `link-visited`, `link-active` | In-content links, independent of the menu colors. `link-hover` applies to epochs 2/3 only |
| Table block | `table-header`, `table-border`, `table-row-a`, `table-row-b` | Plain `#rrggbb` - epoch 2 uses them as `bgcolor` |
| Code block | `code-background`, `code-text`, `code-keyword`, `code-string`, `code-comment`, `code-number`, `code-variable`, `code-tag`, `code-line-number` | Syntax highlighting palette ([code-highlighting.md](code-highlighting.md)). Defaults: CGA/Turbo C on navy |

ᵅ The five `*-background` tokens accept `#rrggbbaa`. The admin form shows a color picker plus an
opacity slider (`*-alpha` field, 0-100) joined by `cms_join_hex_alpha()`; alpha only has an
effect on epoch 3 (CSS), since epoch 1/2 `bgcolor` attributes have no alpha.

**Defaults.** A theme with no `themes` document (or a missing value) uses `THEME_DEFAULTS` in
`cms_themes.c` (Figma "Color palette Dark"/"Light"); a theme key without an entry there falls
back to `dark`'s palette.

### 4.2 How colors reach each epoch

| Epoch | Mechanism | Where |
|---|---|---|
| 3 | `{{THEME_COLORS}}` in `layout_epoch3.html` → inline `<style>:root{--br-color-…}</style>` plus the logo font's `@font-face`. Every rule in `styles_epoch3.css` reads `var(--br-color-x, <default>)` | `splice_theme_colors()` in `page_layout.c` |
| 2 | `{{COLOR_*}}` markers in the layout → plain hex in `<body>` attributes and a small CSS1 block (`a:hover`, menu item ids, category tag id) | `splice_retro_colors()` |
| 1 | Same markers, but only HTML attributes (`<body bgcolor text link vlink alink>`, `<font color>`) - no CSS | `splice_retro_colors()`, `menu.c`, `blog_list.c`, `category_tags.c`, `entry_page.c` |
| 0, −1 | No color model | - |

Several modules also read the palette directly for per-block colors on epochs 1/2 (paragraph
text, byline, table rows, code tokens) via `cms_get_theme_colors(request_theme(), …)`.

---

## 5. Banner, footer and logo (per theme, per epoch)

| Part | Admin page | Storage | Fallback when unset |
|---|---|---|---|
| Home banner | `/dashboard/settings/themes/<key>/banner` | `themes.banner_html.<epoch>` (raw markup) | `mainbanner/mainbanner_epoch<N>.html` of that theme |
| Footer | `…/<key>/footer` | `themes.footer_html.<epoch>` | `layout/footer_epoch<N>.html` |
| Logo | `…/<key>/logo` | `themes.logo.<epoch>` (structured `CmsLogoConfig`) | legacy `themes.logo_html.<epoch>`, then `menu/menu-logo_epoch<N>.html` |

Banner and footer are one `<textarea>` per epoch (−1…3) plus an image upload/browse widget.
An empty value restores the on-disk file. Raw markup is inserted with `str_replace_*`, never
through `printf`, so `%` in CSS is safe. The epoch 3 footer's `{{SITE_NAME}}` token becomes the
site name.

### Logo modes (`CmsLogoConfig`)

| Epoch | Allowed mode (enforced server-side) | Rendering |
|---|---|---|
| −1 (WML) | image only | navbar/footer images must be **WBMP**: PNG/JPEG uploads are converted automatically (see §7) |
| 0 | text only | `text` (or the site name) as plain text in the nav bar and footer |
| 1, 2 | image only | `navbar_image` / `footer_image` from `html/themes/<key>/assets/menu/epoch<N>/` |
| 3 | text **or** image (radio) | Text: `text` (or site name) in `font` = `system:<name>` (Arial, Times New Roman, Courier New, Georgia, Verdana, Comic Sans MS, Impact, Trebuchet MS) or `uploaded:<name>` from the font library. Image: same fields as epochs 1/2 |

On the home page the banner stands in for the logo, so the menu omits it there. The footer logo
uses the `{{FOOTER_LOGO}}` marker (`splice_footer_logo()`), falling back to the theme's
`layout/footer-logo_epoch<N>.html`.

### Epoch 3 logo font

Two controls, in priority order:

1. The Logo panel's epoch 3 `font` when its mode is **text**.
2. Otherwise the Colors panel's **Logo font** picker (`themes.logo_font`, an uploaded font name).
3. Otherwise **Milonga**, hardcoded in `styles_epoch3.css` (`var(--br-font-navbar-logo, Milonga)`).

`build_logo_font_css()` emits `@font-face{src:url('/assets/fonts/<file>') format('<hint>')}` for
uploaded fonts and only the CSS variable for system fonts.

---

## 6. Editable epoch 3 stylesheet

Each theme's `styles_epoch3.css` is **served dynamically**: `GET /themes/<key>/styles_epoch3.css`
is intercepted by `match_theme_css_url()` before the static file server and answers
`cms_get_theme_css(key)` - the `themes.css_epoch3` override if non-empty, otherwise the file on
disk.

- `/dashboard/settings/themes/<key>/css` shows the *effective* CSS in a full-file editor
  (128 KiB limit, `THEME_ASSET_HTML_MAX`).
- `POST …/css` saves the override; `POST …/css/restore` clears it, so the shipped file is always
  the one-click "Restore original".
- Colors are still injected through `{{THEME_COLORS}}`, so an edited stylesheet keeps reacting to
  the color panel as long as it keeps its `var(--br-color-…)` references.

---

## 7. Theme assets and WBMP conversion

The banner/footer/logo editors upload images through `/dashboard/api/theme-assets/*`
([routes.md](routes.md#theme-assets)) into
`html/themes/<key>/assets/<component>/epoch<N>/`, where `component` ∈ `mainbanner`, `footer`,
`menu` (the logo). The theme key always comes from the request, never from the admin's own
active theme, so editing one theme can't write into another.

- Allowed uploads: `.png .jpg .jpeg .gif`; for epoch −1, `.png .jpg .jpeg` only.
- Filenames are sanitized to `[A-Za-z0-9._-]` (spaces → `-`), no leading `.`, no `..`.
- **Epoch −1 logo uploads** (`component=menu`, `epoch=-1`) are converted in place to WBMP by
  `image_convert_to_wbmp(path, out, 200)` (`src/utils/image_convert/`): decoded with the
  vendored `stb_image.h` (PNG/JPEG only), scaled down nearest-neighbor to fit 200×200 (never
  upscaled), grayscale, flat 50 % threshold to 1-bit (no dithering), written by
  `write_wbmp()` (`src/utils/wbmp_writer.c`, shared with the QR generator). The original is
  deleted and the `.wbmp` name is returned to the form.

---

## 8. Site name and preview

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
   `/themes/dark/assets/...`. Replace `/themes/dark/` with `/themes/<key>/` throughout.
3. Keep the markers the code fills in: `{{CONTENT}}`, `{{PAGE_TITLE}}`, `{{THEME_COLORS}}`
   (epoch 3 layout), `{{COLOR_*}}` (epoch 1/2 layouts), `{{BODY_BACKGROUND}}`, `{{FOOTER}}`,
   `{{FOOTER_LOGO}}`, `{{LIGHTBOX}}`, `{{HOME-MODAL}}`, `{{SITE_NAME}}`, and the `%s` slots of
   each fragment in the order its module passes them ([templates-catalog.md](templates-catalog.md)).
4. Delete any file you don't need to change - it will fall back to `html/templates/`. Files that
   exist only in themes today (layout, menu, banner, home sections, category menu, home page
   wrapper) have no shared copy, so a theme must provide them.
5. Optionally add default colors for `<key>` to `THEME_DEFAULTS` in `cms_themes.c`; without an
   entry, epochs 1/2 use `dark`'s palette until the theme is saved once from the dashboard.
6. Restart is **not** needed: the theme appears in `/dashboard/settings/themes` and the visitor
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
| `src/modules/site_settings_admin/` | All `/dashboard/settings*` pages |
| `src/modules/theme_page/` | Public `/theme` page |
| `src/modules/menu/menu.c` | Navbar logo and theme selector |
| `src/html_builder/page_layout.c` | Color, font, footer and site-name splicing |
| `src/utils/image_convert/`, `src/utils/wbmp_writer.c` | WBMP conversion |
| `html/templates/dashboard/settings/` | Admin templates |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
