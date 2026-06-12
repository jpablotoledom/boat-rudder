# Entry Editor (`/dashboard/entries/<id>/edit`)

An AJAX content editor for one `entries` document - meta (link/type/enabled/categories),
header (image, date, per-language title/summary/author) and `content[]` blocks - with live
preview and optional autosave. It's the read/write counterpart to the read-only entries table
embedded in `/dashboard` (`src/modules/entries_admin/`), and is built the same way as the
Categories/Languages/Menu maintainers described in
[architecture.md](architecture.md#dashboard-maintainers-entries-categories-languages-and-menu)
- **`EPOCH_MODERN` only**, session-guarded via `require_dashboard_session()`.

Diagrams:

- [diagrams/entry-editor-components.puml](../diagrams/entry-editor-components.puml) - component
  diagram (router, `entry_editor`/`entry_editor_blocks`, `cms_entries_admin`, templates).
- [diagrams/sequence-entry-editor-edit-route.puml](../diagrams/sequence-entry-editor-edit-route.puml)
  - sequence diagram for `GET /dashboard/entries/<id>/edit`.
- [diagrams/sequence-entry-editor-ajax.puml](../diagrams/sequence-entry-editor-ajax.puml) -
  sequence diagram for the 5 `/dashboard/api/entries/<id>/...` AJAX endpoints.

It ports the-retro-center's epoch3 "trc-editor" to boat-rudder's single-document `entries`
schema (see `develop_docs/plans/cms-entry-model-plan.md`): one collection, `$set`/`$push`/`$pull`
on one document instead of a 5-table relational model with separate `headers`/`contents`/
`translations` tables.

---

## 1. Data model (`src/db/cms_entries_admin.h/.c`)

```c
typedef struct {
    char  *id;          // content[]._id as 24-char hex
    char  *type;        // "tittle" | "paragraph" | "image" | "byline"
    int    order;
    char **text_values; // parallel to langs[], exact text.<lang> ("" if absent)
    char  *extra_data;  // untranslated
} CmsContentBlockEdit;

typedef struct {
    char   *id;
    char   *link;
    char   *type;            // "page" | "blog"
    bool    enabled;
    char  **category_ids;    // entries.categories[] as hex strings
    size_t  category_count;

    char   *header_image_url;
    char  **header_title_values;   // parallel to langs[]
    char  **header_summary_values;
    char  **header_author_values;
    char    header_date[16];       // "YYYY-MM-DD", "" if absent

    CmsContentBlockEdit *content;  // sorted by order
    size_t content_count;
} CmsEntryEdit;
```

`cms_get_entry_for_edit(id_hex, langs, lang_count, &entry)` reads one `entries` document with
**no `enabled` filter** (admins must be able to edit disabled entries) and **no language
fallback** (`exact_lang_value()` per `langs[]` entry - every `map<lang,string>` field is read
exactly, "" if a key is absent). This is deliberately different from the public read path in
`cms_entries.c`, which filters on `enabled: true` and falls back to the default language.

Eight functions cover the full lifecycle:

| Function | Mongo operation |
|---|---|
| `cms_create_entry()` | `insertOne({link:"", type:"page", enabled:false, categories:[], header:{}, content:[]})` |
| `cms_get_entry_for_edit()` | `findOne({_id})`, no filters, exact per-language reads |
| `cms_update_entry_meta()` | `updateOne({_id}, {$set: {link, type, enabled, categories}})` |
| `cms_update_entry_header()` | `updateOne({_id}, {$set: {"header.*"}})` - `date` ("YYYY-MM-DD") parsed into a BSON UTC datetime, the inverse of `cms_entries.c`'s `resolve_header_fields()` |
| `cms_update_entry_content()` | `updateOne({_id}, {$set: {content: [...]}})` - full array replace |
| `cms_add_entry_content_block()` | `updateOne({_id}, {$push: {content: {_id: <new oid>, type, order, text: {}, extra_data: ""}}})` |
| `cms_remove_entry_content_block()` | `updateOne({_id}, {$pull: {content: {_id: block_id}}})` |
| `cms_delete_entry()` | `deleteOne({_id})` |

`cms_update_entry_content()` is a **full replace**: every `blocks[i].id` must already be a valid
hex id from `cms_add_entry_content_block()` - new blocks are always created individually via
`/blocks` *before* the next `saveContent()` call ever references them.

---

## 2. Page rendering (`src/modules/entry_editor/`)

- **`entry_editor.c`**: `entry_editor_page(epoch, &entry, categories, category_count, langs,
  lang_count)` renders `dashboard/entries/editor/container_epoch3.html` with:
  - a **meta sidebar** (`meta_epoch3.html` + one `category-option_epoch3.html` per category,
    "selected" if `category.id` is in `entry->category_ids[]`),
  - a **header sidebar** (`header_epoch3.html` + one `header-lang-tab_epoch3.html` per
    language - title/summary/author, plus an untranslated `image_url` and `date`),
  - a JSON array of language codes (e.g. `["en","es"]`) injected into `data-langs` for the
    inline editor script, and
  - the **blocks section** (below).

  `lang-tab-button_epoch3.html` renders one language-switcher `<button data-lang="<code>"
  onclick="setLang('<code>')">` per language - **one global set** of these buttons drives
  `setLang(code)`, which toggles every `.boat-rudder__entry-editor__lang-panel[data-lang]`
  in the document (header sidebar *and* every content block) at once.

- **`entry_editor_blocks.c`**:
  - `entry_editor_render_block(&block, langs, lang_count, epoch)` renders one `content[]`
    block's edit form: move-up/down/remove buttons, one `blocks/lang-field_epoch3.html` per
    language (bound to `text.<lang>`), a type-specific extra field for `image` ("Caption / alt
    text") and `byline` ("Date") bound to `extra_data`, and an empty `.block-preview` `<div>`
    filled client-side. Loads `blocks/<type>_epoch3.html` for `type` in `{tittle, paragraph,
    image, byline}`; unknown types render `""` (mirrors `entry_page.c`'s `render_block()`).
    **Reused** by `POST .../blocks` to render a brand-new (empty) block for the "add block" AJAX
    response - same markup either way.
  - `entry_editor_render_blocks(&entry, langs, lang_count, epoch)` renders every
    `entry->content[i]` and wraps them in `blocks_epoch3.html`, which also holds the
    "+ Add block" dropdown (one button per supported type, calling `insertNewComponent(type)`)
    and the "Save all"/autosave controls.

All templates live under `html/themes/dark/dashboard/entries/editor/` (epoch3 only, following
the no-embedded-HTML-in-C convention - every fragment is loaded via `generate_url_theme()` +
`read_file_to_string()` + `render_template()`).

---

## 3. Client-side editor (`container_epoch3.html`'s inline `<script>`)

No external `.js` file - same convention as `container/container_epoch3.html` /
`menu/menu_epoch3.html`. An IIFE reads `data-entry-id` and `data-langs` off `#entryEditor` and
wires up:

- **`setLang(code)`** - toggles `display` on every `.lang-panel[data-lang]` (header sidebar +
  every block) and re-runs `refreshAllPreviews()`.
- **`refreshBlockPreview(blockEl)`** - mirrors `entry_page.c`'s public renderers client-side:
  `tittle` -> `<h2>`, `paragraph` -> `<p>`, `image` -> `<figure><img><figcaption>`, `byline` ->
  two `<span>`s. Field values are HTML-escaped *for this preview only* - the data sent to the
  server and the public page itself follow the project's existing no-escaping convention for
  form fields.
- **`saveMeta()` / `saveHeader()` / `saveContent()`** - one `fetch(..., {method:'POST', body:
  new URLSearchParams(...)})` each, against the three `/dashboard/api/entries/<id>/...`
  endpoints below. `editorSaveAll()` runs all three (used by the "Save all" button and by
  autosave).
- **Autosave** - an "Autosave" checkbox; any field edit calls `setContentChanged()`, which (if
  autosave is on) (re)schedules `editorSaveAll()` 3 seconds after the last edit.
- **`insertNewComponent(type)` / `removeComponent(btn)`** - POST to `/blocks` /
  `/blocks/<block_id>/delete` and patch `#entryBlocks` directly, no full page reload.
- **`moveBlockUp(btn)` / `moveBlockDown(btn)`** - pure client-side DOM reorder (swap with the
  previous/next sibling). The new order is only persisted on the next `saveContent()`, where
  each block's `order` = its DOM index at save time.

---

## 4. Routes

| Route | Method | Behavior |
|---|---|---|
| `/dashboard/entries/new` | `POST` | `cms_create_entry()`, redirect to `/dashboard/entries/<new_id>/edit` (or `/dashboard` on failure). |
| `/dashboard/entries/<id>/edit` | `GET` | `404` if `<id>` is invalid/not found; otherwise `entry_editor_page()` embedded via `buildPageWebSite()`. |
| `/dashboard/entries/<id>/delete` | `POST` | `cms_delete_entry()`, redirect `/dashboard`. |
| `/dashboard/api/entries/<id>/meta` | `POST` | `link`, `type`, `enabled` (checkbox), `categories` (multi) -> `cms_update_entry_meta()`. JSON response. |
| `/dashboard/api/entries/<id>/header` | `POST` | `image_url`, `date`, `title_<lang>`/`summary_<lang>`/`author_<lang>` -> `cms_update_entry_header()`. JSON response. |
| `/dashboard/api/entries/<id>/content` | `POST` | `content_count` (capped at 200) + per-block `id`/`type`/`order`/`extra_data`/`text_<lang>` -> `cms_update_entry_content()`. JSON response. |
| `/dashboard/api/entries/<id>/blocks` | `POST` | `type`, `order` -> `cms_add_entry_content_block()` + `entry_editor_render_block()` -> `{"ok":true,"block_id":"...","html":"..."}`. |
| `/dashboard/api/entries/<id>/blocks/<block_id>/delete` | `POST` | `cms_remove_entry_content_block()`. JSON response. |

All routes: `epoch != EPOCH_MODERN` -> `302 /dashboard`; `require_dashboard_session()` -> `503`
(mongo down) / `302 /login` (no/invalid session) / proceed. The 5 `/dashboard/api/entries/...`
routes respond `application/json; charset=UTF-8` via `build_json_response()` /
`build_json_response_status()` (`src/utils/build_epoch_response.c`), with `{"ok":true,...}` /
`400 {"ok":false,"error":"..."}` bodies. `match_block_delete_route()` (`http_router.c`) is a
one-off two-id matcher for the `/blocks/<block_id>/delete` route, since `match_id_route()` only
handles a single `<id>` segment.

---

## 5. Future work

- The other 8 the-retro-center block types (gallery, image-single, image-paragraph,
  youtube-embed, code-text, list, table, link, separator, generic) each need an editor template
  + a new `render_*` in `entry_page.c` + a public element template.
- A media library (`media`/`media_directories` collections, upload endpoint, gallery-modal
  picker - "proposed, not implemented" per `develop_docs/plans/cms-entry-model-plan.md` §2.3).
  Until then, images are plain URL text inputs.
- Server-side code syntax highlighting, only relevant once `code-text` is added.
