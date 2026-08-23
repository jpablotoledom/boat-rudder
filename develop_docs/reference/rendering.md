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
| 2 | `EPOCH_MIDDLE` | Netscape 4 / MSIE 5 era - table layout, no external stylesheet | `text/html; charset=UTF-8` |
| 3 | `EPOCH_MODERN` | Modern HTML5 + CSS3 | `text/html; charset=UTF-8` |

Epoch 2 has **no external stylesheet**. Its pages carry a small inline `<style>` in the page
shell - element selectors only (`a`, `body`, `td`, `p`, `h1`-`h3`), for the font family/size and
`text-decoration: none` - and everything else is expressed as HTML attributes: `bgcolor`,
`background`, `align`, `width`, `cellpadding`, `cellspacing`, `<font>`. This mirrors what the
previous site did and is the reason it survived Netscape 4. Netscape 4 misapplies most box
properties (`padding`, `margin`, `border`, `display`) on inline elements - it paints them without
advancing the text cursor, printing one element on top of the next - and can drop a whole rule
block it fails to parse. Keeping the styling in attributes takes that class of bug off the table
and makes the page look the same in a browser that ignores CSS entirely. The
`class="boat-rudder__..."` attributes still present in the epoch-2 templates are inert leftovers
from the epoch-3 templates they were derived from; nothing styles them.

### Page assembly (`layout/` + fragments)

A page is built in two stages, with two different mechanisms, and the split is the reason the
templates are shaped the way they are.

The **fragment** - `page/page-home_epoch<N>.html` for home, `page/page_epoch<N>.html` for a
content page, and the `page-entry` / `page-blog` variants - is the only part whose *shape* varies
between page types, so it is the only part that goes through `render_template()`. Its `%s` count
is fixed by the file, which is exactly why home (menu, slider, home-content, home-blog: four
placeholders) and a content page (menu, content: two) cannot be the same file.

Everything around it is identical for every page of an epoch, so it lives once per epoch under
`layout/` and is spliced in by name with `str_replace_first()`:

| File | Holds |
|---|---|
| `layout/layout_epoch<N>.html` | doctype, head, `<body>`, `{{PAGE_TITLE}}`, `{{CONTENT}}` |
| `layout/footer_epoch<N>.html` | the site footer |
| `layout/lightbox_epoch3.html` | epoch 3 gallery viewer |
| `layout/home-modal_epoch3.html` | epoch 3 home thumbnail modal |

`page_layout_wrap()` (`src/html_builder/page_layout.c`) does the assembly. A fragment opts into a
part by carrying its marker - `{{FOOTER}}`, `{{LIGHTBOX}}`, `{{HOME-MODAL}}` - and a part with no
file for that epoch resolves to nothing, so the retro epochs drop the epoch-3 viewers without
needing an empty file each. `{{BODY_BACKGROUND}}` fills the one attribute that differs between
epoch-2 pages: home and the blog listing carry the tiled backdrop, content pages do not.

Two practical consequences. Adding a part to every page of an epoch is a one-file edit, not
fourteen. And because the `layout/` files are substituted textually rather than through
`vsnprintf`, they carry a literal `%` - only fragments need `%%`.

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

- `page/` - one fragment per page type, all of them filling the layout's `{{CONTENT}}`:
  `page-home_epoch<N>.html` (4 `%s`: menu, slider, home content, home blog - home is the only
  page with four regions), `page_epoch<N>.html` (2 `%s`: nav, content), and the wider
  `page-entry_epoch{2,3}.html` and `page-blog_epoch{2,3}.html` variants, which the epochs below
  2 fall back out of into `page_epoch<N>.html`. Each carries `{{FOOTER}}`, and on epoch 3
  `{{LIGHTBOX}}` or `{{HOME-MODAL}}`.
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
- `home-blog/` - `home-blog_epoch<N>.html` (2 `%s`: heading, items) and `home-blog/empty_epoch<N>.html`
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
  each - slug, name), and `category-menu-separator_epoch<N>.html` (no placeholders), placed
  between two items. The category bar rendered under the navbar on `/blog` and
  `/blog/category/<slug>`. See "Category menu" below.
- `login/` - `login_epoch<N>.html`. Epoch 3 has 1 `%s` for an optional error block; epochs
  -1/0/1/2 are static "login is not available on this device" content.
- `dashboard/` - `dashboard_epoch<N>.html`, static "Welcome to dashboard" content per epoch (no
  placeholders).
- `error/` - `error_epoch<N>.html`, 2 `%s` placeholders (status code, message).
- `redirect/` - `redirect_epoch<N>.html`, the standalone "click here to continue" body sent
  with `302` responses (for clients that don't auto-follow `Location`); 2 `%s` placeholders
  (both the redirect target: link `href` and link text). Not wrapped by `page_epoch<N>.html`.
- `entry/` - `entry-categories_epoch<N>.html`, the category "tags" wrapper (1 `%s` - concatenated
  rendered category items); `entry-meta_epoch<N>.html` (2 `%s` - author name and
  the rendered categories block), which groups the author line and the category tags into one
  meta strip above the content. It is skipped entirely - not rendered empty - when the entry
  has `header.hide_author` set or no author at all, leaving just the category tags. See "CMS entries" below.
- `elements/<type>/` - one subdirectory per content-block type. The `category/` element is the
  shared category "tag": from epoch 3 it is an `<a href>` (2 `%s` - link, name) pointing at
  `/blog/category/<slug>`; epochs -1..2 keep the plain 1 `%s` label. `gallery/` has additional
  variants: `gallery_epoch<N>.html` (the container, one `%s` for its rows), `gallery-row_epoch<N>.html`
  (one `%s` for its items - shared with the standalone gallery page's thumbnail strip, so a row
  is a row everywhere) and `gallery-item_epoch<N>.html` (one shape covering all five epochs; where
  an epoch does not use one of the three arguments - epoch 3 opens the lightbox in place rather
  than following a link - the markup for it is commented out rather than the argument being
  dropped, so the positions never shift), plus `gallery-item-more_epoch3.html` /
  `gallery-item-hidden_epoch3.html` for the "+N" overflow tile. The standalone page adds
  `gallery-page_epoch<N>.html` (the shell - the "< Back" link is baked directly into it, see
  below), `gallery-page-main_epoch{1,2}.html` (the current image plus prev/next),
  `gallery-page-thumbstrip_epoch{1,2}.html` (one `%s` wrapping the thumbnail rows) and
  `gallery-page-thumb_epoch{1,2}.html`; epochs -1/0 list plain `gallery-page-image-entry`
  entries instead, and epoch 3 uses a `gallery-page-item_epoch3.html` grid. Epochs 1-2 additionally
  cap the inline gallery at 3 images and append `gallery-view-all_epoch<N>.html` (a link to the
  standalone page, when the block names a gallery) or `gallery-view-all-count_epoch<N>.html` (a
  plain count, when it does not) after the table - the machine rendering the article is the one
  paying for every image in the block, and an article's gallery can carry 20+ photos. Epoch 3
  handles the same problem differently, folding the rest behind an in-page "+N" tile instead of
  cutting them - see `gallery_columns()` and the cap logic in `render_gallery()`. The "< Back" link goes to the entry the gallery belongs to (`media_galleries.entry_id`), named
  by its title in the request's language, so a reader who followed a link into the gallery can
  find their way back to what they were reading - not to wherever they happened to click in from.
  `cms_get_entry_backlink()` (`src/db/cms_media_galleries.c`) resolves it; when the entry cannot
  be found (a gallery left without a parent) the link falls back to `javascript:history.back()`
  with the plain label "Back". It is baked directly into each `gallery-page_epoch<N>.html` as two
  positional arguments (`%1$s` the href, `%2$s` the label) ahead of the page's own content
  (`%3$s`) - fixed placement, not a template of its own, since the link itself never varies in
  shape, only in where it points. `list/` has `list-container_epoch<N>.html` (2 `%s`: items, then the tag - `ul` or `ol` - which WML leaves unused because it has no list element) and `list-item_epoch<N>.html`; `table/` has `table_epoch<N>.html` / `table-row_epoch<N>.html` / `table-cell_epoch<N>.html` /
  `table-header-cell_epoch<N>.html`. Epochs 1-2 colour it to match epoch 3's palette
  (`#1e2d3d` header, `#93c5fd` header text, `#1a1a1a` cells) using the same trick as the
  home-blog cards: the outer `<table>` carries the border colour as its `bgcolor` and
  `cellspacing="1"`, so the 1px gap between cells shows it - Netscape 4 does not propagate a
  table's `bgcolor` to its own cells, so each `<th>`/`<td>` sets its own. Epoch 0/-1 are left as
  plain text/WML, where colour has no meaning. See "Content block types" below for the full list and the epochs each one covers.
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
  │     ├─ page/page-home_epoch<N>.html    ── loaded by buildHomeWebSite()
  │     ├─ menu("/", epoch)                ── modules/menu
  │     │     cms_get_menu_items(lang, &items, &count) ── src/db/cms_menu.c
  │     ├─ slider(epoch)                   ── modules/slider
  │     ├─ home_content(epoch, lang)       ── modules/home_content
  │     ├─ home_blog(epoch, lang)          ── modules/blog_list
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
self-contained document per page - `header` (image, title, summary, `author_id`, date,
`hide_author`) plus an ordered `content[]` array of typed blocks - with all user-facing text
stored as a `map<lang,string>` keyed by ISO 639-1 codes (`en`, `es`, ...).

`header.hide_author` (bool, default false) drops the byline from every public surface - the
entry page, the home blog list and `/blog` - for pages that are not authored articles, such as
"Projects". The `author_id` itself is kept: `can_edit_entry()` needs it, and the dashboard
listing and the editor still show the real name.

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
| `tittle` | heading text | heading level `1`-`6` (anything else = `2`) | -1..3 |
| `paragraph` | rich text (stored as HTML) | style variant, e.g. `lead` / `note` | -1..3 |
| `byline` | author | date | -1..3 |
| `image` | image base URL | `"<caption>\|<width>\|<align>"` (see below) | -1..3 |
| `gallery` | `;`-separated image URLs | `media_galleries._id` | -1..3 |
| `separator` | unused | style variant (validated modifier) | -1..3 |
| `link` | link label | target URL (`#` if empty) | -1..3 |
| `list` | one item per line | `"ol"` for ordered, anything else = unordered | -1..3 |
| `table` | rows by line, cells by `\|` | `"header"` makes row 0 a header row | -1..3 |
| `code-text` | code body | caption shown above the code (e.g. a filename) | -1..3 |
| `youtube-embed` | caption / link label | watch/short/embed URL, normalized to `/embed/<id>` | -1..3 |
| `image-paragraph` | image URL (see note below) | `"left"` / `"right"` alignment | -1..3 |
| `social-networks` | display name (falls back to the icon name) | `"<icon>\|<url>"` | -1..3 |
| `generic` | raw HTML passthrough | unused | 3 |

Every type except `generic` now has a template for each epoch (the older-epoch files were
ported from the legacy CMS in `../the-retro-center-old`). Where a template is still missing,
`load_template()` returns `NULL` and the renderer yields `""`, so the entry still renders
without that block. An unknown `content[].type` renders as `""` for the same reason.

`paragraph` text may be stored either as HTML (what the dashboard's rich-text editor writes)
or as plain text with literal newlines (what the CMS migration produced). HTML collapses a bare
newline into a space, so `expand_newlines()` turns each one into a line break the target epoch
understands - `<br/>` for WML, `<br>` elsewhere - before the text reaches the template.
`code-text` needs the same treatment only on WML, since every other epoch wraps the code in
`<pre>`.

**`extra_data` is a value, never markup.** The legacy CMS stored whole attributes there
(`style="font-size: 18px"`) and interpolated them unescaped, which let anyone with editor
access inject HTML into a page. Renderers now validate the value and build the attribute
themselves: `modifier_class()` accepts only `[a-z0-9-]` and emits `<base>--<value>`, and
`image`/`tittle` check theirs against fixed sets. Anything unrecognized falls back to the
default and never reaches the page.

`image` packs three fields as `"<caption>|<width>|<align>"` - the same pipe convention
`social-networks` uses. `width` is one of `100`/`50`/`30` and `align` one of
`left`/`center`/`right`, both defaulting to `100`/`center`. A value with no `|` is all
caption, which is how blocks written before these options look. Epoch 3 turns them into
modifier classes (`boat-rudder__entry-image--w50`); epochs 1-2 have no stylesheet, so they
carry the values as attributes - `align` on the wrapping element and `width` on the `<img>`.

The standalone gallery page (`/gallery/<id>?img=N`) is laid out with tables in epochs 1-2, and
its thumbnail strip is chunked six to a row: these browsers do not reflow a table row, so one
long strip would run off the screen. Its main image is `_half` at `width="100%"` and carries no
stylesheet rule of its own.

Clicking the image opens a bigger copy of it. Epoch 3 passes it to the gallery lightbox through
`data-full` and gets `_full`, the original resolution; epochs 1-2 link straight to the file and
get `_half`, capped at 1024px - a machine of that era has neither the memory nor the connection
for a full-resolution photo, and 1024 already exceeds what it can display.

That `width` must be **in pixels**. Percentage values on `<img width>` arrived with HTML 4.0,
and a browser of the epoch-2 era meeting one draws the image zero pixels wide: it loads, it just
cannot be seen, and no broken-image icon or `alt` text appears to hint at why. So the author's
100/50/30 is resolved against the real width of the file being served, read from the GIF header
by `image_intrinsic_width()` (`src/utils/image_size.c`). The `width` attribute lives inside the
template's placeholder rather than around it, so it can be omitted entirely when that width
cannot be established - the image then comes out at its natural size. The result is capped at
`RETRO_MAX_IMAGE_WIDTH` (550px): these browsers ran on 640x480 and do not reflow, so an image
wider than the content column does not shrink, it grows a horizontal scrollbar and pushes the
text off the right edge. Only `width` is ever emitted, never `height`, so the picture keeps its
proportions. The same rule applies to
any `<img>` written directly into a retro template.

`social-networks` builds its icon path as
`/themes/dark/assets/social-networks/<icon>.<ext>`, picking the extension per epoch: `.svg`
for epoch 3, `.gif` for epoch 2, `-s.gif` (small) for epoch 1, and no icon at all for
epochs -1/0, whose templates keep it inside a comment. Note the theme segment is hardcoded,
so this block type does not follow the active `theme` setting.

`entries.categories[]` (an `ObjectId[]` referencing `entry_categories._id`, per
`plans/cms-entry-model-plan.md` §2.2) is resolved to category names and rendered as a small "tags"
block under the header. `entry_categories` documents are `{ _id, name: <map<lang,string>> }` -
a separate collection (kept normalized, since categories are shared across entries). An entry
with no `categories` field/empty array renders with no tags block. From epoch 3 each tag links
to `/blog/category/<slug>`, where the slug comes from `slugify(name)` in the current content
language (see "Category menu" below).

Image URLs stored in `header.image_url` and `content[].text` (for `image` and `gallery` blocks)
are **bare paths with no size suffix**. The variants (`_micro`/`_small`/`_medium`/`_half`/`_full`)
are generated by `scripts/image-optimizer.sh`, and the renderer appends the one it wants:

| Caller | Variant |
|---|---|
| `home_blog`, `blog_list`, `entries_admin` thumbnails | `_small` |
| `gallery` block, epoch 3 | `_small` (grid) + `_full` (lightbox) |
| `gallery` block, epoch 1 | `_micro`, extension rewritten to `.gif` |
| `image` block, epoch 3 / 2 / 1 | `_full` / `_half` / `_medium.gif` |
| `image-paragraph` block, epoch 3 / 1-2 | `_full` / `_micro.gif` |
| `/gallery/<id>` page | `_small` + `_full` |

Epoch 1 gets GIFs because HTML 3.2 browsers cannot display JPEG reliably.

`image-paragraph` is the one exception to "bare path, renderer appends a suffix": its
`content[].text` stores the path **with `_full` already baked in**, not a bare path. Getting
`_micro` for epochs 1-2 therefore means splicing out the stored `_full` first
(`image_paragraph_src()` in `entry_page.c`) rather than appending a second suffix on top of it.

The optimizer also creates a base-name symlink pointing at `_half` so a bare URL resolves on
its own, but **that covers only images run through the script** - images uploaded through the
media dashboard have no symlink (13 of 754 files in the reference site). Renderers must
therefore always append a variant rather than rely on the bare path resolving.

Clicking an image opens it at full size: epoch 3 hands it to the same lightbox the galleries
use (any `img[data-full]` on the page joins that viewer, so gallery items and standalone
`image` blocks navigate as one sequence), while epochs 1-2 wrap the image in a plain link to
the `_full` file - the `/gallery/<id>` viewer needs a `media_galleries` id, which an `image`
block has nowhere to store.

`youtube-embed` cannot embed a player before HTML5, so retro epochs render a **QR code** the
reader scans with a phone, plus a text link. `utils/qr_generator/` encodes it with
`libqrencode` and writes the asset per epoch: a GIF (epochs 1-2), a WBMP (WML), or Unicode
half-blocks inline (epoch 0). Assets are cached under `html/content/qr/` (gitignored,
regenerated on demand) and written via a temp file + `rename()`, since several connection
threads may render the same page at once.

**Not yet implemented**: the `image-single` block type (legacy entries using it were migrated
to `image`), and older-epoch templates for `generic` - see
`develop_docs/plans/cms-entry-model-plan.md` for the full target schema.

### Home blog list (`/`)

The home page's "Latest Blog Posts" gallery (`modules/blog_list/blog_list.c`) lists `entries`
documents with `type == "blog"`:

```
home_blog(epoch, lang)                          ── src/modules/blog_list/blog_list.c
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
  │     ├─ if count == 0: home-blog/empty_epoch<N>.html ("No blog entries found")
  │     │
  │     ├─ for each item: home-blog/home-blog-item_epoch<N>.html (reused, same as
  │     │     home blog list - link -> "/blog/<item.link>", categories, etc.)
  │     │
  │     └─ home-blog/home-blog_epoch<N>.html (2 `%s`: heading "Blog", items)
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
  │     category-menu-separator_epoch<N>.html   (between items only, never at either end)
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

A category tag is a link in every epoch, WML included - an anchor is the one thing all of them
can do. All four places that list an entry's categories - the cards on home and `/blog`, the article
page, and the dashboard's entry list - go through `category_tags_render()`
(`src/utils/category_tags.c`), which applies the separator. They used to be four hand-written
copies of the same loop, which is how the dashboard ended up without the separator the public
site had gained.

The separator exists for every epoch, but only those that lay the bar out as running text carry
a visible one: ` | ` for epochs -1 and 0, ` |&nbsp;` for epoch 2 (the hard space keeps a bar and
the category it introduces on the same line). Epoch 1 lays the bar out as a row of `<td>` and
epoch 3 as a flex container, so their files hold only whitespace. Do not bake the separator into
the item template instead - that leaves one dangling after the last category, which is what
epoch 0 used to do.

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
