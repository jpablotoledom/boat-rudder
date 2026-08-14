# Boat Rudder - Public Page Rendering

How every public route is turned into markup: the epoch system, the per-epoch template
convention, and one section per public page type. The server foundation underneath
(sockets, router, static files) is in [architecture.md](architecture.md); the admin side is in
[dashboard.md](dashboard.md).

---

## Retro-Compatible CMS (epoch-based rendering)

The `/` route is rendered dynamically instead of being served as a static file. The goal is to send each
client the simplest markup its browser can handle, from WAP-era phones to modern HTML5/CSS3 browsers.

### Epochs

`utils/detect_epoch.c` classifies the `User-Agent` header into one of five epochs:

| Epoch | Constant | Target | `Content-Type` |
|---|---|---|---|
| -1 | `EPOCH_WML` | WAP phones | `text/vnd.wap.wml` |
| 0 | `EPOCH_PRESTANDARD` | Text browsers (e.g. Lynx) | `text/html` |
| 1 | `EPOCH_EARLY` | Old browsers (table layouts, `<font>`) | `text/html` |
| 2 | `EPOCH_MIDDLE` | HTML4 + CSS1 era | `text/html; charset=UTF-8` |
| 3 | `EPOCH_MODERN` | Modern HTML5 + CSS3 | `text/html; charset=UTF-8` |

### Templates (`html/themes/<theme>/`)

**Convention: no HTML/markup embedded in C.** All HTML, WML and other markup lives in
template files under `html/themes/<theme>/`, loaded via `generate_url_theme()` +
`read_file_to_string()` and rendered with `render_template()` / `str_replace_first()`. C code
must not contain inline markup string literals (e.g. `"<p>...</p>"`); if a new piece of markup
is needed, add a template file instead. The one accepted exception is the catastrophic
fallback in `send_error_response()` (`http_router.c`), which sends the plain-text status line
via `send_simple()` when the template pipeline itself has failed and cannot be relied on.

Each visual component has one HTML template per epoch, named `<component>_epoch<N>.html`
(`<N>` ∈ {-1, 0, 1, 2, 3}), under `html/themes/<theme>/<component>/`:

- `container/` - page shell (`<head>`/`<body>` wrapper); contains `{{PAGE_TITLE}}` and 4 `%s`
  placeholders for menu, slider, home content and home blog.
- `menu/` - `menu_epoch<N>.html` (1 `%s`: items), `menu-item_epoch<N>.html` (3 `%s`: link, name, separator), `menu-item-selected_epoch<N>.html` (same 3 `%s`, adds `--selected` CSS modifier for the active nav item), `menu-item-separator_epoch<N>.html` (static). See "Menu" above.
- `slider/` - hero/banner block, static per epoch (no placeholders).
- `home-content/` - `home-content_epoch<N>.html` (1 `%s`: items) and
  `home-content-item_epoch<N>.html` (3 `%s`: title, date, text).
- `home-blog/` - `home-blog_epoch<N>.html` (1 `%s`: items) and `home-blog-item_epoch<N>.html`
  (epoch -1/0: 4 `%s` - title, date, summary, categories; epoch 1/2/3: 7 `%s` - image, link,
  title, summary, author, categories, date); `home-blog/empty_epoch<N>.html`, a static
  "No blog entries found" message rendered instead of `items` when there are no blog entries.
  Category tags reuse `elements/category/category_epoch<N>.html` items, concatenated without
  the `entry-categories` wrapper. See "Home blog list" below.
- `blog-list/` - `blog-list_epoch<N>.html` (1 `%s`: items) and `blog-list/empty_epoch<N>.html`
  (static "No blog entries found"), the `/blog` page wrapper and empty state. Items reuse
  `home-blog/home-blog-item_epoch<N>.html` (same placeholders/category rendering as the home
  blog list). See "Blog list" below.
- `page/` - `page_epoch<N>.html`, the generic page shell for non-home routes (`/login`,
  `/dashboard`, error pages); contains `{{PAGE_TITLE}}`, 1 `%s` for the menu and 1 `%s` for the
  page's content fragment. Two wider variants exist for epoch 2/3 content pages:
  `page-entry_epoch{2,3}.html` (article wrapper used by `buildEntryWebSiteAtUrl()`) and
  `page-blog_epoch3.html` (full-width listing wrapper used by `buildBlogListWebSiteAtUrl()`).
  Both add a third `%s` for the category menu, inserted right after the navbar.
- `category-menu/` - `category-menu_epoch<N>.html` (1 `%s`: items),
  `category-menu-item_epoch<N>.html` and `category-menu-item-selected_epoch<N>.html` (2 `%s`
  each - slug, name). The category bar rendered under the navbar on `/blog` and
  `/blog/category/<slug>`. See "Category menu" below.
- `login/` - `login_epoch<N>.html`. Epoch 3 has 1 `%s` for an optional error block; epochs
  -1/0/1/2 are static "login is not available on this device" content.
- `dashboard/` - `dashboard_epoch<N>.html`, static "Welcome to dashboard" content per epoch (no
  placeholders).
- `error/` - `error_epoch<N>.html`, 2 `%s` placeholders (status code, message).
- `redirect/` - `redirect_epoch<N>.html`, the standalone "click here to continue" body sent
  with `302` responses (for clients that don't auto-follow `Location`); 2 `%s` placeholders
  (both the redirect target: link `href` and link text). Not wrapped by `page_epoch<N>.html`.
- `entry/` - `entry-header_epoch<N>.html`, the header/SEO block for a CMS entry (epochs 1-3:
  5 `%s` - image, title, summary, author, date; epochs -1/0: 4 `%s` - title, summary, author,
  date); `entry-categories_epoch<N>.html`, the category "tags" wrapper (1 `%s` - concatenated
  rendered category items); `entry-meta_epoch3.html` (epoch 3 only, 2 `%s` - author name and
  the rendered categories block), which groups the author line and the category tags into one
  meta strip above the content. See "CMS entries" below.
- `elements/<type>/` - one subdirectory per content-block type. The `category/` element is the
  shared category "tag": from epoch 3 it is an `<a href>` (2 `%s` - link, name) pointing at
  `/blog/category/<slug>`; epochs -1..2 keep the plain 1 `%s` label. `gallery/` has additional
  variants: `gallery_epoch<N>.html` (container/wrapper for epochs -1 through 2), `gallery-container_epoch3.html` + `gallery-item_epoch3.html` + `gallery-item-more_epoch3.html` (CSS grid items for epoch 3), and `gallery-page_epoch<N>.html` + `gallery-page-item_epoch3.html` (standalone gallery page templates for the `/gallery/<id>` route). `list/` has `list-container_epoch3.html` / `list-container-ol_epoch3.html` / `list-item_epoch3.html`; `table/` has `table_epoch3.html` / `table-row_epoch3.html` / `table-cell_epoch3.html` / `table-header-cell_epoch3.html`. See "Content block types" below for the full list and the epochs each one covers.
- `dashboard/media/` - templates for the `/dashboard/media` admin page: `media_epoch3.html`, `media-directory-container_epoch3.html`, `media-directory_epoch3.html`, `item-photo_epoch3.html`, `media-modal_epoch3.html`.

`%s` placeholders are resolved with `printf`-family formatting, so any literal `%` in a template
that is itself used as a format string must be written as `%%`. Templates that are only ever
substituted *into* another template's `%s` (e.g. `slider`, `menu-item-separator`) are inserted
as-is and must use a single `%`.

### Build pipeline

```
http_router.c  (route == "/")
  │
  ├─ epoch = (force_epoch in -1..3) ? force_epoch : detect_epoch(User-Agent)
  │
  ├─ buildHomeWebSite(epoch, lang)        ── html_builder/orchestrator.c
  │     ├─ container(epoch)               ── modules/container
  │     ├─ menu("/", epoch)                ── modules/menu
  │     │     cms_get_menu_items(lang, &items, &count) ── src/db/cms_menu.c
  │     ├─ slider(epoch)                   ── modules/slider
  │     ├─ home_content(epoch, lang)       ── modules/home_content
  │     ├─ home_blog(epoch, lang)          ── modules/home_blog
  │     │     cms_get_blog_entries(lang, HOME_BLOG_LIMIT, &items, &count) ── src/db/cms_entries.c
  │     └─ render_template(container, menu, slider, home_content, home_blog)
  │
  └─ build_epoch_response(body, extra_headers, epoch) ── utils/build_epoch_response.c
        sets Content-Type per epoch, reuses SECURITY_HEADERS
```

Each module resolves its template path via `generate_url_theme("<subpath>_epoch%d.html", epoch)`,
which expands to `./html/themes/<theme>/<subpath>` (relative to the process working directory,
using the global `theme` from `config_loader`), reads it with `read_file_to_string()`, and renders
it with `render_template()`. The orchestrator frees every intermediate buffer on all paths.

For `HEAD /`, `http_router.c` builds the same response and truncates it at the end of the header
block (`\r\n\r\n`) before writing.

### CMS entries (`GET /page/<link>`, `GET /blog/<link>`)

The database-backed CMS described in `develop_docs/plans/cms-entry-model-plan.md`: a single
`entries` MongoDB collection (`ENTRIES_COLLECTION`, `src/db/mongodb_manager.h`) holds one
self-contained document per page - `header` (image, title, summary, `author_id`, date) plus an
ordered `content[]` array of typed blocks - with all user-facing text stored as a
`map<lang,string>` keyed by ISO 639-1 codes (`en`, `es`, ...).

```
http_router.c  (route == "/page/<link>" or "/blog/<link>")
  │
  ├─ serve_cms_entry(ctx, link, expected_type, lang, method, epoch, category_menu_html)
  │     │                                          ── src/web_server/http_router.c
  │     │  Takes ownership of category_menu_html (both routes currently pass NULL).
  │     │
  │     ├─ cms_get_entry_by_link(link, lang, &entry)  ── src/db/cms_entries.c
  │     │     db.entries.findOne({ link, enabled: true })
  │     │     resolves header.* and content[].text map<lang,string> to `lang`
  │     │     (an already-resolved ISO code from cms_resolve_default_lang()),
  │     │     falling back to "en" if the requested language is missing
  │     │     resolves header.author_id (ObjectId -> users.name) via
  │     │     cms_get_user_name_by_id() - the author is a reference, not translated text
  │     │     resolves entries.categories[] (ObjectId[]) -> entry_categories.name
  │     │     (ENTRY_CATEGORIES_COLLECTION) via db.entry_categories.find({_id: {$in: [...]}}),
  │     │     same lang resolution, plus a parallel category_links[] of
  │     │     "/blog/category/<slugify(name)>" URLs; no categories -> category_count == 0
  │     │
  │     ├─ entry_page(&entry, epoch)                   ── src/modules/entry_page/entry_page.c
  │     │     │  Note: the header (image, title, summary) is NOT rendered here -
  │     │     │  it is used only for the blog/home listing thumbnails.
  │     │     ├─ entry/entry-categories_epoch<N>.html   (category "tags", skipped if none)
  │     │     │     + elements/category/category_epoch<N>.html (one per category;
  │     │     │       epoch 3 renders <a href="/blog/category/<slug>">)
  │     │     ├─ entry/entry-meta_epoch3.html          (epoch 3 only: author + categories strip;
  │     │     │     older epochs fall back to the bare categories block)
  │     │     └─ elements/<type>/<type>_epoch<N>.html  (one per content[] block, in order)
  │     │           See "Content block types" below (14 types)
  │     │
  │     └─ buildEntryWebSiteAtUrl(epoch, entry.header_title, content, current_url, cat_menu)
  │           current_url = "/blog" for blog entries, "/page/<link>" for pages
  │           uses page/page-entry_epoch{2,3}.html, falling back to page_epoch<N>.html
```

`cms_get_entry_by_link()`'s query (`db.entries.findOne({ link, enabled: true })`) has no `type`
filter; `serve_cms_entry()` 404s unless `entries.type` matches the route's `expected_type` -
`"page"` for `/page/<link>`, `"blog"` for `/blog/<link>`. This keeps a single canonical URL per
entry: blog articles live at `/blog/<link>` (linked from the home blog list and `/blog`
listing), `/page/<link>` is for `type: "page"` only. Unknown `content[].type` values render as
empty output, so a page still renders if it contains a block type this increment doesn't
support.

#### Content block types

`render_block()` (`entry_page.c`) dispatches on `content[].type`. Every block carries a
translated `text` (`map<lang,string>`) and an untranslated `extra_data` string; each type
interprets those two fields differently:

| Type | `text` | `extra_data` | Epochs |
|---|---|---|---|
| `tittle` | heading text | heading level `1`-`6` | -1..3 |
| `paragraph` | rich text (stored as HTML) | unused | -1..3 |
| `byline` | author | date | -1..3 |
| `image` | image base URL | caption / alt text | -1..3 |
| `gallery` | `;`-separated image URLs | `media_galleries._id` | -1..3 |
| `separator` | unused | modifier passed to the template | 3 |
| `link` | link label | target URL (`#` if empty) | 3 |
| `list` | one item per line | `"ol"` for ordered, anything else = unordered | 3 |
| `table` | rows by line, cells by `\|` | `"header"` makes row 0 a header row | 3 |
| `code-text` | code body | language label | 3 |
| `youtube-embed` | unused | watch/short/embed URL, normalized to `/embed/<id>` | 3 |
| `image-paragraph` | HTML text with a floated image | `"left"` / `"right"` alignment | 3 |
| `social-networks` | display name (falls back to the icon name) | `"<icon>\|<url>"` | 3 |
| `generic` | raw HTML passthrough | unused | 3 |

The first five types have a template for every epoch. The nine added later are **epoch 3 only** -
`load_template()` returns `NULL` for the missing older-epoch files and the renderer yields `""`,
so an entry containing them still renders on a WAP phone or Lynx, just without those blocks. An
unknown `content[].type` renders as `""` for the same reason.

`social-networks` builds its icon path as
`/themes/dark/assets/social-networks/<icon>.svg` - note the theme segment is hardcoded, so this
block type does not follow the active `theme` setting.

`entries.categories[]` (an `ObjectId[]` referencing `entry_categories._id`, per
`plans/cms-entry-model-plan.md` §2.2) is resolved to category names and rendered as a small "tags"
block under the header. `entry_categories` documents are `{ _id, name: <map<lang,string>> }` -
a separate collection (kept normalized, since categories are shared across entries). An entry
with no `categories` field/empty array renders with no tags block. From epoch 3 each tag links
to `/blog/category/<slug>`, where the slug comes from `slugify(name)` in the current content
language (see "Category menu" below).

Image URLs stored in `header.image_url` and `content[].text` (for `image` blocks) follow the convention `_small`/`_half`/`_full` suffixes generated by `scripts/image-optimizer.sh`. `home_blog`, `blog_list` and `entries_admin` use `image_url_variant(url, "_small")` for thumbnails; `entry_page`'s gallery renderer uses `_small` and `_full`, and on epoch 1 it additionally rewrites the extension to `.gif` over the `_micro` variant, since HTML 3.2 browsers cannot display JPEG reliably. The base URL (without suffix) is a symlink to `_half` created by the optimizer.

**Not yet implemented**: the `image-single` block type, and older-epoch templates for the nine
epoch-3-only block types listed above - see `develop_docs/plans/cms-entry-model-plan.md` for the
full target schema.

### Home blog list (`/`)

The home page's "Latest Blog Posts" gallery (`modules/home_blog/home_blog.c`) lists `entries`
documents with `type == "blog"`:

```
home_blog(epoch, lang)                          ── src/modules/home_blog/home_blog.c
  │
  ├─ cms_get_blog_entries(lang, HOME_BLOG_LIMIT, &items, &count)  ── src/db/cms_entries.c
  │     db.entries.find({ type: "blog", enabled: true })
  │       .sort({ "header.date": -1 }).limit(HOME_BLOG_LIMIT)
  │     resolves header.* and categories[] to `lang`, reusing the same
  │     resolve_header_fields()/resolve_category_names() helpers as cms_get_entry_by_link()
  │
  ├─ if count == 0: home-blog/empty_epoch<N>.html ("No blog entries found")
  │
  └─ for each item: home-blog-item_epoch<N>.html
        link        -> "/blog/<item.link>"
        categories  -> elements/category/category_epoch<N>.html items, concatenated
                        (no entry-categories wrapper)
```

`HOME_BLOG_LIMIT` (`src/db/cms_entries.h`, currently 10) bounds the result size;
`cms_get_blog_entries()` allocates a fixed-size array of that length and never grows it. On a
DB error or if MongoDB is not ready, it returns `*out_count == 0` and the empty-state template
is shown - the home blog list is decorative and must never fail the home page.

### Blog list (`/blog`, `/blog/category/<slug>`)

A dedicated page listing every `entries` document with `type == "blog"`
(`modules/blog_list/blog_list.c`), optionally filtered to one category:

```
http_router.c  (route == "/blog" or "/blog/category/<slug>")
  │
  ├─ cms_get_categories(content_lang, &cats, &cat_count)   ── src/db/cms_categories.c
  │
  ├─ [/blog/category/<slug> only] resolve <slug> against slugify(cats[i].name)
  │     no match -> 404
  │
  ├─ category_menu_render(cats, cat_count, current_slug, epoch)
  │                                                 ── src/modules/category_menu/category_menu.c
  │
  ├─ blog_list(epoch, lang)                         ── src/modules/blog_list/blog_list.c
  │  or blog_list_category(epoch, lang, cat_id)     (same, restricted to one category id)
  │     │
  │     ├─ cms_get_blog_entries(lang, BLOG_LIST_LIMIT, &items, &count) ── src/db/cms_entries.c
  │     │     db.entries.find({ type: "blog", enabled: true })
  │     │       .sort({ "header.date": -1 }).limit(BLOG_LIST_LIMIT)
  │     │     (the category variant adds { categories: {$in: [ObjectId(cat_id)]} })
  │     │
  │     ├─ if count == 0: blog-list/empty_epoch<N>.html ("No blog entries found")
  │     │
  │     ├─ for each item: home-blog/home-blog-item_epoch<N>.html (reused, same as
  │     │     home blog list - link -> "/blog/<item.link>", categories, etc.)
  │     │
  │     └─ blog-list/blog-list_epoch<N>.html (1 `%s`: items, heading "Blog")
  │
  └─ buildBlogListWebSiteAtUrl(epoch, title, content, "/blog", category_menu_html)
```

`cms_get_blog_entries()` is shared with `home_blog()`, parameterized by a `limit`:
`HOME_BLOG_LIMIT` (10) for the home page, `BLOG_LIST_LIMIT` (`src/db/cms_entries.h`, currently
50) for `/blog`. Same decorative/never-fail behavior: on a DB error or if MongoDB is not
ready, `*out_count == 0` and the empty-state template is shown instead of failing the page.

Both routes pass `"/blog"` as the active menu URL, so the Blog nav item stays highlighted while
browsing a category. The page title becomes `"Blog - <category name>"` on the filtered route.

Each blog article is then served at `/blog/<item.link>` - see "CMS entries" above.

### Category menu (`/blog`, `/blog/category/<slug>`)

`category_menu_render(categories, count, current_slug, epoch)`
(`modules/category_menu/category_menu.c`) renders the category bar shown directly under the
navbar on the blog listing pages:

```
category_menu_render(cats, count, current_slug, epoch)
  │
  ├─ count == 0 -> "" (no bar at all)
  │
  ├─ for each category:
  │     slug = slugify(category.name)
  │     slug == current_slug ? category-menu-item-selected_epoch<N>.html
  │                          : category-menu-item_epoch<N>.html
  │     (both: 2 %s - slug, name)
  │
  └─ category-menu_epoch<N>.html (1 %s: items)
```

The category `<slug>` is **derived from the display name at request time**, never stored. Two
consequences worth knowing: renaming a category changes its public URL, and two categories whose
names slugify identically are indistinguishable in the URL (the first match wins). If
`category-menu-item-selected_epoch<N>.html` is missing, the plain item template is used for the
active entry too; if the container or item template is missing, the whole bar degrades to `""`
rather than failing the page.

### Menu (all pages)

The nav bar (`modules/menu/menu.c`, used by both `buildHomeWebSite()` and `buildPageWebSite()`)
is backed by its own `menu` collection (`MENU_COLLECTION`, `src/db/mongodb_manager.h`),
decoupling navigation from `entries` so items can point anywhere (`/`, `/page/<link>`, external
URLs, ...):

```
menu(current_url, epoch)                        ── src/modules/menu/menu.c
  │
  ├─ cms_get_menu_items(lang, &items, &count)    ── src/db/cms_menu.c
  │     db.menu.find({ enabled: true })
  │       .sort({ order: 1 }).limit(MENU_ITEM_LIMIT)
  │     resolves name (map<lang,string>) to `lang` via resolve_lang_map()
  │     (src/db/bson_lang.c, shared with cms_entries.c)
  │
  ├─ for each item where item.link == current_url: menu-item-selected_epoch<N>.html
  └─ for each other item:                          menu-item_epoch<N>.html
       (both: 3 %s - link, name, separator)
```

`menu` documents are `{ _id, link, name: <map<lang,string>>, order, enabled }`. `MENU_ITEM_LIMIT` (`src/db/cms_menu.h`, currently 20) bounds the result size. If `cms_get_menu_items()` returns 0 items (DB not ready, empty collection, or a DB error), `menu()` falls back to a single built-in `{"/", "Home"}` item so the nav bar is never empty.

`menu-item-selected_epoch<N>.html` adds the CSS class `boat-rudder__navbar__menu_item--selected` (epoch 3) or equivalent styling for older epochs. The active item is determined by `strcmp(current_url, item.link)`:
- Home page (`/`) always passes `"/"`.
- Blog list (`/blog`) and all blog entries pass `"/blog"` - entries use the section URL so the Blog item stays highlighted.
- Pages pass `"/page/<link>"` - matches menu items that point to that specific page.
- Dashboard and other internal routes pass `"/"` (no menu item is highlighted).

---

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
