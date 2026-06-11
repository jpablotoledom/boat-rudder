# the-retro-center "Entry" Data Model (reference)

> **Context**: this document is a reference analysis of the sister project
> `../the-retro-center`, describing how it models a CMS **page** ("entry") and its
> **content** in MongoDB, and how that data is rendered into epoch-aware HTML/WML.
> It exists to inform a possible future increment of Boat Rudder's MVP (a dynamic,
> database-backed content system), by documenting a working pattern that already
> solves the same problem (multi-epoch rendering, multi-language text, ordered
> content blocks). It does **not** describe anything currently implemented in
> Boat Rudder.

Diagrams:
- [diagrams/retro-center-entry-er.puml](diagrams/retro-center-entry-er.puml) - collections
  and how they reference each other.
- [diagrams/retro-center-page-render-flow.puml](diagrams/retro-center-page-render-flow.puml) -
  end-to-end request flow for rendering one entry page.

## 1. Collections overview

The database is `the_retro_center` (see `src/api/mongodb_manager.h`). The collections
relevant to "a page and its content" are:

| Collection | Role |
|---|---|
| `entries` | One document per **route/page** (slug, type, ordering, visibility, menu flag). The root object everything else hangs off of. |
| `header` | 1:1 metadata for an entry: hero image, title, summary, date, author. Used for blog cards and SEO (`<meta>`/OpenGraph) tags. |
| `content` | 1:N **ordered content blocks** for an entry - the actual body of the page, as a sequence of typed elements. |
| `translation` | Free-standing `{ eng, esp }` text pairs, referenced by `ObjectId` from `entries`, `header` and `content`. This is the multi-language layer. |
| `entry_categories` | Join collection: links an `entries` document to a category (itself a `translation` document used as the category's display name). |
| `media` / `media_directories` | Image galleries, referenced from `content` blocks of type `gallery`. |

## 2. `entries` - the page/route

Source: `src/api/private/insert_entry.c`, `src/api/private/get_entry.h` (`Entry` struct).

| Field | Type | Meaning |
|---|---|---|
| `_id` | ObjectId | Primary key. |
| `link` | string | Public slug, unique. Used as the lookup key for everything else (`/blog/<link>`, `/page/<link>`). |
| `type` | string | `"blog"`, `"page"`, ... - drives which routes/listings the entry shows up in. |
| `order` | int | Sort order (e.g. for menu items). |
| `enabled` | bool | Visibility toggle. |
| `menu` | bool | Whether this entry appears in the site navigation. |
| `name_id` | ObjectId -> `translation` | Short name/title, e.g. for the menu link label. |
| `categories` | ObjectId[] -> `entry_categories` | Categories this entry belongs to (used to filter blog listings). |

## 3. `header` - page metadata (1:1 with an entry)

Source: `src/api/private/get_header.h` (`Header` struct), `insert_header.c`.

| Field | Type | Meaning |
|---|---|---|
| `_id` | ObjectId | Primary key. |
| `entry_id` | ObjectId -> `entries._id` | The entry this header belongs to. |
| `image_url` | string | Hero/featured image path. |
| `tittle_id` | ObjectId -> `translation` | Long-form page title. *(sic: "tittle" is the spelling used throughout the codebase)* |
| `summary_id` | ObjectId -> `translation` | Page summary/excerpt. |
| `author_id` | ObjectId -> `translation` | Author display name. |
| `date` | datetime | Publication date. |

`header` feeds `getBlogItemBySlug()` / `getPageMetaBySlug()` (`HomeBlogItems` struct in
`src/api/public/home_blog_items.h`), which is used for:
- Blog listing cards (title, summary, image, author, date, category text).
- SEO metadata on epoch3 pages: canonical URL, OpenGraph title/image/description, etc.,
  via `container_seo(...)`.

## 4. `content` - the page body, as ordered typed blocks

Source: `src/api/public/page_items.h` (`PageItems` struct), `src/api/public/page_items.c`
(`getPageItems`).

| Field | Type | Meaning |
|---|---|---|
| `_id` | ObjectId | Primary key. |
| `entry_id` | ObjectId -> `entries._id` | The entry (page) this block belongs to. |
| `content_id` | ObjectId -> `translation` | The block's user-facing text, resolved per language. |
| `type` | string | The **element type** - selects which template renders this block (see §6). |
| `extra_data` | string | Type-specific configuration that is **not** translated (e.g. heading level, image filenames, gallery id). |
| `order` | int | Sort key - blocks are rendered in ascending `order`. |

`getPageItems(link, lang)` retrieves these via an aggregation pipeline:

```
content
  $lookup entries        (content.entry_id == entries._id)  -> as "entry"
  $unwind entry
  $match entry.link == <link>
  $lookup translation     (content.content_id == translation._id) -> as "translation"
  $unwind translation
  $sort order ascending
```

The `content` field returned to the caller is `translation.eng` or `translation.esp`
depending on the requested `lang`.

## 5. `translation` - the multi-language layer

Source: `src/api/private/insert_translation.c`.

| Field | Type | Meaning |
|---|---|---|
| `_id` | ObjectId | Primary key, referenced from `entries.name_id`, `header.tittle_id` / `summary_id` / `author_id`, `content.content_id`, and `entry_categories.category_id`. |
| `eng` | string | English text. |
| `esp` | string | Spanish text. |

Every piece of user-facing text in the system is stored exactly once here and resolved
at read time based on the request's language (`lang` cookie/query, "Esp" vs. default "eng").

## 6. `entry_categories` - categories

Source: `src/api/private/insert_entry_category.c`, `get_entry_category_ids.c`,
`get_categories.c`.

| Field | Type | Meaning |
|---|---|---|
| `_id` | ObjectId | Primary key, referenced from `entries.categories[]`. |
| `category_id` | ObjectId -> `translation` | The category's display name (translated). |

So a category is really just "a `translation` document, wrapped in an `entry_categories`
document so it can be referenced from `entries.categories[]` and listed independently".

## 7. Content block types ("elements")

Each `content.type` maps to a template family under
`html/themes/<theme>/elements/<type>/<type>_epoch<N>.html` (one variant per epoch
`-1..3`). `src/modules/page/page.c` loads **all** of these templates up front, then for
each `PageItems` entry (in `order`), formats the matching template using `content`
(translated text) and `extra_data` (untranslated config), with epoch-specific
adjustments. Element types found in `html/themes/dark/elements/`:

| Type | `content` holds | `extra_data` holds | Notable epoch-specific behavior |
|---|---|---|---|
| `paragraph` | Paragraph text (HTML allowed) | (epoch>WML) extra formatting param | WML drops `extra_data`. |
| `tittle` | Heading text | Heading level (`1`-`6`, default `1`) | WML drops the level. |
| `image` | Image filename | Caption/alt | - |
| `image-single` | Image filename | Caption | Per-epoch filename suffix: `_micro`/`_medium` (epoch1), `_small`/`_full` (epoch3), `_small`/`_half` (epoch2), none (WML/epoch0). |
| `image-landscape` | Image filename | Caption | Variant of `image-single` for landscape layout. |
| `image-paragraph` | Image filename | Paragraph text | Same per-epoch image-suffix logic as `image-single`. |
| `gallery` | `;`-separated list of image filenames | Gallery id (links to `media`/`media_directories`) | epoch -1/0: text link "[View gallery]" only; epoch 1/2: first 3 thumbnails + "View all (N)" link; epoch 3: full `<table>` grid via `gallery-row`/`gallery-item` templates. |
| `list` | `;`-separated list items | - | Each item wrapped via `list-item`, then all wrapped via `list-container`. |
| `date-time` | Date/time text | Label | - |
| `link` | Link text | URL | - |
| `byline` | Author name | Date | - |
| `code-text` | Source code | Language/label | Passed through `highlight_code()` for syntax highlighting before rendering. |
| `table` | Rows as `cell|cell|...` lines, separated by `\n` | `"header"` flag (first row -> `<th>`) | Rows/cells are built manually into `<tr>/<td>`/`<th>` before substitution. |
| `separator` | - | Style param | Horizontal rule / divider. |
| `youtube-embed` | YouTube URL | Caption | epoch>=3: `<iframe>` embed. epoch 1/2: QR code image linking to a short URL. epoch 0: text-art QR (Unicode half-blocks). epoch -1 (WML): WBMP QR + short URL text link. |
| `form` (`form-start` / `form-end`) | Form action / submit-related text | HTTP method (default `post`) | Pair of blocks that wrap other input elements. |
| `button` (`button-primary` / `button-secondary`) | Button label | `type` attribute (default `submit`) | - |
| `input` (`input-text` / `password`) | Label/value | Field name / placeholder | - |
| `radio-button` | Label | Field name + value | - |
| `checkbox` | Label | Field name + value | - |

## 8. Rendering pipeline (request -> HTML)

See [diagrams/retro-center-page-render-flow.puml](diagrams/retro-center-page-render-flow.puml)
for the full sequence diagram. Summary for `GET /blog/<link>` or `GET /page/<link>`:

1. `handle_blog_entry` / `handle_page_entry` extract `<link>` from the URL and call
   `buildPageEntryWebSite(id, url, epoch, lang, theme)`.
2. **(epoch3 only)** `getBlogItemBySlug` / `getPageMetaBySlug` join `entries` + `header` +
   `translation` to fetch SEO metadata (title, summary, canonical URL, image, author,
   date), passed to `container_seo(...)` to build the page shell with OpenGraph tags.
   Other epochs (or if no metadata found) use the plain `container(epoch)` shell.
3. `menu(url, epoch, lang, theme)` builds the navigation.
4. `page(id, epoch, lang)`:
   - Loads `page-container_epoch<N>.html` and **every** `elements/<type>_epoch<N>.html`
     template up front.
   - Calls `getPageItems(id, &count, lang)` to get the ordered, translated content blocks
     (§4).
   - Iterates blocks in `order`, formatting each one against its type's template (§7),
     accumulating the result into a single buffer.
   - Wraps the accumulated blocks in `page-container_epoch<N>.html`.
5. The orchestrator combines container + menu + page fragment into the final HTML/WML
   document and returns it to the router, which sends it with epoch-appropriate headers.

## 9. Why this matters for Boat Rudder

Boat Rudder's CMS today (`src/modules/home_content`, `src/modules/home_blog`, etc.) uses
**static C arrays** as the data source, with one template per epoch per component - the
same templating mechanics (`generate_url_theme` + `read_file_to_string` +
`render_template`/`str_replace_first`) as the-retro-center.

The structure documented here shows how to extend that same templating mechanism to
**dynamic, database-backed pages**, by separating:
- **Routing/identity** (`entries`: slug, ordering, menu visibility) from
- **Metadata/SEO** (`header`: title, summary, image, author, date) from
- **Body content** (`content`: an ordered sequence of typed, independently-templated
  blocks) from
- **Translated text** (`translation`: language-neutral storage, resolved per request).

A future Boat Rudder increment that wants editable pages/posts could reuse this
collection split (and the per-type "element template" pattern in §7) without needing to
copy the-retro-center's specific element set - only the ones Boat Rudder actually needs
(e.g. `paragraph`, `tittle`, `image`, `byline`) would need templates per epoch.
