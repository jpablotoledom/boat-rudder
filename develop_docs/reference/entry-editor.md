# Entry Editor (`/dashboard/entries/<id>/edit`)

An AJAX content editor for one `entries` document - meta (link/type/enabled/categories),
header (cover image, date, per-language title/summary, read-only author) and `content[]` blocks
- written WYSIWYG-style on a sheet drawn like the published entry, with one context bar for the
open block's tools, a keyboard model that crosses blocks, a `/` menu and Markdown shortcuts,
drag-and-drop reordering and optional autosave.
**`EPOCH_MODERN` only** (with the rules' dashboard flag - see [dashboard.md](dashboard.md#routes)), session-guarded via `require_dashboard_session_role()`.

Diagrams: [entry-editor-components.puml](../diagrams/entry-editor-components.puml),
[sequence-entry-editor-edit-route.puml](../diagrams/sequence-entry-editor-edit-route.puml),
[sequence-entry-editor-ajax.puml](../diagrams/sequence-entry-editor-ajax.puml).

---

## 1. Data model (`src/db/cms_entries_admin.h/.c`)

```c
typedef struct {
    char  *id;          // content[]._id as 24-char hex
    char  *type;        // one of the 13 block types - see §3
    int    order;
    char **text_values; // parallel to langs[] - exact text.<lang> ("" if absent)
    char  *extra_data;  // untranslated; heading level for "title", gallery _id for "gallery", ...
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
    bool    header_hide_author;    // header.hide_author
    char   *created_by;            // entries.created_by as hex, "" if absent

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
the settings panel and is not part of the `/header` save payload.

Every block type uses `render_extra_block()`, so they all receive `extra_data` in the editor
template (3 `%s`: block_id, lang_fields, extra_data).

---

## 2. Page rendering (`src/modules/entry_editor/`)

`entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count)` renders
`dashboard/entries/editor/container_epoch3.html` with:

- **Top bar** (fixed, 44px): back link, *View post*, *Save all*, save status, autosave toggle,
  *Translate*, publish toggle (wired to the hidden `enabled` checkbox), the theme `<select>`,
  language tabs and **⚙ Settings**. Under 600px it scrolls sideways instead of squeezing its
  controls.
- **Theme** (`#editorTheme`, only with two themes or more): `entry_editor.c` puts every theme in
  the root's `data-themes` and the one in use in `data-theme`. Choosing one goes through
  `/theme/set?key=<key>&return=<this editor>` - the site navbar's switch, so the dashboard and
  the site both change - which reloads the editor. `switchEditorTheme()` saves unsaved work first
  (`editorSaveAll()`; when that fails the `<select>` goes back and nothing switches) and leaves the
  language tab and scroll in `sessionStorage` (`br-editor-theme-restore`), which
  `restoreAfterThemeSwitch()` puts back once on load.
- **Context bar** (`#editorContext`, fixed, 40px under the top bar): **+ Insert**, the open
  block's type (a `<select>` for paragraph / heading / list, see [Block types](#block-types-switching--menu-markdown-shortcuts)),
  a slot for the open block's tools (`#editorContextTools`) and its actions (Split, Merge,
  move up/down, Advanced, delete) - see [Context bar](#context-bar).
- **Settings panel** (`#editorSettings`, `.boat-rudder-entry-editor__sidebar`): the meta section
  (`meta_epoch3.html` + `category-option_epoch3.html` per category) and the post header
  (`header_epoch3.html` - cover image, date, read-only author - plus one
  `header-lang-tab_epoch3.html` per language with its title and summary). See
  [Settings panel](#settings-panel).
- **Sheet** (`.boat-rudder-entry-editor__main`): `blocks_epoch3.html` - the ordered list of
  content blocks and "+ Add block" - see [The sheet](#the-sheet).

`lang-tab-button_epoch3.html` renders one language-switcher `<button data-lang="<code>">` per
language, displayed in the topbar. Clicking a language button calls `setLang(code)`, which
toggles every `.boat-rudder-entry-editor-block__lang-content[data-lang]` across the entire page
(settings panel + every content block).

---

## 3. Block rendering (`src/modules/entry_editor/entry_editor_blocks.c`)

`entry_editor_render_block(&block, langs, lang_count, epoch)` renders one block's edit form.
Most types use `render_extra_block()` (3 `%s`: block_id, lang_fields, extra_data); `image` uses
`render_image_block()`, which swaps the last two (see below). These editor templates live under
`blocks/` and are **epoch 3 only**, consistent with the editor itself - unrelated to the public
templates, which now cover every epoch:

| Type | Template | Extra field |
|---|---|---|
| `title` | `blocks/title_epoch3.html` | heading level (1-6) |
| `paragraph` | `blocks/paragraph_epoch3.html` | style variant (`lead`, `note`) |
| `image` | `blocks/image_epoch3.html` | `"<caption>\|<width>\|<align>"`, align incl. `float-left`/`float-right` |
| `byline` | `blocks/byline_epoch3.html` | date |
| `gallery` | `blocks/gallery_epoch3.html` | `media_galleries._id` |
| `separator` | `blocks/separator_epoch3.html` | style modifier |
| `link` | `blocks/link_epoch3.html` | target URL |
| `list` | `blocks/list_epoch3.html` | `"ol"` for ordered, else unordered |
| `table` | `blocks/table_epoch3.html` | `"header"` to make row 0 a header row |
| `code-text` | `blocks/code-text_epoch3.html` | language (`<select>` grouped Web / Languages / Retro / Data and shell - the keys are in [code-highlighting.md](code-highlighting.md#languages)) |
| `terminal` | `blocks/terminal_epoch3.html` | shell (`<select>`: Linux / Unix `""`, MS-DOS / Windows `dos`, PowerShell `powershell`) - see [rendering.md](rendering.md#terminal-block) |
| `youtube-embed` | `blocks/youtube-embed_epoch3.html` | video URL |
| `social-networks` | `blocks/social-networks_epoch3.html` | `"<icon>\|<url>"` |
| `generic` | `blocks/generic_epoch3.html` | unused |

These are the same 13 types `entry_page.c`'s `render_block()` renders publicly - the editor and
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

The open block's body shows the picture rather than its path. Layout:

- **Context bar** (its `.boat-rudder-entry-editor-block__tools` row): **Select photo**, `Size`
  (100/50/30 %) and `Alignment` (left/center/right, or *Float left/right* - text after the image
  wraps around it). `gallery`'s tools row has **+ Add photos**.
- **Body**: the image itself (`renderImagePreview()`, requesting `_small`) - a click on it opens
  the media library too - then `Caption / alt text`.
- **Advanced**: the per-language image path, plus the raw `extra_data`.

The three controls read from and write back to the single hidden `extra_data` field via
`initImageBlock()` / `syncImageOptions()`, the same pattern `social-networks` uses for
`"<icon>|<url>"`. Paths are stored **bare**, with no size suffix - the public renderer picks
the variant per epoch (see [rendering.md](rendering.md)), so the media picker's value is
stored unchanged.

`blocks/lang-field_epoch3.html` - one `<div class="boat-rudder-entry-editor-block__lang-content" data-lang="<code>">` wrapping a `<textarea name="text">` per language.

`entry_editor_render_blocks(&entry, langs, lang_count, epoch)` renders all blocks and wraps them in `blocks_epoch3.html` (holds the block list + the block menu shared by "+ Add block" and the context bar's "+ Insert").

---

## 4. Client-side editor (`container_epoch3.html` inline `<script>`)

No external `.js` file - same convention as `layout/layout_epoch3.html`. An IIFE reads
`data-entry-id` and `data-langs` off `#entryEditor` and wires up:

### Language switching
- `setLang(code)` - updates all `.boat-rudder-entry-editor__lang-btn` active states, calls `applyLangVisibility(document)` (shows only `[data-lang=code]` panels) and `refreshAllPreviews()`.

### The sheet

The content area is drawn as the published entry is (`styles_epoch3.css`'s
`.boat-rudder-page-entry`): a column at most 980px wide in `--br-color-page-content-background`
over `--br-color-body-background`, with the page's text color, so writing looks like reading.
Blocks have **no border and no padding of their own**, so a heading's margin and the next
paragraph's collapse as on the page. Sizes follow the public sheet: paragraphs and lists 18px /
26px with 16px margins, headings `h1`-`h3` from `styles_epoch3.css` (32 / 24 with its underline /
20px) and the browser's own for `h4`-`h6`. "+ Add block" closes the column and stretches it to
the bottom of the screen. A drag grip (`.boat-rudder-entry-editor-block__grip`) shows in the
left margin on hover (hidden under 600px).

### Inline and open blocks

- **Paragraphs and titles are always editable** (`initInlineBlock()`, run for every block on
  load and for each new one in `createBlockAfter()`): a paragraph gets its rich-text editor at
  once, a title's textarea is drawn as its heading (`data-level` on the block picks the size)
  and grows with its text (`autosizeTitle()`, re-run by `setLang()`, since only the visible
  language's box can be measured). They have no preview (`isInlineType()`;
  `refreshBlockPreview()` returns early for them) and no outline when open. An open paragraph
  or title does **not** clear floats, so it stays wrapped around a floated image above, as on
  the page.
- **Every other block** shows its preview until clicked; then it is *open*:
  `.boat-rudder-entry-editor-block--editing` hides the preview, shows the body and draws an
  accent outline (no card, no header - the block's own header markup is hidden; only its type
  label is read, by the context bar). An open form clears floats so its fields get the full width.
- `activateBlock(el)` - closes the previously open block, opens this one, runs
  `initBlockEditors(block)` and `updateContextBar()`. Focusing a field in a block (Tab, a click)
  opens it too (a `focusin` listener on `#entryBlocks`).
- `deactivateBlock(el)` - closes it and calls `refreshBlockPreview(block)`. Escape or a click
  outside the open block closes it, except on the top bar, the context bar, the media picker or
  the lightbox.
- `refreshBlockPreview(blockEl)` - builds the preview of a closed block from the current
  language's text field and `extra_data`. An open block (`--editing`) hides its preview, so it
  returns at once: typing (`onBlockInput()`) asks the server for nothing, and
  `deactivateBlock()` draws the preview once, when the block closes. Two strategies:
  - **Server-rendered** (`fetchServerPreview()`, see [Block preview](#block-preview)):
    `image`, `gallery`, `table`, `list`, `youtube-embed`, `code-text`, `terminal` - the exact
    epoch 3 markup a reader gets (highlighting, the terminal window, a list's bold labels, gallery "+N" tiles, themed
    table rows, the real iframe, image size/float classes). A client-side approximation fills
    the gap until the response arrives, or if it fails.
  - **Client-side**: `byline`, `separator` (`<hr>`), `link`, `list` (`<ul>`/`<ol>` per line),
    `social-networks`, `generic`.
  - Each type renders a `<span class="boat-rudder-empty">` placeholder
    when its field is blank. Values are HTML-escaped for client-side previews; `generic`
    intentionally injects raw HTML, since that is what it stores.

### Context bar

One place for the tools of whichever block is open, instead of a header per block.

- **Tools**: a block template marks its tool row with `.boat-rudder-entry-editor-block__tools`
  (title: H1-H6; code-text: language; terminal: shell; list: list type; table: header-row
  toggle; image: Select photo, size, alignment; gallery: + Add photos; paragraph: the rich-text
  toolbar `initParagraphEditors()` builds). The row **stays inside its block**, so the existing
  handlers (`closest('.boat-rudder-entry-editor-block')`, `closest('...__lang-content')`) keep
  working; while the block is open CSS pins it `position: fixed` over `#editorContextTools`,
  whose box `placeContextTools()` measures into `--br-editor-tools-left` / `-width` (again on
  resize and whenever the bar's layout changes). Under 600px the row goes to the bottom of the
  screen instead. The drag clone of an open block drops `--editing`, so it doesn't put a second
  row in the bar.
- **Actions** (`#editorContextActions`, hidden while no block is open): `contextAction(action)` -
  `split` / `merge` (paragraphs only; Merge disabled unless the block above is a paragraph),
  `up`, `down`, `advanced` (opens the block's Advanced panel - its own toggle is hidden), `delete`.
  Each calls the existing function (`splitParagraph()`, `mergeParagraphUp()`, `moveBlockUp/Down()`,
  `toggleAdvanced()`, `removeComponent()`) with the open block, which `closest()` resolves to
  itself.
- **+ Insert** and the sheet's **+ Add block** share one menu (`#addBlockMenu`):
  `toggleAddBlockDropdown(opener)` moves it next to the button that opened it (always downward
  in the bar). `insertNewComponent(type)` inserts after the open block - or, when that block is
  an empty paragraph, turns it into the new block (`convertBlock()`).
- `updateContextBar()` - fills the type label or switch, shows the actions, re-places the tools;
  called on open/close, reorder, delete and by a `MutationObserver` on `#entryBlocks`.

### Settings panel

`toggleEditorSettings(open)` (the top bar's ⚙ Settings) shows the panel as a column right of
the sheet; under 900px it is a drawer over the sheet with a backdrop (a click on it, or Escape,
closes it). The choice is remembered in `localStorage` (`br-editor-settings-open`); on a first
visit it opens on screens 1200px and wider.

### Block preview

`POST /dashboard/api/block-preview` (any signed-in user, epoch 3) takes `type`, `text`, `extra`
and answers `text/html` with `entry_page_render_block(type, text, extra, EPOCH_MODERN)` - the
public renderer itself, so the editor shows exactly what `/blog/<link>` will. Only the
server-rendered types above (and `paragraph`, which the editor no longer previews) are accepted;
anything else is a `500`. On the client:

- A per-block key (`type` + `text` + `extra`) skips the request when nothing changed - the editor
  refreshes previews often (language switch, closing a block), and re-rendering would reload a
  YouTube iframe or every gallery image.
- A per-block token discards a slower, older response that lands after a newer one.
- After a gallery preview arrives, `buildGalleries()` (from the epoch 3 lightbox script) is
  re-run so the new grid gets its "+N" tile and lightbox wiring.

### Rich text (paragraph blocks)
`initParagraphEditors(block)` - for each `.boat-rudder-entry-editor-block__lang-content` panel: creates a
`<div class="boat-rudder-entry-editor__richtext" contenteditable>` div above the hidden textarea, syncs content via
`syncRichtext()`. The rich-text toolbar (formatting + alignment/lists/source, one
`.boat-rudder-entry-editor-block__tools` row shown in the context bar) is injected via
`insertAdjacentHTML`. Paste handler strips external styles/classes. `toggleSource(btn)` toggles
between the contenteditable view and a raw `<textarea class="boat-rudder-entry-editor__source-editor">`.
An emptied editor is reset to `""` (the browser leaves a lone `<br>`), so its placeholder -
*Write something, or type / to insert a block*, shown only where the caret is - comes back.

### Splitting and merging paragraphs
`splitParagraph(btn)` (the context bar's **Split**) moves everything after the caret into a new
paragraph block below (one per language); `mergeParagraphUp(btn)` (**Merge**, or Backspace at a
paragraph's start) appends the paragraph onto the one above, removes it and resolves with the
block above (`null` if nothing was merged).

### Keyboard

The sheet behaves like one page (listeners on `#entryBlocks`, plus one on `document` for a
selected block):

| Where | Key | Effect |
|---|---|---|
| Paragraph | Enter / Shift+Enter | A line break inside the paragraph (`insertLineBreak`, a `<br>` as the stored text has them). In a list item, quote or `<pre>` of the rich text, the browser's own Enter. |
| Paragraph start | Backspace | Merges into the paragraph above, caret at the join (`placeCaretAtText()`); an empty paragraph under another block type is removed and the caret goes up. |
| Title | Enter | A paragraph below (`paragraphAfter()`); Backspace in an empty title removes it. |
| Code, terminal, list | Ctrl/Cmd+Enter, or Enter on an empty last line (dropped) | A paragraph below. HTML (`generic`): Ctrl/Cmd+Enter only. |
| Paragraph / title, first or last line | ↑ / ↓ | The previous / next block (`caretOnEdgeLine()`, `focusBlock()`); a block with no text field is selected instead. |
| A selected block, no field focused | Enter, ↑ / ↓ | A paragraph below; the neighbours. |

`paragraphAfter(block)` moves into the empty paragraph already below, or creates one - one at a
time (`creatingParagraph`), since a block is made by the server. A click on the sheet below the
last block writes there: at the end of that paragraph, or in a new one. Deleting a selected
object with a key is not offered (there's no undo); the context bar's delete does it.

### Block types (switching, `/` menu, Markdown shortcuts)

- **Switching**: for a paragraph, title or list the context bar shows a `<select>`
  (`#editorContextSwitch`: Paragraph, Heading 1-6, Bulleted / Numbered list);
  `switchActiveBlockType()` → `convertBlock(block, type, variant)`. Between paragraph, title and
  list the text is carried in every language (`convertText()`: a paragraph's `<br>` / block
  elements become lines - `htmlLines()` - a list's lines, a title one line); the new block is
  created right after, the old one hidden and deleted. Same type: only the variant changes (a
  heading's level, a list's kind - `applyVariant()`), no new block. A conversion can't be undone
  with Ctrl+Z (it is a server create + delete); switching back can.
- **`/` menu**: `/` alone in an empty paragraph opens `openSlashMenu()`, a list of every block
  (`SLASH_ITEMS`: label, type, variant, keywords); letters after it filter by word prefix
  (`/ima` → Image, Gallery; `/shell` → Terminal), ↑ ↓ and Enter/Tab pick, Escape, a space or
  deleting the `/` close it. Its keys are taken in the capture phase, before the paragraph's.
  `pickSlashItem()` turns the paragraph into the picked block.
- **Markdown shortcuts** (`MARKDOWN_SHORTCUTS`), typed alone in an empty paragraph: `# ` / `## ` /
  `### ` → heading 1-3, `- ` or `* ` → bulleted list, `1. ` → numbered list, ```` ``` ```` → code,
  `---` → separator.
- `enterBlock(block)` - where the writer goes on after a switch, a pick or a shortcut: the text of
  a paragraph or title, the first field of the rest; `image` and `gallery` open the media
  library, `separator` starts a paragraph below.

### Type-specific controls
- `image`: a picture per language, English's the default. *Select photo* (`setBlockImage()`, called
  by the media picker) sets the picture of the language being edited: on English it replaces
  every other language's that had none of its own - or a copy of English's previous one, how
  blocks were stored before - and on another language it gives that one its own (picking
  English's picture there, or *Use the English image*, empties it again). The first picture of a
  block is English's, whatever tab it was picked on. On another language, a note under the
  picture says which one it shows. A language left empty shows English's picture, in the editor
  and on the site.
- `code-text`, `terminal`: the language / shell `<select>` (in the context bar) → `syncCodeLang()`
  writes the key into `extra_data`; `initCodeBlock()` sets the `<select>` from it when the block
  opens. Their textarea is written in the code block's colors.
- `table`: a cell grid editor (`initTableBlock()`, `buildTableGrid()`) with
  `addTableRow/Col()`, `delTableRow/Col()` and a header-row toggle (`setTableHeader()`, in the
  context bar), serialized back to the `|`-separated text by `syncTableEditor()`.
- `list`: ordered/unordered `<select>` (`setListType()`, in the context bar).
- `link`, `youtube-embed`, `social-networks`: URL/icon inputs synced into `extra_data`
  (`syncLinkUrl()`, `syncYouTubeUrl()`, `syncSocialIcon()/syncSocialUrl()`).

### Heading levels (title blocks)
`initBlockHeadingBtns(block)` - reads `extra_data`, marks the matching H1-H6 button active and
sets the block's `data-level`. `setHeadingLevel(btn, level)` - updates `extra_data` and
`data-level`, resizes the heading.

### Gallery blocks
The open block shows its photos as numbered tiles (`.boat-rudder-entry-editor-gallery`), in
gallery order, each with ← / → (move one place; disabled at the ends) and × (remove from the
gallery), plus a last **+ Add photos** tile. A tile can also be dragged to a new place. The
arrows are what work on touch screens and from the keyboard, where HTML5 drag-and-drop doesn't.

- `initGalleryBlock(block)` - reads the current language's text field (semicolon URLs) and calls `renderGalleryThumbs()`.
- `renderGalleryThumbs(container, value)` - renders the tiles (`_small` variant thumbnails) and the add tile. Its click
  and drag listeners are attached to the container once (`data-gallery-ready`), so re-rendering never stacks them.
- `initGalleryTiles(container)` - one delegated `click` handler for `data-gallery-action` = `earlier`/`later`/`remove`/`add`,
  and the drag handlers (`dragstart`/`dragover` move the tile live, `dragend` saves the order).
- `syncGalleryInput(container)` - writes the tiles' order back to every language's text field, re-renders the tiles
  (numbers and arrows), refreshes the block preview and marks the entry changed.
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
- `editorSaveAll(retried)` - runs all three in parallel via `Promise.all()`. A save never hangs:
  `postForm()` rejects when there is no JSON to read (see [Session](#session)), and the status
  then says why (`Save failed: <error>`, `Not saved: ...`, `Signed out - not saved`), with the
  page left marked as changed.

### Session

A session ends on its own after `session_ttl_seconds` (24 h by default, not renewed while
writing - see [configuration.md](configuration.md)) or in another tab. The editor:

- **Knows**: `/dashboard/api/*` answers `401` JSON (`"session":"expired"`) without a session
  instead of redirecting to `/login` (which `fetch()` would follow, handing `res.json()` the login
  page), and `403` JSON (`"session":"csrf"`) for a token of an older session. `postForm()` turns
  both into rejections with `err.session` set.
- **Warns early**: `checkSession()` polls `GET /dashboard/api/session` every 5 minutes
  (`SESSION_CHECK_MS`) and whenever the tab becomes visible again, so the writer hears of it
  before saving, not after.
- **Keeps the work**: `sessionLost()` shows `#editorSession` under the context bar - the changes
  are still on the page, *Sign in in a new tab* (`/login`), then *Save again*
  (`retryAfterSignIn()`: checks the session, takes its new CSRF token through
  `window.boatRudderSetCsrfToken()`, hides the notice and runs `editorSaveAll()`). A save that
  fails only on the token (signed in again elsewhere) fetches it and retries once on its own.
- **Asks before leaving**: a `beforeunload` handler stops a reload, a close or the *Dashboard*
  link while there are unsaved changes.

### Other
- `togglePublish()` - toggles the hidden `enabled` checkbox + publish-switch UI. Unpublished =
  draft: still reachable at its public URL for any signed-in user, marked `[Draft]`.
- `openBlogPreview()` - the top bar's *View post* button: opens `/blog/<link>` or
  `/page/<link>` in a new tab - works for drafts too, since the tab carries the session cookie.
- `toggleAutoSave()` - enables/disables the 3-second autosave timer.
- `toggleTranslatePanel()` / `runTranslation()` - the *Translate* button, see [Translation](#translation).
- `insertNewComponent(type)` / `removeComponent(btn)` - POST to `.../blocks` / `.../blocks/<id>/delete`; patch `#entryBlocks` directly. `createBlockAfter(anchor, type, before)` is the shared
  create: after `anchor` (the end when none), or before `before` when given.
- `moveBlockUp/Down(btn)` - client-side DOM reorder; persisted on next `saveContent()`.
- Drag-and-drop block reorder - mousedown on `.boat-rudder-entry-editor-block__grip`, ghost clone, placeholder indicator; reindexes order on mouseup.

### Translation

The top bar's **Translate** button (shown when *Settings → Translation* has a provider and the
site has more than one language) fills the other languages from the one being edited. Plan:
[entry-translation-plan.md](../plans/entry-translation-plan.md).

- The panel lists every other language, all checked; *Translate* runs `runTranslation()`.
- **Replace what is already written** (`#translateOverwrite`) redoes every field that has source
  text, written or not, after a `confirm()` naming the languages. It is unchecked every time the
  panel opens, so it can't be left on by mistake; the hint under it says which mode is on.
- Otherwise **only empty fields are filled**, and only where the source has text: header title and summary,
  and the `title`, `paragraph` (as HTML, tags kept), `list` (line by line), `table` (cell by cell,
  so `|` and line breaks survive) and `link` blocks. `code-text` and `terminal` are **copied** as
  they are, never sent to the translator: code and commands are the same in every language, and a
  translator would rename keywords, paths and options - but left empty, the block would show
  nothing in that language. `image`, `gallery`, `byline`, `youtube-embed`, `separator`,
  `social-networks` and `generic` are left alone. An opened table's starter grid (`Column 1`...)
  still counts as empty.
- The texts go in batches - up to 50 texts and 40,000 characters, one format each - so an entry is
  one or two calls per language. `translateBatch()` is the only part that depends on the provider:
  - **Azure**: `POST /dashboard/api/translate` (§5); the server calls Azure AI Translator
    (`/translate?api-version=3.0&from=…&to=…&textType=plain|html`) with the stored key and region
    (`src/utils/translator.c`); the key never reaches the page. Azure takes the site's ISO 639-1
    codes as they are (only `zh` becomes `zh-Hans`).
  - **Local**: the browser POSTs straight to `data-translate-url` + `/translate`, LibreTranslate's
    API - `{"q": [...], "source", "target", "format", "api_key"}` → `{"translatedText": [...]}` -
    since the server (on a VPS) can't reach the editor's computer. Browsers treat
    `http://localhost` as secure, so this works from a site served over HTTPS; the translator
    must answer CORS (and Chrome may ask once to allow access to the local network).
- Progress shows in the panel and the save status. The panel stays open until the run ends, so
  *Nothing to translate* or an error (Azure's reason - a refused key names the region sent, or
  that none is saved) is shown where the editor is looking. With autosave on, `editorSaveAll()` runs as soon as it
  ends; otherwise the fields wait for *Save all* (*Translated 7 fields - not saved yet*). An
  error stops the run and is shown there; what was already filled stays, marked as changed.

---

## 5. Routes

Every route below is `EPOCH_MODERN` only (`302 /dashboard` otherwise) and passes through
`require_dashboard_session_role()` (except `block-preview`, see the table). The five `/api/` endpoints share one ownership gate:
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
| `/dashboard/api/session` | `GET` | `require_dashboard_session()` only. `{"ok":true,"csrf":"<token>"}`, or `401 {"ok":false,"session":"expired"}` - see [Session](#session) |
| `/dashboard/api/translate` | `POST` | `require_dashboard_session()` only (a viewer is refused, as for any write). Azure provider only. `source`, `target`, `format` (`text`\|`html`), `text` × up to 50 → `{"ok":true,"texts":[...]}` \| `400`/`502 {"ok":false,"error":"..."}` |
| `/dashboard/api/block-preview` | `POST` | `require_dashboard_session()` only (no entry id, no ownership). `type`, `text`, `extra` → `text/html` from `entry_page_render_block()`; `403` plain text on non-epoch-3 |

---

## 6. Gallery block and media integration

When the content is saved (`.../content` POST), the router detects gallery blocks and calls
`cms_upsert_media_gallery(existing_gallery_id_or_null, entry_id, urls_csv, out_id)`. This
creates or updates a `media_galleries` document (BSON array of URLs) and writes the resulting
`_id` back into `content[].extra_data`. On the next save the existing `_id` is updated in-place.

The media picker modal (`/dashboard/api/media/modal`) returns the full
`dashboard/media/media_epoch3.html` wrapped in `.boat-rudder-modal`. Its inline JS sets selected
URLs into the target block's text fields (gallery) or the header `image_url` input (header cover).
For a gallery the picked photos are **appended** after the ones it already has, in the order they
were picked, skipping any it already has - removing and reordering happen in the block itself.
`activateScripts(container)` re-executes all `<script>` tags after `innerHTML` injection so that
`selectDirectory`, `upload`, and other media functions are available.

---

## 7. Future work

- Mobile: keep the context bar's bottom tool row above the virtual keyboard (`visualViewport`),
  and fold the top bar's secondary controls (Draft, Manual, View post) into a ⋯ menu.
- An undo for deleting and converting blocks (both are server writes today).
- The `image-single` block type (the last block type from the legacy editor not yet ported).
- A `/gallery/<slug>` human-readable URL (currently `_id` hex only).
- Deleting `media_galleries` documents when their gallery block or entry is removed.
