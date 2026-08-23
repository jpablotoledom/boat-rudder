# Entry Editor (`/dashboard/entries/<id>/edit`)

An AJAX content editor for one `entries` document - meta (link/type/enabled/categories),
header (cover image, date, per-language title/summary, read-only author) and `content[]` blocks
- with live preview, drag-and-drop reordering, rich-text editing, and optional autosave.
**`EPOCH_MODERN` only**, session-guarded via `require_dashboard_session_role()`.

---

## 1. Data model (`src/db/cms_entries_admin.h/.c`)

```c
typedef struct {
    char  *id;          // content[]._id as 24-char hex
    char  *type;        // one of the 14 block types - see §3
    int    order;
    char **text_values; // parallel to langs[] - exact text.<lang> ("" if absent)
    char  *extra_data;  // untranslated; heading level for "tittle", gallery _id for "gallery", ...
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
    char   *header_author_name;    // resolved from header.author_id -> users.name
    char    header_date[16];       // "YYYY-MM-DD", "" if absent

    CmsContentBlockEdit *content;  // sorted by order
    size_t content_count;
} CmsEntryEdit;
```

`cms_get_entry_for_edit(id_hex, langs, lang_count, &entry)` reads one `entries` document with
**no `enabled` filter** and **no language fallback** (exact `text.<lang>` per language, `""` if
absent). Eight functions cover the full lifecycle - see [dashboard.md, "Entry editor"](dashboard.md).

**The author is a reference, not translated text.** `entries.header.author_id` is an `ObjectId`
into `users`, set once at creation time; `cms_get_entry_for_edit()` resolves it to a single
`header_author_name` display string via `cms_get_user_name_by_id()`. It is rendered read-only in
the header sidebar and is not part of the `/header` save payload.

Every block type uses `render_extra_block()`, so they all receive `extra_data` in the editor
template (3 `%s`: block_id, lang_fields, extra_data).

---

## 2. Page rendering (`src/modules/entry_editor/`)

`entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count)` renders
`dashboard/entries/editor/container_epoch3.html` with:

- **Top bar** (fixed, full-width): back link, preview button, save-all button, save status,
  autosave toggle, publish toggle (wired to the hidden `enabled` checkbox), language tabs.
- **Block type toolbar** (fixed, second bar): quick-insert buttons for each supported block type.
- **Left sidebar**: meta section (`meta_epoch3.html` + `category-option_epoch3.html` per
  category) and header section (`header_epoch3.html` - image preview thumbnail, date input and
  a read-only author name - plus one `header-lang-tab_epoch3.html` per language, each holding
  that language's title and summary).
- **Right main area**: `blocks_epoch3.html` - the ordered list of content blocks and the
  "+ Add block" dropdown.

`lang-tab-button_epoch3.html` renders one language-switcher `<button data-lang="<code>">` per
language, displayed in the topbar. Clicking a language button calls `setLang(code)`, which
toggles every `.boat-rudder__entry-editor__block__lang-content[data-lang]` across the entire page (header sidebar +
every content block).

---

## 3. Block rendering (`src/modules/entry_editor/entry_editor_blocks.c`)

`entry_editor_render_block(&block, langs, lang_count, epoch)` renders one block's edit form.
Most types use `render_extra_block()` (3 `%s`: block_id, lang_fields, extra_data); `image` uses
`render_image_block()`, which swaps the last two (see below). These editor templates live under
`blocks/` and are **epoch 3 only**, consistent with the editor itself - unrelated to the public
templates, which now cover every epoch:

| Type | Template | Extra field |
|---|---|---|
| `tittle` | `blocks/tittle_epoch3.html` | heading level (1-6) |
| `paragraph` | `blocks/paragraph_epoch3.html` | style variant (`lead`, `note`, ...) |
| `image` | `blocks/image_epoch3.html` | `"<caption>\|<width>\|<align>"` |
| `byline` | `blocks/byline_epoch3.html` | date |
| `gallery` | `blocks/gallery_epoch3.html` | `media_galleries._id` |
| `separator` | `blocks/separator_epoch3.html` | style modifier |
| `link` | `blocks/link_epoch3.html` | target URL |
| `list` | `blocks/list_epoch3.html` | `"ol"` for ordered, else unordered |
| `table` | `blocks/table_epoch3.html` | `"header"` to make row 0 a header row |
| `code-text` | `blocks/code-text_epoch3.html` | caption above the code |
| `youtube-embed` | `blocks/youtube-embed_epoch3.html` | video URL |
| `image-paragraph` | `blocks/image-paragraph_epoch3.html` | `"left"` / `"right"` |
| `social-networks` | `blocks/social-networks_epoch3.html` | `"<icon>\|<url>"` |
| `generic` | `blocks/generic_epoch3.html` | unused |

These are the same 14 types `entry_page.c`'s `render_block()` renders publicly - the editor and
the public renderer are kept deliberately in sync. Unknown types render `""` in both.

Most blocks share `render_extra_block()`, which feeds the template `(id, lang_fields,
extra_data)` in that order. `image` uses its own `render_image_block()` because its template
needs `extra_data` **before** the language fields: the picture is what the body shows, so the
path inputs sit at the bottom inside the Advanced panel while the caption stays above them.

The `<textarea>` in `blocks/lang-field_epoch3.html` opens sized to its content -
`text_rows()` counts the lines and clamps to `BLOCK_TEXT_ROWS_MIN`..`MAX` (3..20). Past the
cap it scrolls; the CSS sets `overflow: auto` and `resize: vertical` so longer content is
always reachable.

### `image` block controls

The body shows the picture rather than its path, because opening a block hides the collapsed
preview (`.block--editing .block__preview { display: none }`) and reveals the body. Layout:

- **Header**: a `Select photo` button, next to the same-styled `Select photos` of `gallery`,
  so a picture can be swapped without opening the block.
- **Body**: the image itself (`renderImagePreview()`, requesting `_small`), then
  `Caption / alt text`, then `Size` (100/50/30 %) and `Alignment` (left/center/right).
- **Advanced**: the per-language image path, plus the raw `extra_data`.

The three controls read from and write back to the single hidden `extra_data` field via
`initImageBlock()` / `syncImageOptions()`, the same pattern `social-networks` uses for
`"<icon>|<url>"`. Paths are stored **bare**, with no size suffix - the public renderer picks
the variant per epoch (see [rendering.md](rendering.md)), so the media picker's value is
stored unchanged.

`blocks/lang-field_epoch3.html` - one `<div class="boat-rudder__entry-editor__block__lang-content" data-lang="<code>">` wrapping a `<textarea name="text">` per language.

`entry_editor_render_blocks(&entry, langs, lang_count, epoch)` renders all blocks and wraps them in `blocks_epoch3.html` (holds the block list + "+ Add block" dropdown).

---

## 4. Client-side editor (`container_epoch3.html` inline `<script>`)

No external `.js` file - same convention as `layout/layout_epoch3.html`. An IIFE reads
`data-entry-id` and `data-langs` off `#entryEditor` and wires up:

### Language switching
- `setLang(code)` - updates all `.boat-rudder__entry-editor__lang-btn` active states, calls `applyLangVisibility(document)` (shows only `[data-lang=code]` panels) and `refreshAllPreviews()`.

### Preview / edit mode
- `activateBlock(el)` - deactivates the previously active block, adds `.boat-rudder__entry-editor__block--editing` to the clicked block, calls `initBlockEditors(block)` (paragraph → rich text, title → heading buttons, gallery → thumbnail preview).
- `deactivateBlock(el)` - removes `.boat-rudder__entry-editor__block--editing`, calls `refreshBlockPreview(block)`.
- `refreshBlockPreview(blockEl)` - generates preview HTML for all 14 block types, reading the
  current language's text field and `extra_data`:
  - `tittle` → `<hN class="boat-rudder__entry-editor__preview__tittle">`, level from `extra_data`
  - `paragraph` → `<div class="boat-rudder__entry-editor__preview__paragraph">` with raw rich-text HTML
  - `image` → `<figure class="boat-rudder__entry-editor__preview__image"><img><figcaption>`
  - `byline` → `<div class="boat-rudder__entry-editor__preview__byline">`
  - `gallery` → inline thumbnail strip (`<img class="boat-rudder__entry-editor__preview__thumb">` per URL)
  - `separator` → an `<hr>`; `link` → an `<a>`; `list` → `<ul>`/`<ol>` per line
  - `code-text` → `<pre><code>` truncated to 200 chars; `generic` → raw HTML truncated to 300
  - `youtube-embed`, `table`, `social-networks` → compact summaries (URL, row count, icon/URL)
    rather than a real embed
  - Each type renders a `<span class="boat-rudder__entry-editor__preview__empty">` placeholder when its field is blank.
  Values are HTML-escaped for the preview only; `paragraph` and `generic` intentionally inject
  raw HTML, since that is what those blocks store.

### Rich text (paragraph blocks)
`initParagraphEditors(block)` - for each `.boat-rudder__entry-editor__block__lang-content` panel: creates a
`<div class="boat-rudder__entry-editor__richtext" contenteditable>` div above the hidden textarea, syncs content via
`syncRichtext()`. The rich-text toolbar (two rows: formatting + alignment/lists/source) is
injected via `insertAdjacentHTML`. Paste handler strips external styles/classes. `toggleSource(btn)` toggles between the contenteditable view and a raw `<textarea class="boat-rudder__entry-editor__source-editor">`.

### Heading levels (title blocks)
`initBlockHeadingBtns(block)` - reads `extra_data` and marks the matching H1-H6 button active.
`setHeadingLevel(btn, level)` - updates `extra_data`, refreshes preview.

### Gallery blocks
- `initGalleryBlock(block)` - reads the first lang's text field (semicolon URLs) and calls `renderGalleryThumbs()`.
- `renderGalleryThumbs(container, value)` - renders 56×56 px `<img class="boat-rudder__entry-editor__gallery-thumb">` draggable thumbnails using `_small` variant URLs.
- `initGalleryThumbDragDrop(container)` - HTML5 drag-and-drop to reorder thumbnails; on drop calls `syncGalleryInput()`.
- `syncGalleryInput(container)` - writes reordered URLs back to all lang text fields.
- `openGalleryForBlock(btn)` / `openImageGallery(btn)` - fetch `/dashboard/api/media/modal`, inject into `#modalContainer`, execute inline scripts so `selectDirectory`, `upload`, etc. work.

### Save functions
- `saveMeta()` - POSTs `link`, `type`, `enabled` (checkbox), `categories` (multi) to `.../meta`.
- `saveHeader()` - POSTs `image_url`, `date`, `hide_author`, `title_<lang>`, `summary_<lang>` to
  `.../header`. The author name is read-only and never submitted; `hide_author` is sent
  explicitly as `1`/`0`, because an unchecked checkbox submits nothing and the server could not
  otherwise tell "unchecked" from "field absent".
- `saveContent()` - POSTs `content_count` + per-block `id`/`type`/`order`/`extra_data`/`text_<lang>` to `.../content`, then copies the returned `ids` back into each block's `data-block-id`.

  **Blocks with an empty id are minted one, not dropped.** `cms_update_entry_content()` rewrites
  the whole `content[]` array, so a block it skips disappears from the document. It used to skip
  any block whose id was not a valid ObjectId, which meant an entry whose blocks predated
  `content[]._id` (imported content, for instance) lost its entire body on the first save while
  the API still answered `{"ok":true}`. Such a block now gets a fresh ObjectId, and the response
  carries the ids so the editor's `data-block-id` attributes stay in sync with what was stored -
  otherwise per-block delete and gallery edits would target the wrong block.
- `editorSaveAll()` - runs all three in parallel via `Promise.all()`.

### Other
- `togglePublish()` - toggles the hidden `enabled` checkbox + publish-switch UI.
- `toggleAutoSave()` - enables/disables the 3-second autosave timer.
- `insertNewComponent(type)` / `removeComponent(btn)` - POST to `.../blocks` / `.../blocks/<id>/delete`; patch `#entryBlocks` directly.
- `moveBlockUp/Down(btn)` - client-side DOM reorder; persisted on next `saveContent()`.
- Drag-and-drop block reorder - mousedown on `.boat-rudder__entry-editor__block__grip` or `.boat-rudder__entry-editor__block__drag`, ghost clone, placeholder indicator; reindexes order on mouseup.

---

## 5. Routes

Every route below is `EPOCH_MODERN` only (`302 /dashboard` otherwise) and passes through
`require_dashboard_session_role()`. The five `/api/` endpoints share one ownership gate:
`404 {"ok":false,"error":"not found"}` if `<id>` doesn't resolve via `cms_get_entry_for_edit()`,
`403 {"ok":false,"error":"forbidden"}` if `can_edit_entry(role, user_id, &entry)` is false (an
Autor may only edit their own `type:"blog"` entries - see
[dashboard.md, "Roles and privileges"](dashboard.md)).
All of them answer `application/json; charset=UTF-8`.

| Route | Method | Request → response |
|---|---|---|
| `/dashboard/entries/new` | `POST` | `cms_create_entry(user_id, type)` - `type` forced to `"blog"` for an Autor, `"page"` for an Administrador - then `302 .../edit` |
| `/dashboard/entries/<id>/edit` | `GET` | `entry_editor_page()` via `buildPageWebSite()`; `404` on a bad ObjectId, `302 /dashboard` if `!can_edit_entry()` |
| `/dashboard/entries/<id>/delete` | `POST` | `require_admin_session()` - Administrador only. `cms_delete_entry()`, `302 /dashboard` |
| `.../api/entries/<id>/meta` | `POST` | `link`, `enabled` (checkbox), `categories` (multi-value); `type` is read from the form for an Administrador (default `"page"`) and forced to `"blog"` for an Autor, so a post can never become a page. `cms_update_entry_meta()` → `{"ok":true}` \| `400 {"ok":false,"error":"update failed"}` |
| `.../api/entries/<id>/header` | `POST` | `image_url`, `date`, `hide_author` (`1`/`0`), and `title_<lang>`/`summary_<lang>` per `cms_get_languages()` entry. The author is **not** submitted (`header.author_id` is set at creation and shown read-only); `hide_author` only controls whether public views show the byline. `cms_update_entry_header()` → same shape |
| `.../api/entries/<id>/content` | `POST` | `content_count` (capped at 200) plus `content_<i>_id`/`_type`/`_order`/`_extra_data`/`_text_<lang>` per block → `CmsContentBlockEdit[]` → `cms_update_entry_content()` (full `content[]` replace) + gallery upsert (§6) → `{"ok":true,"ids":[...]}` (see below) |
| `.../api/entries/<id>/blocks` | `POST` | `type`, `order` → `cms_add_entry_content_block()` (`$push` of an empty block), rendered via `entry_editor_render_block()` → `{"ok":true,"block_id":"...","html":"..."}` (HTML escaped with `json_escape_alloc()`) \| `400 ... "create failed"` |
| `.../api/entries/<id>/blocks/<block_id>/delete` | `POST` | `cms_remove_entry_content_block()` (`$pull` by `_id`) → same shape (`"error":"remove failed"`) |

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

- The `image-single` block type (the last block type from the legacy editor not yet ported).
- A `/gallery/<slug>` human-readable URL (currently `_id` hex only).
