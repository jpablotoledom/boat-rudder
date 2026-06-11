# CMS "Entry" Data Model - Embedded Schema Plan

> **Status**: design proposal / planning document, not implemented. This is documentation
> only - it does not change any code in this repository.
>
> **Context**: [retro-center-entry-model.md](retro-center-entry-model.md) documents how
> `../the-retro-center` models a CMS page ("entry") across **7 normalized collections**
> (`entries`, `header`, `content`, `translation`, `entry_categories`, `media`,
> `media_directories`), joined at read time via `$lookup`/`$unwind` aggregation
> pipelines. This document proposes an **embedded** variant of the same data, for a
> possible future Boat Rudder CMS increment, that keeps the same information but
> reduces the collection count and removes the join-heavy read path.

Diagram: [diagrams/cms-entry-model-embedded.puml](diagrams/cms-entry-model-embedded.puml)

## 1. Goal

Keep the same conceptual data (a page = identity/routing info + header/SEO metadata +
an ordered list of typed content blocks + multi-language text + categories + media
galleries), but **embed** the pieces that are always read and written together as part
of their parent document, instead of storing them as separate collections joined by
`ObjectId` references.

Target shape (as given):

```
entries
  - header
      - translation
  - content
      - translation
entry_categories
  - translation
media
media_directories
```

i.e. **7 collections -> 4 collections**:

| the-retro-center (normalized) | Proposed (embedded) |
|---|---|
| `entries` | `entries` (now includes `header` and `content`) |
| `header` | embedded as `entries.header` |
| `content` | embedded as `entries.content[]` |
| `translation` | embedded inline as `{eng, esp}` wherever it was referenced |
| `entry_categories` | `entry_categories` (now includes its `translation`) |
| `media` | `media` (unchanged) |
| `media_directories` | `media_directories` (unchanged) |

### 1.1 Language code convention

The original analysis used `eng`/`esp` as the keys for translated text. To support an
arbitrary number of languages without changing the schema, every embedded translation
object should instead be an **open map keyed by [ISO 639-1](https://en.wikipedia.org/wiki/ISO_639-1)
two-letter lowercase language codes** (`en`, `es`, `fr`, `de`, `pt`, ...):

```jsonc
{ "en": "Hello", "es": "Hola", "fr": "Bonjour" }
```

- Adding a new language is just adding a new key - no schema/migration needed.
- A document doesn't need to populate every language; the rendering layer falls back
  to a configured default (e.g. `en`) if a key is missing for the requested `lang`.
- If region-specific variants are ever needed (e.g. `en-US` vs. `en-GB`), extend to
  [BCP 47](https://en.wikipedia.org/wiki/IETF_language_tag) tags - they're a superset
  of ISO 639-1, so plain `en`/`es` keys remain valid.
- This replaces `eng`/`esp` everywhere in this document (`en`/`es` below). The
  the-retro-center-specific [retro-center-entry-model.md](retro-center-entry-model.md)
  is left as-is, since it describes that project's *current* code (`translation.eng` /
  `translation.esp`).

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
    "author":  { "en": "Pablo",          "es": "Pablo" },
    "date": ISODate("2026-06-01T00:00:00Z")
  },

  "content": [
    {
      "_id": ObjectId,
      "type": "tittle",
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

`text`/`name`/`title`/`summary`/`author` are all maps following §1.1 - shown above with
`en`/`es` populated, but any subset of ISO 639-1 codes is valid per document.

Field-by-field mapping from the normalized model:

| Embedded field | Replaces (normalized) |
|---|---|
| `entries.name` (map) | `entries.name_id` -> `translation.{eng,esp}` |
| `entries.header.*` | the entire `header` document (`entry_id` link is implicit - it's the parent doc) |
| `entries.header.title` (map) | `header.tittle_id` -> `translation.{eng,esp}` |
| `entries.header.summary` (map) | `header.summary_id` -> `translation.{eng,esp}` |
| `entries.header.author` (map) | `header.author_id` -> `translation.{eng,esp}` |
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

### 3.1 Fetching a full page (was: `getEntry` + `getHeader`/`getBlogItemBySlug` + `getPageItems`, 3 queries with aggregation `$lookup`/`$unwind`)

**Before** (the-retro-center): 3 separate aggregation pipelines, each doing
`$lookup` + `$unwind` against `translation` (and `entries` for `content`).

**After**: a single query.

```js
db.entries.findOne({ link: "<link>", enabled: true })
```

Everything needed to render the page - routing info, header/SEO metadata, and all
content blocks with their translations - comes back in one document. Content blocks
are already an array; sort by `content[].order` in application code (they can also be
stored pre-sorted, since they're rewritten as a whole array on every edit anyway).

### 3.2 Listing blog cards (was: `getBlogItems`, joins `entries` + `header` + `translation`)

**After**:

```js
db.entries.find(
  { type: "blog", enabled: true, menu: true },
  { link: 1, name: 1, header: 1, categories: 1 }
).sort({ "header.date": -1 })
```

No `$lookup` needed - `header` (with its embedded title/summary/author/date) is already
part of the projected document. If category **names** are needed for the card (not just
ids), that's the one remaining join: `entry_categories` by `categories[]`.

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
  current `MAX_TITTLE_LENGTH`/`MAX_SUMMARY_LENGTH`/`MAX_CONTENT_LENGTH` of 50000 chars
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

This schema is **not** wired into Boat Rudder yet. If/when a future increment adds
database-backed pages (beyond the current static `home_content`/`home_blog` arrays),
this is the proposed shape for the new `entries` collection. The per-type "element"
templates described in [retro-center-entry-model.md §7](retro-center-entry-model.md#7-content-block-types-elements)
remain the rendering pattern - only the *storage* shape changes; rendering still goes
through `generate_url_theme` + `read_file_to_string` + `render_template` per
`content[].type`/epoch, per Boat Rudder's [no-embedded-HTML convention](architecture.md#templates-htmlthemestheme).
