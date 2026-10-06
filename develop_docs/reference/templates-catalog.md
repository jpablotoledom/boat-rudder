# Boat Rudder - Templates Catalog

Inventory of every template under `html/templates/` (shared) and `html/themes/<theme>/`
(per theme): what it renders, which epochs have a variant, how many `printf` arguments it takes,
and which `{{MARKERS}}` it carries. How templates are resolved is in
[themes.md](themes.md#3-template-resolution); the rendering pipeline in
[rendering.md](rendering.md).

---

## Reading this catalog

- **File naming:** `<name>_epoch<N>.html`, `N` ∈ `-1 0 1 2 3`. A module asks for
  `"<dir>/<name>_epoch%d.html"` through `generate_url_theme()`; the theme's copy wins, otherwise
  the shared one. A missing file means "this epoch doesn't have that piece" - most callers render
  nothing or return `NULL` (→ `500` for a whole page).
- **Args:** the number of `printf` conversions (`%s`, `%d`, or positional `%N$s`). Fragments are
  filled by `render_template(tpl, …)` **in the order the module passes them**, so changing the
  count or order of `%s` in a template requires the matching change in C. Literal `%` must be
  written `%%`. When the count differs per epoch it's listed as a set (e.g. `2/5/6`): epochs
  ignore trailing arguments they don't use, but never consume more than are passed.
- **Markers:** `{{NAME}}` tokens replaced with `str_replace_*` after rendering (never through
  `printf`), so raw admin markup containing `%` is safe there.

| Marker | Filled by | Value |
|---|---|---|
| `{{CONTENT}}` | `page_layout_wrap()` | The page fragment |
| `{{PAGE_TITLE}}` | `page_layout_wrap()` | Escaped title (`{{SITE_NAME}}` inside resolved first) |
| `{{SITE_NAME}}` | `splice_site_name()`, `splice_footer()` | `site_settings.site_name`, escaped |
| `{{FOOTER}}` | `splice_footer()` | Theme footer (DB override or `layout/footer_epoch<N>.html`) |
| `{{FOOTER_LOGO}}` | `splice_footer_logo()` | Footer logo image/text or `layout/footer-logo_epoch<N>.html` |
| `{{LIGHTBOX}}`, `{{HOME-MODAL}}` | `splice_part()` | `layout/lightbox_epoch<N>.html`, `layout/home-modal_epoch<N>.html` (epoch 3 only) |
| `{{BODY_BACKGROUND}}` | `page_layout_wrap()` | Per-page body background argument |
| `{{THEME_COLORS}}` | `splice_theme_colors()` | Epoch 3 `<style>` with `--br-color-*` and logo font |
| `{{COLOR_*}}` | `splice_retro_colors()` | Epoch 1/2 hex colors (see [themes.md](themes.md#42-how-colors-reach-each-epoch)) |

---

## Per-theme templates (`html/themes/<theme>/`)

**These have no shared copy in `html/templates/`: a new theme must provide all of them.** Both
shipped themes (`dark`, `light`) contain the same set.

| Directory | Template | Epochs | Args | Markers | Used by |
|---|---|---|---|---|---|
| `layout/` | `layout` | −1…3 | 0 | `CONTENT PAGE_TITLE BODY_BACKGROUND THEME_COLORS(3) COLOR_*(1,2)` | `page_layout_wrap()` - the outer document |
| | `footer` | −1…3 | 0 | `FOOTER_LOGO SITE_NAME` | `splice_footer()` fallback |
| | `footer-logo` | 2 | 0 | `SITE_NAME` | `splice_footer_logo()` fallback |
| | `lightbox`, `home-modal` | 3 | 0 | | `splice_part()` |
| `menu/` | `menu` | −1…3 | 3/4/5 | | `menu()` container |
| | `menu-item`, `menu-item-selected` | −1…3 | 3/4 | | one nav item (selected = current section) |
| | `menu-item-separator` | −1…3 | 0 | | between items |
| | `menu-logo` | −1, 1, 2 | 0 | `SITE_NAME` | default logo when none configured |
| | `menu-lang` | −1…3 | 2 | | language control |
| | `menu-lang-item` | 3 | 4 | | epoch 3 drop-down entry |
| | `menu-theme` | 1, 2, 3 | 2 | | theme control (epoch 1/2: link to `/theme`) |
| | `menu-theme-item` | 3 | 4 | | epoch 3 drop-down entry |
| | `menu-user`, `menu-user-mobile` | 3 | 1 | | signed-in user's name |
| | `menu-compact` | −1 | 1 | | WML: single `[Menu]` link (arg: return path) |
| | `menu-page` | −1 | 1 | | `/menu` page body (arg: return path) |
| `mainbanner/` | `mainbanner` | −1…3 | 0 | `SITE_NAME` | home banner fallback (`cms_get_theme_banner()`) |
| `home-content/` | `home-content`, `home-content-item` | −1…3 | 1-2 / 3-4 | | home page "welcome" entry (`home_content.c`) |
| `home-blog/` | `home-blog`, `home-blog-item`, `empty` | −1…3 | 2-3 / 6-11 / 0-1 | | blog list - home section, `/blog`, category listing (`blog_list.c`) |
| `category-menu/` | `category-menu`, `-item`, `-item-selected`, `-separator` | −1…3 | 1-3 | | category bar on blog pages (`category_menu.c`) |
| | `category-menu-compact` | −1 | 1 | | WML: single `[Categories]` link |
| | `category-menu-page` | −1 | 2 | | `/blog/categories` page body |
| `page/` | `page-home` | −1…3 | 4 | `FOOTER HOME-MODAL` | home page wrapper (`buildHomeWebSite()`) |
| `.` | `styles_epoch3.css` | 3 | - | | epoch 3 stylesheet, served via `/themes/<key>/styles_epoch3.css` (DB-overridable) |
| `assets/` | `blog-list/`, `footer/`, `mainbanner/`, `menu/`, `social-networks/` | | | | images; `mainbanner|footer|menu/epoch<N>/` are also the theme-assets upload targets |

---

## Shared templates (`html/templates/`)

### Page wrappers and system pages

| Directory | Template | Epochs | Args | Markers | Used by |
|---|---|---|---|---|---|
| `page/` | `page` | −1…3 | 2-3 | `FOOTER LIGHTBOX` | `buildPageWebSite()` - generic page |
| | `page-entry` | 2, 3 | 2-3 | `FOOTER LIGHTBOX` | `buildEntryWebSiteAtUrl()` (falls back to `page` on other epochs) |
| | `page-blog` | 2, 3 | 2 | `FOOTER` | `buildBlogListWebSiteAtUrl()` |
| `entry/` | `entry-meta` | −1…3 | 3/5 | | entry header (title, author, date) |
| | `entry-categories` | −1…3 | 1 | | category tags under an entry |
| `error/` | `error` | −1…3 | 2 | | `error_content()` - every error page |
| `redirect/` | `redirect` | −1…3 | 2 | | body of `302` responses (link for clients that don't follow) |
| `login/` | `login` | −1…3 | 0-1 | | `/login` (epoch 3 form; other epochs "not available") |
| | `login-error` | 3 | 1 | | error line |
| `language/` | `language`, `language-item` | −1…3 | 2 / 3-4 | | `/language` page |
| `theme/` | `theme`, `theme-item` | 1, 2, 3 | 2 / 3-4 | | `/theme` page (no WML/epoch 0 variant) |

### Content blocks (`elements/`)

Rendered by `entry_page.c`; formats of each block's `text`/`extra_data` are in
[rendering.md](rendering.md#content-block-types).

| Directory | Template | Epochs | Args | Notes |
|---|---|---|---|---|
| `title/` | `title` | −1…3 | 1/3/4 | `title-h2` (1, 2) for second-level headings |
| `paragraph/` | `paragraph` | −1…3 | 1-2 | epoch 1/2 take a color, epoch 3 a modifier class |
| `byline/` | `byline` | −1…3 | 2/4 | epochs 1/2 take author/date colors |
| `image/` | `image` | −1…3 | 2/5/6 | epochs −1/0: caption + QR link |
| | `image-float` | 1, 2 | 5-6 | floated image (HTML `align`) |
| | `image-qr-page` | 0 | 4 | `/image-qr/<code>` page |
| `gallery/` | `gallery`, `gallery-row`, `gallery-item` | 1-3 | 1-3 | inline gallery block |
| | `gallery-item-hidden`, `gallery-item-more` | 3 | 2-3 | lightbox overflow |
| | `gallery-view-all`, `gallery-view-all-count` | 1, 2 | 1-2 | link to `/gallery/<id>` |
| | `gallery-view-link` | −1, 0 | 1 | link to `/gallery/<id>` |
| | `gallery-page` | −1…3 | 3 | `/gallery/<id>` shell (back href, back label, body) |
| | `gallery-page-item` | 3 | 2 | grid item |
| | `gallery-page-main`, `-thumb`, `-thumbstrip` | 1, 2 | 5 / 4 / 1 | paginated viewer |
| | `gallery-page-qr` | −1, 0 | 3 | QR to the gallery |
| `youtube-embed/` | `youtube-embed` | −1…3 | 1/3 | iframe (3), QR image (1, 2, −1), QR link (0) |
| | `youtube-qr-page` | 0 | 4 | `/youtube-qr/<id>` page |
| `code-text/` | `code-text` | −1…3 | 2/3/6/8 | see [code-highlighting.md](code-highlighting.md) |
| `table/` | `table` | 0-3 | 1-2 | epochs 0/1 wrap an ASCII-drawn table; WML has no template - rows are transposed to `label: value` lines |
| | `table-row`, `table-cell`, `table-header-cell` | 2, 3 | 1-3 | |
| `list/` | `list-container`, `list-item` | −1…3 | 1-2 / 1 | |
| `link/` | `link` | −1…3 | 2 | |
| `separator/` | `separator` | −1…3 | 0-1 | |
| `generic/` | `generic` | −1…3 | 0-1 | raw content |
| `social-networks/` | `social-networks` | −1…3 | 1/4 | |
| `category/` | `category`, `category-separator` | −1…3 | 2-3 / 0 | category tags (`category_tags.c`) |
| | `category-list` | −1 | 1 | WML tag list |

### Dashboard (`dashboard/`) - epoch 3 only

| Directory | Templates | Module |
|---|---|---|
| `.` | `dashboard` (all epochs; −1…2 are a bare welcome with no admin tools), `nav-admin`, `nav-author` (option groups in a `<details>` "Menu" dropdown, next to the **Log out** form in `dashboard`'s header) | `dashboard.c` |
| `entries/` | `list`, `list-row`, `list-row-delete` (the admin-only Delete form, one `%s`: the id), `list-empty` (one `%s`: the empty message, plus a "+ New entry" button), `list-draft` (the "Draft" badge) | `entries_admin.c` |
| `entries/` | `list` (the `/dashboard/entries` page) | `dashboard.c` |
| `entries/editor/` | `container`, `meta`, `header`, `header-lang-tab`, `lang-tab-button`, `category-option`, `blocks` | `entry_editor.c` ([entry-editor.md](entry-editor.md)) |
| `entries/editor/blocks/` | one per block type: `title paragraph byline image gallery separator link list youtube-embed code-text generic table social-networks lang-field` | `entry_editor_blocks.c` |
| `categories/`, `menu/`, `users/`, `languages/` | `list`, `list-row`, `list-empty`/`list-error`, `form`, `form-field`, `form-error`, `option`, `list-row-actions` | the matching `*_admin.c` |
| `media/` | `media`, `media-directory-container`, `media-directory`, `item-photo`, `media-modal` | `media_admin.c` ([media-admin.md](media-admin.md)) |
| `settings/` | `settings`, `settings-error`, `settings-themes`, `settings-themes-panel` (48 args), `settings-themes-activate`, `settings-asset`, `settings-asset-panel`, `settings-logo`, `settings-logo-panel_{radio,image-only,text-only}`, `settings-css`, `preview` | `site_settings_admin.c` ([themes.md](themes.md)) |
| `fonts/` | `list`, `list-row`, `list-error` | `fonts_admin.c` ([fonts.md](fonts.md)) |
| `analytics/` | `analytics` (28 args), `summary` (13 args, dashboard home: charts plus their folded tables) | `analytics_view.c` ([analytics.md](analytics.md)) |

---

## Checklist for a new or modified template

- Keep the argument count and order in sync with the C caller; `%%` for a literal percent.
- WML (`_epoch-1`): well-formed XML, self-closing `<br/>`, no HTML lists (the response layer
  strips them anyway), keep it small - each deck is paginated to 860 compiled bytes.
- Epochs 0/1: no CSS, no JavaScript; epoch 1 widths in pixels, not percentages.
- Use `{{SITE_NAME}}` instead of a literal site name, and no site-specific text or URLs.
- If the template is per-theme, add it to **every** theme directory.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
