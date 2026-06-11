# Home Blog List - database-backed `entries` (type: "blog")

> **Status**: implemented - see
> [architecture.md, "Home blog list"](../reference/architecture.md#home-blog-list).

## 1. Context & Goal

`/` currently renders its "Latest Blog Posts" section (`home_blog()`,
`src/modules/home_blog/home_blog.c`) from a hard-coded `BLOG_POSTS[]` array - an MVP
placeholder with no database backing.

The `entries` collection (embedded `header` + `content[]` + `categories[]`, see
[cms-entry-model-plan.md](cms-entry-model-plan.md) and
[architecture.md, "CMS entries"](../reference/architecture.md#cms-entries-get-pagelink)) already supports
an `entries.type` field with values `"page"` and (per the target schema) `"blog"`. This
increment makes the home blog list read real `entries` documents where `type == "blog"` and
`enabled == true`, ordered by `header.date` (newest first), reusing the exact same
`map<lang,string>` resolution, header shape, and category-tag rendering already built for
`/page/<link>`.

Out of scope: category-filtered listing (`getBlogItemsByCategory`-style queries) - remains a
future increment per `cms-entry-model-plan.md` §3.2. A dashboard editor for blog entries is
also out of scope. (The dedicated `/blog` listing route, and `/blog/<link>` for individual
articles, were implemented as a separate increment - see
[architecture.md, "Blog list"](../reference/architecture.md#blog-list-blog).)

## 2. DB layer (`src/db/cms_entries.h` / `cms_entries.c`)

### 2.1 Share header/category parsing between `CmsEntry` and a new list-item type

`parse_header()` and `parse_categories()` currently write directly into a `CmsEntry *out`
(`out->header_*`, `out->category_names`/`category_count`). The blog list needs the same two
pieces of data (header fields + resolved category names) per matching document, but does
**not** need `content[]`, so it shouldn't allocate/parse a full `CmsEntry`.

Extract the BSON-parsing bodies of `parse_header()` and `parse_categories()` into two
reusable static helpers that take plain output parameters instead of `CmsEntry *`:

```c
// Fills *image_url/*title/*summary/*author (always malloc'd, "" on absence) and
// date[16] ("YYYY-MM-DD" or "" if absent), resolving map<lang,string> fields to `lang`.
static void resolve_header_fields(const bson_t *doc, const char *lang,
                                   char **image_url, char **title, char **summary,
                                   char **author, char date[16]);

// Resolves doc.categories[] (ObjectId[]) -> entry_categories.name, for `lang`.
// *out_names/*out_count are NULL/0 on any failure (decorative, never fails the caller).
static void resolve_category_names(const bson_t *doc, const char *lang,
                                    char ***out_names, size_t *out_count);
```

`parse_header()` and `parse_categories()` become thin wrappers around these two helpers,
assigning into `out->header_*` / `out->category_*` as before - **`CmsEntry`'s field layout and
`entry_page.c` are unaffected**.

### 2.2 New type: `CmsBlogListItem`

```c
// One "entries" document with type == "blog", header + categories resolved to `lang`.
// Lighter than CmsEntry: no content[] (not needed for a list view).
typedef struct {
    char *link;              // entries.link, for building /page/<link>

    char *header_image_url;
    char *header_title;
    char *header_summary;
    char *header_author;
    char  header_date[16];   // "YYYY-MM-DD", empty if absent

    char  **category_names;  // entries.categories[] -> entry_categories.name, for `lang`
    size_t  category_count;  // 0 if the entry has no categories
} CmsBlogListItem;
```

### 2.3 New function: `cms_get_blog_entries()`

```c
// Looks up db.entries.find({type: "blog", enabled: true}).sort({"header.date": -1}),
// resolving header.* and categories[] to `lang` for each match (same lang convention as
// cms_get_entry_by_link: configs/settings.conf "Eng"/"Esp" -> ISO "en"/"es", default "en").
// On success, *out points to a malloc'd array of *out_count items (possibly 0) and the
// caller must pass it to cms_blog_list_free(). On a DB error or if mongodb is not ready,
// *out = NULL and *out_count = 0 (the home page renders with an empty list, never fails).
void cms_get_blog_entries(const char *lang, CmsBlogListItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_blog_list_free(CmsBlogListItem *items, size_t count);
```

Implementation notes:

- Query: `BCON_NEW("type", BCON_UTF8("blog"), "enabled", BCON_BOOL(true))` - no `menu`
  field (see §6.1).
- Options: `BCON_NEW("sort", "{", "header.date", BCON_INT32(-1), "}", "limit",
  BCON_INT64(HOME_BLOG_LIMIT))` via `mongoc_collection_find_with_opts()` - same call shape
  already used in `cms_entries.c`/`auth.c`/`session_manager.c`, just with a non-NULL
  `opts`.
- `HOME_BLOG_LIMIT` is a new `#define HOME_BLOG_LIMIT 10` in `cms_entries.h` (or
  `cms_entries.c`, alongside the other `MAX_*`-style constants) - the home list always
  shows at most the 10 most recent `type: "blog"` entries. The `/blog` listing route
  (implemented separately - see
  [architecture.md, "Blog list"](../reference/architecture.md#blog-list-blog)) reuses the
  same query with a higher `BLOG_LIST_LIMIT`.
- The result count is at most `HOME_BLOG_LIMIT`, so the output array can be a fixed-size
  `CmsBlogListItem[HOME_BLOG_LIMIT]` allocated once (`calloc(HOME_BLOG_LIMIT, ...)`) and
  filled while iterating the cursor, with `*out_count` set to however many were actually
  returned (no `realloc`-per-item growth needed).
- For each matching doc: `link` via `bson_iter_init_find(doc, "link")` (same pattern as
  `cms_get_entry_by_link`), then `resolve_header_fields()` and `resolve_category_names()`
  (§2.1) directly into the new item's fields.
- On any error (`mongodb_manager_get_collection()` returns NULL, cursor error), set
  `*out = NULL`, `*out_count = 0` and free anything already allocated - the blog list is
  decorative on the home page and must never take down `/`.

### 2.4 `mongodb_manager.h` / `ENTRIES_COLLECTION`

No new collection constants needed - `cms_get_blog_entries()` queries
`ENTRIES_COLLECTION` (already defined) and `resolve_category_names()` queries
`ENTRY_CATEGORIES_COLLECTION` (already defined, added for `entry_categories`).

## 3. Rendering layer

### 3.1 `src/modules/home_blog/home_blog.c`

- Change the public signature to `char *home_blog(int epoch, const char *lang)` (the
  orchestrator already receives `lang` - see §3.3).
- Remove the static `BLOG_POSTS[]` array and `blog_post_t` type entirely.
- Call `cms_get_blog_entries(lang, &items, &item_count)`; always call
  `cms_blog_list_free(items, item_count)` before returning.
- For each item, build the per-item HTML:
  - **epoch >= 1** (image-card layout, 7 `%s` after the template change in §3.2):
    `image_url`, `link_url`, `title`, `summary`, `author`, `categories_html`, `date`.
  - **epoch -1/0** (text-only layout, 4 `%s` after the template change in §3.2):
    `title`, `date`, `summary`, `categories_html` - same order as the current static
    implementation, with `categories_html` appended.
- `link_url` is `/page/<link>` (built with `snprintf`/`render_template`, e.g.
  `"/page/%s"`). `cms_get_entry_by_link()`'s query
  (`db.entries.findOne({link, enabled: true})`, **no `type` filter** - see
  `src/db/cms_entries.c`) fetches any enabled entry by `link` regardless of `type`, but
  `http_router.c`'s `/page/<link>` handler additionally 404s unless `entry.type` is `"page"`
  - this check needs to also accept `"blog"` so a `type: "blog"` entry's `link` is viewable
  at `/page/<link>` (see §4).
- `categories_html` (**all epochs**, see §6.4): the concatenated
  `elements/category/category_epoch<N>.html` items (one per category) - empty string if
  `category_count == 0`. Unlike `entry_page.c`'s `render_categories()` (§3.2), this is
  **without** the `entry-categories_epoch<N>.html` wrapper, since the placeholder already sits
  inside the item's own byline container (`<div>`/`<span>`/inline text) - nesting another
  block-level wrapper there would be invalid or visually awkward.
- **Empty list** (`item_count == 0`, see §6.3): instead of rendering
  `home-blog_epoch<N>.html` with empty `items`, load and use a new
  `home-blog/empty_epoch<N>.html` template (static "No blog entries found" message) as
  the section's body - i.e. `render_template(content_tpl, empty_tpl)` instead of
  `render_template(content_tpl, items)`.

### 3.2 Template changes (`html/themes/dark/home-blog/`)

Per §6.4, category tags are shown for **all 5 epochs** (matching how the-retro-center's
home blog list always shows categories, regardless of epoch).

- **`home-blog-item_epoch{3,2,1}.html`**: add one more `%s` placeholder for the rendered
  category tags, placed in the byline area next to the author/date (mirroring
  `entry/entry-categories_epoch<N>.html`'s placement under the entry header). New
  placeholder count: **7** (`image, link, title, summary, author, categories, date`).
  - epoch3: add `<div class="boat-rudder__home-blog__item__byline-categories">%s</div>`
    inside `.boat-rudder__home-blog__item__byline`.
  - epoch2: add `<span class="boat-rudder__home-blog__item__byline-categories">%s</span>`
    inside `.boat-rudder__home-blog__item__byline`.
  - epoch1: append `%s` after the author/date line (plain text, consistent with epoch1's
    `<font>`-based styling).
- **`home-blog-item_epoch{0,-1}.html`**: add one more `%s` placeholder, appended after the
  summary. New placeholder count: **4** (`title, date, summary, categories`).
  - epoch0: append `%s` after the `<p>%s</p>` summary line.
  - epoch-1 (WML): append `%s` after the summary, inside the same `<p>` - WML text browsers
    have no inline styling, so the category item template for this epoch is plain
    `[%s]\n` (already established for `/page/<link>`'s `elements/category/category_epoch-1.html`,
    see [cms-entry-model-plan.md](cms-entry-model-plan.md)).
- Category tags themselves are rendered with the **existing**
  `elements/category/category_epoch<N>.html` item template (one per category) for every
  epoch - no new category template files needed. The items are concatenated directly into
  `categories_html` **without** the `entry/entry-categories_epoch<N>.html` wrapper (that
  wrapper remains `/page/<link>`-only, via `entry_page.c`'s `render_categories()`).
- **CSS** (`styles_epoch3.css`, `styles_epoch2.css`): add
  `.boat-rudder__home-blog__item__byline-categories` (small/muted text, inline-flex with
  `gap`, reusing the existing `.boat-rudder__entry-category` /
  `.br-entry-category` badge styles for the `<span class="boat-rudder__entry-category">`/
  `.br-entry-category` items rendered inside).

### 3.3 New `home-blog/empty_epoch<N>.html` templates (5 files)

Per §6.3, a new static (no placeholders) template per epoch, shown via
`render_template(content_tpl, empty_tpl)` when `cms_get_blog_entries()` returns 0 items
(see §3.1). Message: "No blog entries found" (English only - static template text is not
currently translated per `lang`, consistent with the "Latest Blog Posts" heading in
`home-blog_epoch<N>.html`, which is also untranslated).

- epoch3: `<p class="boat-rudder__home-blog__empty">No blog entries found</p>`
- epoch2: `<p class="br-home-blog-empty">No blog entries found</p>`
- epoch1: `<p><font color="#FFFFFF">No blog entries found</font></p>`
- epoch0/-1: `<p>No blog entries found</p>`

CSS: optionally add `.boat-rudder__home-blog__empty` / `.br-home-blog-empty` (muted/italic
text) to `styles_epoch3.css` / `styles_epoch2.css`.

### 3.4 `src/html_builder/orchestrator.c`

`buildHomeWebSite(int epoch, const char *lang)` already receives `lang` - only the
`home_blog(epoch)` call site changes to `home_blog(epoch, lang)`. No signature change to
`buildHomeWebSite()` itself.

## 4. Documentation updates

- **`architecture.md`**, "Build pipeline" diagram: change
  `├─ home_blog(epoch)  ── modules/home_blog` to
  `├─ home_blog(epoch, lang)  ── modules/home_blog`, and add a line describing
  `cms_get_blog_entries(lang, ...)`
  (`db.entries.find({type:"blog", enabled:true}).sort({"header.date":-1}).limit(10)`),
  mirroring how `cms_get_entry_by_link` is described under "CMS entries".
- **`architecture.md`**, "Templates" section: update the `home-blog/` bullet - epoch 1/2/3
  item templates now have **7** `%s` placeholders (image, link, title, summary, author,
  categories, date); epoch -1/0 now have **4** `%s` (title, date, summary, categories).
  Mention the new `home-blog/empty_epoch<N>.html` (static, no placeholders), shown when
  the blog list is empty.
- **`src/web_server/http_router.c`**: the `/page/<link>` handler 404s unless
  `entry.type == "page"`; widen this check to also accept `"blog"` so the home blog list's
  `/page/<link>` items render instead of 404ing.
- **`architecture.md`**, "CMS entries" section: fix "Only `entries.type == "page"`
  documents are served by `/page/<link>`" to describe the actual two-step check:
  `cms_get_entry_by_link()`'s query has no `type` filter, but `http_router.c` only renders
  `type: "page"` or `type: "blog"` entries (other types 404).
- **`cms-entry-model-plan.md`** §3.2 "Listing blog cards": mark this query pattern as
  **implemented** (`cms_get_blog_entries()`, `src/db/cms_entries.c`, capped at
  `HOME_BLOG_LIMIT = 10`), matching the "Status" blockquote convention used for
  §3.1/`entries`/`entry_categories` after the previous increment. Note that the
  implemented query omits the `menu: true` filter from the original proposal (navigation
  is being redesigned separately, see §6.2) and that category **names** (not just ids) are
  resolved per item via the same `entry_categories` lookup as `/page/<link>`.

## 5. Verification plan

1. `cmake --build build2` - clean, no new warnings.
2. Seed a few `type: "blog"` entries via `mongosh` (reusing the `entry_categories` docs
   seeded for the previous increment), e.g.:
   ```js
   db.entries.insertOne({
     link: "first-post", type: "blog", enabled: true,
     categories: [c1],
     header: {
       image_url: "/themes/dark/assets/home-blog/sample.jpg",
       title:   { en: "First Post",  es: "Primera Entrada" },
       summary: { en: "Hello world", es: "Hola mundo" },
       author:  { en: "Boat Rudder Crew", es: "Tripulación de Boat Rudder" },
       date: ISODate("2026-06-01T00:00:00Z")
     },
     content: []
   });
   ```
   Insert a second entry with an earlier `header.date` and no `categories` to check
   ordering and the no-categories case.
3. `curl /` for epochs `3, 2, 1, 0, -1` (force_epoch, restart server between runs) and both
   `lang=Eng`/`lang=Esp` - confirm:
   - Items appear newest-first by `header.date`.
   - Title/summary/author/date/image render correctly per language.
   - Category tags render for the entry that has `categories`, **for every epoch
     including -1/0**, and render as nothing (no empty `<div>`/placeholder leftovers) for
     the entry without.
   - `<a href="/page/first-post">` works and renders the full entry page (header +
     content, even if `content: []`).
4. Insert 12 `type: "blog"` entries total (varying `header.date`) and confirm only the 10
   most recent appear on `/`, across at least one epoch.
5. Sanity-check the zero-results case: temporarily `enabled: false` all blog entries, curl
   `/` for a couple of epochs - confirm the "Latest Blog Posts" section renders the new
   "No blog entries found" empty template (no `%s`/placeholder leftovers, no crash), then
   restore.

## 6. Resolved decisions

1. **Result limit**: `cms_get_blog_entries()` caps the home list at the **10** most
   recent `type: "blog"` entries via `HOME_BLOG_LIMIT = 10` (§2.3). The `/blog` listing
   route (implemented separately - see
   [architecture.md, "Blog list"](../reference/architecture.md#blog-list-blog)) reuses the
   same query with `BLOG_LIST_LIMIT = 50`.

2. **No `menu` field on `entries`** - separate `menu` collection. Implemented as a
   separate increment - see
   [architecture.md, "Menu"](../reference/architecture.md#menu-all-pages). Site
   navigation (`src/modules/menu/menu.c`) is backed by its own `menu` collection of link
   documents, `{ _id, link, name: map<lang,string>, order, enabled }`. This decouples
   the top nav from `entries` entirely, so menu items can point anywhere (external URLs,
   `/page/<link>`, `/`, etc.), not just CMS pages. The home blog list query in this plan
   stays `{type: "blog", enabled: true}` with no `menu` field involved.

3. **Empty state**: implemented via a new static `home-blog/empty_epoch<N>.html` template
   per epoch, message "No blog entries found" (§3.3), shown when
   `cms_get_blog_entries()` returns 0 items.

4. **Category tags on all epochs**: category tags are rendered for **every** epoch
   (-1 through 3), not just 1-3 (§3.2), matching the source project's behavior of always
   showing categories on home blog list items regardless of epoch.
