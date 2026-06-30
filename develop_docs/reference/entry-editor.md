# Entry Editor (`/dashboard/entries/<id>/edit`)

An AJAX content editor for one `entries` document — meta (link/type/enabled/categories),
header (cover image, date, per-language title/summary/author) and `content[]` blocks — with
live preview, drag-and-drop reordering, rich-text editing, and optional autosave. **`EPOCH_MODERN`
only**, session-guarded via `require_dashboard_session()`.

---

## 1. Data model (`src/db/cms_entries_admin.h/.c`)

```c
typedef struct {
    char  *id;          // content[]._id as 24-char hex
    char  *type;        // "tittle" | "paragraph" | "image" | "byline" | "gallery"
    int    order;
    char **text_values; // parallel to langs[] — exact text.<lang> ("" if absent)
    char  *extra_data;  // untranslated; heading level for "tittle", gallery _id for "gallery"
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
**no `enabled` filter** and **no language fallback** (exact `text.<lang>` per language, `""` if
absent). Eight functions cover the full lifecycle — see `architecture.md` §"Entry editor".

All four block types (`tittle`, `paragraph`, `image`, `byline`) and the new `gallery` type use
`render_extra_block()`, so they all receive `extra_data` in the editor template (3 `%s`:
block_id, lang_fields, extra_data).

---

## 2. Page rendering (`src/modules/entry_editor/`)

`entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count)` renders
`dashboard/entries/editor/container_epoch3.html` with:

- **Top bar** (fixed, full-width): back link, preview button, save-all button, save status,
  autosave toggle, publish toggle (wired to the hidden `enabled` checkbox), language tabs.
- **Block type toolbar** (fixed, second bar): quick-insert buttons for each supported block type.
- **Left sidebar**: meta section (`meta_epoch3.html` + `category-option_epoch3.html` per
  category) and header section (`header_epoch3.html` + one `header-lang-tab_epoch3.html` per
  language — title/summary/author as separate tabs + image preview thumbnail + date input).
- **Right main area**: `blocks_epoch3.html` — the ordered list of content blocks and the
  "+ Add block" dropdown.

`lang-tab-button_epoch3.html` renders one language-switcher `<button data-lang="<code>">` per
language, displayed in the topbar. Clicking a language button calls `setLang(code)`, which
toggles every `.trc-block__lang-content[data-lang]` across the entire page (header sidebar +
every content block).

---

## 3. Block rendering (`src/modules/entry_editor/entry_editor_blocks.c`)

`entry_editor_render_block(&block, langs, lang_count, epoch)` renders one block's edit form.
All block types use `render_extra_block()` (3 `%s`: block_id, lang_fields, extra_data):

| Type | Template | Extra field |
|---|---|---|
| `tittle` | `blocks/tittle_epoch3.html` | `extra_data` = heading level (1-6) |
| `paragraph` | `blocks/paragraph_epoch3.html` | `extra_data` unused |
| `image` | `blocks/image_epoch3.html` | `extra_data` = caption/alt text |
| `byline` | `blocks/byline_epoch3.html` | `extra_data` = date |
| `gallery` | `blocks/gallery_epoch3.html` | `extra_data` = `media_galleries._id` |

`blocks/lang-field_epoch3.html` — one `<div class="trc-block__lang-content" data-lang="<code>">` wrapping a `<textarea name="text">` per language.

`entry_editor_render_blocks(&entry, langs, lang_count, epoch)` renders all blocks and wraps them in `blocks_epoch3.html` (holds the block list + "+ Add block" dropdown).

---

## 4. Client-side editor (`container_epoch3.html` inline `<script>`)

No external `.js` file — same convention as `container/container_epoch3.html`. An IIFE reads
`data-entry-id` and `data-langs` off `#entryEditor` and wires up:

### Language switching
- `setLang(code)` — updates all `.trc-lang-btn` active states, calls `applyLangVisibility(document)` (shows only `[data-lang=code]` panels) and `refreshAllPreviews()`.

### Preview / edit mode
- `activateBlock(el)` — deactivates the previously active block, adds `.trc-block--editing` to the clicked block, calls `initBlockEditors(block)` (paragraph → rich text, title → heading buttons, gallery → thumbnail preview).
- `deactivateBlock(el)` — removes `.trc-block--editing`, calls `refreshBlockPreview(block)`.
- `refreshBlockPreview(blockEl)` — generates preview HTML for each block type:
  - `tittle` → `<hN class="trc-preview__tittle">`, level from `extra_data`
  - `paragraph` → `<div class="trc-preview__paragraph">` with raw rich-text HTML
  - `image` → `<figure class="trc-preview__image"><img><figcaption>`
  - `byline` → `<div class="trc-preview__byline">`
  - `gallery` → inline thumbnail strip (`<img class="trc-preview__thumb">` per URL)

### Rich text (paragraph blocks)
`initParagraphEditors(block)` — for each `.trc-block__lang-content` panel: creates a
`<div class="trc-richtext" contenteditable>` div above the hidden textarea, syncs content via
`syncRichtext()`. The rich-text toolbar (two rows: formatting + alignment/lists/source) is
injected via `insertAdjacentHTML`. Paste handler strips external styles/classes. `toggleSource(btn)` toggles between the contenteditable view and a raw `<textarea class="trc-source-editor">`.

### Heading levels (title blocks)
`initBlockHeadingBtns(block)` — reads `extra_data` and marks the matching H1-H6 button active.
`setHeadingLevel(btn, level)` — updates `extra_data`, refreshes preview.

### Gallery blocks
- `initGalleryBlock(block)` — reads the first lang's text field (semicolon URLs) and calls `renderGalleryThumbs()`.
- `renderGalleryThumbs(container, value)` — renders 56×56 px `<img class="trc-gallery-thumb">` draggable thumbnails using `_small` variant URLs.
- `initGalleryThumbDragDrop(container)` — HTML5 drag-and-drop to reorder thumbnails; on drop calls `syncGalleryInput()`.
- `syncGalleryInput(container)` — writes reordered URLs back to all lang text fields.
- `openGalleryForBlock(btn)` / `openImageGallery(btn)` — fetch `/dashboard/api/media/modal`, inject into `#modalContainer`, execute inline scripts so `selectDirectory`, `upload`, etc. work.

### Save functions
- `saveMeta()` — POSTs `link`, `type`, `enabled` (checkbox), `categories` (multi) to `.../meta`.
- `saveHeader()` — POSTs `image_url`, `date`, `title_<lang>`, `summary_<lang>`, `author_<lang>` to `.../header`.
- `saveContent()` — POSTs `content_count` + per-block `id`/`type`/`order`/`extra_data`/`text_<lang>` to `.../content`.
- `editorSaveAll()` — runs all three in parallel via `Promise.all()`.

### Other
- `togglePublish()` — toggles the hidden `enabled` checkbox + publish-switch UI.
- `toggleAutoSave()` — enables/disables the 3-second autosave timer.
- `insertNewComponent(type)` / `removeComponent(btn)` — POST to `.../blocks` / `.../blocks/<id>/delete`; patch `#entryBlocks` directly.
- `moveBlockUp/Down(btn)` — client-side DOM reorder; persisted on next `saveContent()`.
- Drag-and-drop block reorder — mousedown on `.trc-block__grip` or `.trc-block__drag`, ghost clone, placeholder indicator; reindexes order on mouseup.

---

## 5. Routes

| Route | Method | Behavior |
|---|---|---|
| `/dashboard/entries/new` | `POST` | `cms_create_entry()`, redirect to `.../edit` |
| `/dashboard/entries/<id>/edit` | `GET` | `entry_editor_page()` via `buildPageWebSite()` |
| `/dashboard/entries/<id>/delete` | `POST` | `cms_delete_entry()`, redirect `/dashboard` |
| `/dashboard/api/entries/<id>/meta` | `POST` | `cms_update_entry_meta()`. JSON response. |
| `/dashboard/api/entries/<id>/header` | `POST` | `cms_update_entry_header()`. JSON response. |
| `/dashboard/api/entries/<id>/content` | `POST` | `cms_update_entry_content()` + gallery upsert. JSON response. |
| `/dashboard/api/entries/<id>/blocks` | `POST` | `cms_add_entry_content_block()` + render. `{"ok":true,"block_id":"...","html":"..."}` |
| `/dashboard/api/entries/<id>/blocks/<block_id>/delete` | `POST` | `cms_remove_entry_content_block()`. JSON response. |

All routes: `epoch != EPOCH_MODERN` → `302 /dashboard`. Session guard via `require_dashboard_session_role()`.

---

## 6. Gallery block and media integration

When the content is saved (`.../content` POST), the router detects gallery blocks and calls
`cms_upsert_media_gallery(existing_gallery_id_or_null, entry_id, urls_csv, out_id)`. This
creates or updates a `media_galleries` document (BSON array of URLs) and writes the resulting
`_id` back into `content[].extra_data`. On the next save the existing `_id` is updated in-place.

The media picker modal (`/dashboard/api/media/modal`) returns the full
`dashboard/media/media_epoch3.html` wrapped in `.boat-rudder__modal`. Its inline JS sets selected
URLs into the target block's text fields (gallery) or the header `image_url` input (header cover).
`activateScripts(container)` re-executes all `<script>` tags after `innerHTML` injection so that
`selectDirectory`, `upload`, and other media functions are available.

---

## 7. Future work

- Remaining the-retro-center block types: `image-single`, `image-paragraph`, `youtube-embed`,
  `code-text`, `list`, `table`, `separator`, `link`, `generic`.
- A `/gallery/<slug>` human-readable URL (currently `_id` hex only).
