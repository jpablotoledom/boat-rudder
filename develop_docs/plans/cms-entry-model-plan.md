# CMS "Entry" Data Model - Embedded Schema

> **Status**: fully implemented, with one deviation from this document. `entries` (with embedded
> `header` and `content[]`), `entry_categories`, `media` and `media_directories` all exist - see
> [rendering.md, "CMS entries"](../reference/rendering.md) and
> [media-admin.md](../reference/media-admin.md).
>
> **Deviation:** `header.author` was specified here as a `map<lang,string>` (§2.1). It is
> implemented instead as `header.author_id`, an `ObjectId` reference into `users`, resolved to
> `users.name` at render time. An author's name is a proper noun, not translated content, so a
> per-language map was the wrong shape - and the reference keeps the name in one place when a
> user is renamed.

Diagram: [diagrams/cms-entry-model-embedded.puml](../diagrams/cms-entry-model-embedded.puml)

## 1. Goal

A page ("entry") is identity/routing info + header/SEO metadata + an ordered list of typed
content blocks + multi-language text + categories + (eventually) media galleries. Pieces
that are always read and written together as a unit are **embedded** in one `entries`
document, instead of being split across separate collections joined by `ObjectId`
references; only `entry_categories` (and, in future, `media`/`media_directories`) stay
separate, since they're shared many-to-many across entries.

Target shape:

```
entries
  - header
  - content[]
entry_categories
media
media_directories
```

4 collections total:

| Collection | Contents |
|---|---|
| `entries` | page identity/routing + embedded `header` + embedded `content[]`, all translated text as `map<lang,string>` |
| `entry_categories` | `{ _id, name: map<lang,string> }`, referenced from `entries.categories[]` |
| `media` | uploaded file metadata (§2.3) - implemented, see [media-admin.md](../reference/media-admin.md) |
| `media_directories` | folder structure (§2.3) - implemented, see [media-admin.md](../reference/media-admin.md) |

### 1.1 Language code convention

Every embedded translation object is an **open map keyed by
[ISO 639-1](https://en.wikipedia.org/wiki/ISO_639-1) two-letter lowercase language codes**
(`en`, `es`, `fr`, `de`, `pt`, ...):

```jsonc
{ "en": "Hello", "es": "Hola", "fr": "Bonjour" }
```

- Adding a new language is just adding a new key - no schema/migration needed.
- A document doesn't need to populate every language; the rendering layer falls back
  to a configured default (`en`) if a key is missing for the requested `lang`. This is
  implemented by `resolve_lang_map()` in `src/db/cms_entries.c`, which also bridges
  `configs/settings.conf`'s `lang="Eng"/"Esp"` convention to `en`/`es` (see
  [rendering.md, "CMS entries"](../reference/rendering.md)).
- If region-specific variants are ever needed (e.g. `en-US` vs. `en-GB`), extend to
  [BCP 47](https://en.wikipedia.org/wiki/IETF_language_tag) tags - they're a superset
  of ISO 639-1, so plain `en`/`es` keys remain valid.

## 2. Proposed schema

### 2.1 `entries`

One self-contained document per page. Replaces `entries` + `header` + `content` +
all the `translation` documents those referenced.

```jsonc
{
  "_id": ObjectId,
  "link": "my-page-slug",
  "type": "blog",            // "blog" | "page" | ...
  "order": 10,
  "enabled": true,
  "menu": true,
  "name": { "en": "My Page", "es": "Mi Página" },

  "categories": [ObjectId, ObjectId],   // -> entry_categories._id (unchanged)

  "header": {
    "image_url": "/themes/dark/assets/blog/my-page.jpg",
    "title":   { "en": "My Page Title",  "es": "Título de mi página" },
    "summary": { "en": "A short teaser", "es": "Un resumen corto" },
    "author_id": ObjectId,               // -> users._id, resolved to users.name at render time
                                          // (this document originally specified a map; see Status)
    "date": ISODate("2026-06-01T00:00:00Z")
  },

  "content": [
    {
      "_id": ObjectId,
      "type": "title",
      "order": 0,
      "text": { "en": "Section title", "es": "Título de sección" },
      "extra_data": "1"
    },
    {
      "_id": ObjectId,
      "type": "paragraph",
      "order": 1,
      "text": { "en": "Lorem ipsum...", "es": "Lorem ipsum (esp)..." },
      "extra_data": ""
    },
    {
      "_id": ObjectId,
      "type": "gallery",
      "order": 2,
      "text": { "en": "img1.jpg;img2.jpg;img3.jpg", "es": "img1.jpg;img2.jpg;img3.jpg" },
      "extra_data": "<media_directories._id as gallery id>"
    }
  ]
}
```

`text`/`name`/`title`/`summary` are all maps following §1.1 - shown above with
`en`/`es` populated, but any subset of ISO 639-1 codes is valid per document. `author_id` is the
exception: a plain `ObjectId` reference, not a map (see Status above).

Field-by-field mapping from the normalized model:

| Embedded field | Replaces (normalized) |
|---|---|
| `entries.name` (map) | `entries.name_id` -> `translation.{eng,esp}` |
| `entries.header.*` | the entire `header` document (`entry_id` link is implicit - it's the parent doc) |
| `entries.header.title` (map) | `header.title_id` -> `translation.{eng,esp}` |
| `entries.header.summary` (map) | `header.summary_id` -> `translation.{eng,esp}` |
| `entries.header.author_id` (ObjectId -> `users._id`) | `header.author_id` -> `translation.{eng,esp}` (kept as a reference instead of flattening to a map - see Status) |
| `entries.content[]` | the `content` collection (`entry_id` link is implicit) |
| `entries.content[].text` (map) | `content.content_id` -> `translation.{eng,esp}` |
| `entries.content[].order` | `content.order` (sort key, now sorted client-side or via `$unwind`+`$sort` only if needed for cross-entry queries) |
| `entries.content[]._id` | new - stable id per block, useful for editing/reordering in a future dashboard editor |

Unchanged: `_id`, `link`, `type`, `order`, `enabled`, `menu`, `categories[]`.

### 2.2 `entry_categories`

```jsonc
{
  "_id": ObjectId,
  "name": { "en": "Tutorials", "es": "Tutoriales" }
}
```

Replaces `entry_categories` (which had `category_id -> translation`) by embedding the
category's name directly. `entries.categories[]` still references
`entry_categories._id` - **this collection stays separate** because categories are
shared many-to-many across entries (embedding the category name into every `entries`
document that uses it would duplicate and risk drifting if a category is renamed).

### 2.3 `media` / `media_directories`

Unchanged. Still standalone collections, referenced from
`entries.content[].extra_data` when `content[].type == "gallery"`.

## 3. Query pattern changes

### 3.1 Fetching a full page

A single query, with no `$lookup`/`$unwind` pipeline needed:

```js
db.entries.findOne({ link: "<link>", enabled: true })
```

Everything needed to render the page - routing info, header/SEO metadata, and all
content blocks with their translations - comes back in one document. Content blocks
are already an array; sort by `content[].order` in application code (they can also be
stored pre-sorted, since they're rewritten as a whole array on every edit anyway).

### 3.2 Listing blog cards (was: `getBlogItems`, joins `entries` + `header` + `translation`)

**Implemented** - `cms_get_blog_entries()` in `src/db/cms_entries.c`, used by the home blog
list (`src/modules/home_blog/home_blog.c`, see `develop_docs/reference/architecture.md` "Home
blog list"):

```js
db.entries.find({ type: "blog", enabled: true })
  .sort({ "header.date": -1 })
  .limit(HOME_BLOG_LIMIT)
```

No `$lookup` needed - `header` (with its embedded title/summary/author/date) is already part
of the document. Category **names** are resolved with the same `entry_categories` `$in` lookup
used by `cms_get_entry_by_link()` (`resolve_category_names()`). There is no `menu` field on
`entries`; menu-driven navigation is deferred to a future separate `menu` collection (see
[home-blog-list-plan.md](home-blog-list-plan.md) §6).

`cms_get_blog_entries()` is parameterized by `limit`, reused by both the home blog list
(`HOME_BLOG_LIMIT`) and the full `/blog` listing (`BLOG_LIST_LIMIT`) - see
[rendering.md, "Blog list"](../reference/rendering.md). Category-filtered
listing (`getBlogItemsByCategory`-style queries) remains a future increment.

### 3.3 Editing a page (dashboard / future editor)

**Before**: editing a page meant writing to up to `1 + 1 + N + (2 + N)` documents
across 3 collections (`entries`, `header`, `content` x N blocks, `translation` x
(name + title + summary + author + N block texts)).

**After**: a single `replaceOne`/`updateOne` on the `entries` document. Reordering,
adding, or removing content blocks is just array manipulation within one document.

## 4. Trade-offs

**Pros**
- One read per page render (`findOne` by `link`) instead of 3 aggregation pipelines
  with `$lookup`/`$unwind`.
- One write per page edit instead of writes spread across 3 collections.
- Simpler application code: no `$lookup`/`$unwind`/`$sort` pipeline construction
  (`get_entry.c`, `get_header.c`, `page_items.c` collapse into one `getEntryByLink()`).
- Content blocks naturally keep their order as an array - no separate `order` field
  needed for storage (though keeping it can still help with stable diffing in an
  editor UI).

**Cons / things to watch**
- **Document size**: MongoDB's 16 MB document limit applies per `entries` document now.
  Not a practical concern for typical text + image-filename content, but a page with an
  enormous number of large `code-text` blocks could theoretically approach it (the
  current `MAX_title_LENGTH`/`MAX_SUMMARY_LENGTH`/`MAX_CONTENT_LENGTH` of 50000 chars
  each, times many blocks, is still far below 16 MB).
- **Updating a single content block** requires an array-element update
  (`db.entries.updateOne({_id, "content._id": blockId}, {$set: {"content.$.text.en": ...}})`)
  rather than a simple document replace - slightly more complex than updating a
  standalone `content` document, but still a single round trip.
- **No cross-entry text reuse**: in the normalized model, a `translation` document is
  *referenced*, so in principle the same translation could be shared (in practice it
  wasn't - each field got its own `translation` doc 1:1). Embedding makes this
  explicitly "no sharing", which matches actual usage and removes orphaned
  `translation` documents as a failure mode.
- **`entry_categories` stays a separate collection** specifically because it *is*
  shared many-to-many; this is the one place normalization is kept, to avoid
  duplicating/drifting category names across every entry that uses them.

## 5. Open questions for whoever implements this

1. **Content block `_id`**: needed for a future block-level editor (stable identity for
   reorder/edit/delete), but unused for read-only rendering. Include it now (cheap) or
   add it when the editor is built?
2. **`content[].order`**: keep an explicit field (lets the array be stored/edited
   out-of-order and sorted on read/write), or rely purely on array position? Explicit
   `order` is more forgiving for concurrent edits/migrations.
3. **Category names on listing pages**: embed category names into `entries.categories[]`
   too (e.g. `[{ "_id": ObjectId, "name": {en, es, ...} }]`) to avoid the one remaining join
   on blog-listing pages, at the cost of needing to update every entry when a category
   is renamed? (Renames are presumably rare; the join is presumably cheap. Leaning
   towards **not** embedding, per §2.2.)
4. **Indexes**: `entries.link` (unique), `entries.type` + `entries.enabled` +
   `entries.menu` (for menu/listing queries), `entries.header.date` (for blog ordering).

## 6. Relationship to Boat Rudder's current code

Everything described above is implemented and wired into the `/page/<link>`, `/blog/<link>`,
`/blog` and `/blog/category/<slug>` routes - see
[rendering.md, "CMS entries"](../reference/rendering.md),
`src/db/cms_entries.c`, and `src/modules/entry_page/entry_page.c`. Fourteen `content[].type`s
are implemented; heading levels via `content[].extra_data` for `title`, category filtering of
the blog listing, and `media`/`media_directories` (§2.3) all landed as well.

Still open: the `image-single` element type, and older-epoch (-1..2) templates for the nine
block types that currently only have an epoch 3 variant. When these land, rendering follows the
same pattern as the implemented types: per-type "element" templates loaded via
`generate_url_theme` + `read_file_to_string` + `render_template` per `content[].type`/epoch, per
Boat Rudder's
[rendering.md, "Templates"](../reference/rendering.md).
