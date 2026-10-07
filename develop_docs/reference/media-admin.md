# Media Admin (`/dashboard/media`)

A file manager and image library for the CMS. Admins and authors can upload images, organize
them into directories, move and delete them, and select them from the entry editor. All routes
are epoch 3 only and require a valid session cookie (`require_dashboard_session()`) - any role;
there is no per-item ownership check (see [security.md](security.md#known-gaps)). Collection
schemas are summarized in [data-model.md](data-model.md#media).

---

## 1. Data model (`src/db/cms_media.h/.c`)

### Collections

**`media`** - one document per uploaded file:

```jsonc
{
  "_id": ObjectId,
  "name": "photo.jpg",          // sanitized filename (no spaces, alphanumeric/-/.)
  "date": "2026-06-29 12:00:00",
  "format": "jpg",              // extension extracted from name
  "author_id": ObjectId,        // ref to users._id
  "dir_id": ObjectId            // ref to media_directories._id
}
```

**`media_directories`** - one document per folder:

```jsonc
{
  "_id": ObjectId,
  "name": "Docs",               // folder name (3-60 chars, [a-zA-Z0-9_-])
  "parent": "posts",
  "author_id": ObjectId
}
```

**`media_galleries`** - created/updated by the content save route for each `gallery` block:

```jsonc
{
  "_id": ObjectId,
  "content": ["/content/posts/user/dir/photo.jpg", ...],  // BSON array of URLs
  "entry_id": ObjectId
}
```

### Key functions

| Function | Operation |
|---|---|
| `cms_get_media_directories(&dirs, &count)` | Find all dirs, sorted by `_id` asc |
| `cms_get_media_directory_by_id(id_hex, &dir)` | Find one dir |
| `cms_get_media_directory_by_name(name, author_id, &dir)` | Find one author's dir by exact name - used for the per-author `default` directory |
| `cms_create_media_directory(name, parent, author_id, out_id)` | Insert dir |
| `cms_rename_media_directory(id_hex, new_name)` | `$set` name |
| `cms_delete_media_directory(id_hex)` | Delete dir record |
| `cms_get_media_items(dir_id, skip, limit, &items, &count)` | Aggregation: `$lookup` author + dir, `$match` by `dir_id`, sort by `_id` desc, skip/limit |
| `cms_get_media_item_by_id(id_hex, &item)` | One item with author username and directory name/parent, to locate its files before delete/move |
| `cms_insert_media(filename, author_id, dir_id, out_id)` | Insert media record |
| `cms_delete_media(id_hex)` | Delete media record |
| `cms_move_media(id_hex, dest_dir_id)` | `$set` `dir_id` (the router moves the files) |
| `cms_upsert_media_gallery(gallery_id, entry_id, urls_csv, out_id)` | Create or update gallery document |
| `cms_get_media_gallery(id_hex, &gallery)` | Read one gallery document |

`cms_get_media_items()` and `cms_get_media_item_by_id()` use a MongoDB aggregation pipeline:
`$lookup` users (`author_id` → `username` derived from `email` prefix), `$lookup`
media_directories (`dir_id` → `name`), `$match` by `dir_id`/`_id` (if specified),
`$sort { _id: -1 }`, `$skip`/`$limit`. They run through `mongodb_manager_aggregate()`, not
`mongoc_collection_aggregate()`, which is broken in libmongoc 1.30.4-1+deb13u3 (see
[data-model.md](data-model.md#known-driver-issue)).

---

## 2. File storage

Uploaded files are stored at:
```
./html/content/posts/<username>/<dirname>/<filename>
```
where `<username>` is derived from the uploader's email (part before `@`, sanitized).
Files are served by the existing static file server at `/content/posts/...`.

Physical directory operations (`mkdir`, `rename`, `rmdir`) are performed by `http_router.c`
when creating, renaming, or deleting a directory. The on-disk path is built from the **signed-in
user's** username and the directory name sent by the client.

- **Default directory.** An upload with no directory selected (or an unknown one) - e.g. from
  the entry editor's picker before any directory exists - goes into the uploader's `default`
  directory, created on demand on disk and in `media_directories`
  (`resolve_or_create_default_media_directory()`). The upload response returns its `dir_id` so
  the picker can reveal it.
- **Deleting a directory** calls `rmdir()`, which only succeeds on an empty folder, but the
  `media_directories` record is deleted **regardless**. Deleting a non-empty directory from the
  UI (which warns "The directory and its contents will be deleted") therefore leaves its files on
  disk and its `media` records pointing at a missing directory. Empty it first (select all →
  *Delete selected* or *Move to…*).
- **Deleting items** removes the base file and every size variant (`_full/_half/_small` with the
  original extension, `_medium/_micro` as `.gif`) with `unlink()`, then the record.
- **Moving items** `rename()`s every variant into `posts/<author>/<destination>/` (creating it if
  needed), then updates `dir_id`. Variants that don't exist are skipped silently.

---

## 3. Image optimizer (`scripts/image-optimizer.sh`)

Runs after each upload via `popen()`. Requires `imagemagick` (magick), `jpegoptim`, `gifsicle`.

For each uploaded image, generates 5 variants and deletes the original:

| Variant | Max dimension | Format |
|---|---|---|
| `_full` | original | JPEG (photos) or GIF (≤256 colors) |
| `_half` | 1024 px | JPEG or GIF |
| `_small` | 300 px | JPEG or GIF - used for thumbnails |
| `_medium` | 600 px | always GIF - epoch 1/2 compatibility |
| `_micro` | 180 px | always GIF - epoch 1 / Mosaic compatibility |

Additionally creates a symlink `<original_name> → <basename>_half.<ext>` so the stored URL
(without suffix) resolves via the static file server.

**Color detection**: images with ≤256 unique colors generate all variants as GIF (with
Riemersma dithering + gifsicle optimization). Photos generate `_full/_half/_small` as JPEG
(jpegoptim, quality 80) and `_medium/_micro` as GIF for old-browser compatibility.

---

## 4. Page rendering (`src/modules/media_admin/media_admin.c`)

- `media_admin_page(epoch, dirs, dir_count, items, item_count)` - renders `dashboard/media/media_epoch3.html` with directory sidebar HTML + initial photo grid HTML.
- `media_admin_render_directories(dirs, count, epoch)` - renders each directory item via `media-directory_epoch3.html`, wrapped in `media-directory-container_epoch3.html`.
- `media_admin_render_items(items, count, epoch)` - renders each photo card via `item-photo_epoch3.html`, constructing the thumbnail URL as `posts/<author>/<dir>/<basename>_small<ext>` and the full URL as `posts/<author>/<dir>/<basename><ext>`.
- `media_admin_render_directory_item(dir, epoch)` - renders a single directory item (returned by the AJAX create/rename endpoints).
- `media_admin_modal(epoch, dirs, dir_count, items, item_count)` - wraps `media_admin_page()` output inside `media-modal_epoch3.html` (`.boat-rudder-modal` overlay) for the entry editor picker.

---

## 5. Templates (`html/templates/dashboard/media/`)

Shared (theme-agnostic) since the split between `html/templates/` and `html/themes/` - see
[templates-catalog.md](templates-catalog.md).

| Template | `%s` count | Description |
|---|---|---|
| `media_epoch3.html` | 2 | Full page: `%s` = dirs HTML, `%s` = items HTML. Contains all inline JS. |
| `media-directory-container_epoch3.html` | 1 | `<ul>` wrapper for directory list items |
| `media-directory_epoch3.html` | 12 | `<li>` with hidden inputs (id × 3, name × 4, parent, author name, author id). Supports inline rename (hidden input shown on rename action). |
| `item-photo_epoch3.html` | 8 | Photo card: `%s` = id (×3), thumb path, name, id (×2), content path |
| `media-modal_epoch3.html` | 1 | Modal wrapper: `%s` = media page HTML |

---

## 6. Client-side JS (inline in `media_epoch3.html`)

All functions are declared on `window` so they remain available after `activateScripts()` re-executes the template's script when loaded in the entry editor modal.

**Directory management**:
- `newDirectoryBtnHandler()` - appends a text input to the directory list; `createNewDirectory()` POSTs on blur.
- `selectDirectory(id)` - sets `#media-directory-selected`, clears the grid, loads page 1.
- `renameDirectoryBtnHandler()` / `renameDirectory()` - toggle label/input; POSTs `id` + `newname` on blur.
- `deleteDirectoryBtnHandler()` / `deleteDirectory(id)` - confirm + POST `id` (only empty directories are deleted).
- Errors from these and the bulk actions are shown with `alert()` using the server's plain-text
  reason (`responseTextOrThrow()`); after a move/delete the grid is reloaded
  (`refreshSelectedDirectory()`), since only the user's own images may have been affected.
- Every POST carries the CSRF token through `/assets/js/csrf.js` (loaded by the page layout), not
  through code in this template.
- `validarCaracter(e)` - keypress validator for directory name input (`[a-zA-Z0-9_-]` only).

**Content loading**:
- `getMediaContents(directoryId, start, end)` - GET `/dashboard/api/media/contents?...`, appends returned HTML to `#media-content-dynamically`.
- Infinite scroll: `scroll` listener on `#media_content__charger`; loads the next page when near
  the bottom. Page size is `MEDIA_PAGE = 36` in the template, kept in step with the server's
  `MEDIA_PAGE_SIZE` (`cms_media.h`). With no directory selected (the picker's initial state) the
  grid pages through the newest uploads across every directory.

**Upload**:
- `upload()` - iterates selected files (JPEG, PNG, GIF, WebP only; the server re-checks), XHR POST to `/dashboard/api/media/upload` with progress bar per file; reloads directory on completion.
- Drag-and-drop zone: `dragover`/`dragleave`/`drop` on `#drop-zone`; file-input `change` listener.

**Bulk actions** (on the current selection):
- `deleteSelectedMedia()` - confirm, POST `ids` (comma-separated) to `/dashboard/api/media/delete`.
- `openMoveMediaPicker()` / `applyMoveMedia()` / `closeMoveMediaPicker()` - the *Move to…*
  directory list; POSTs `ids` + `dest_dir` to `/dashboard/api/media/move`.

**Selection** (for the entry editor picker):
- Click toggles `boat-rudder-media__photo--selected` + checkbox; Shift+click for range selection.
- `getSelectedMediaIds()` - collects selected URLs in selection order; writes to:
  - `header` type: `#headerImageUrl` input + header image preview
  - `image-block` type: all lang text fields of `window._galleryTargetBlock`
  - `gallery-block` type: all lang text fields + calls `renderGalleryThumbs()`
  - Closes the modal.
- ESC key clears selection.

---

## 7. Routes

All routes require `require_dashboard_session()` (which, for `POST`, also checks the CSRF
token). The modal and contents routes return raw HTML (not wrapped by `buildPageWebSite()`).

**Ownership and paths.** Writes go through `media_can_manage()`: an `admin` manages everything,
an `author` only directories and images whose `author_id` is theirs. On-disk paths are built
from the DB record - `./html/content/posts/<owner username>/<directory name>/` - never from a
name or path sent by the client, and every component is re-validated
(`media_dir_name_valid()`, `media_username_valid()`, `media_file_name_valid()`) before it touches
the filesystem. See [security.md](security.md#media-ownership).

| Route | Method | Behavior |
|---|---|---|
| `GET /dashboard/media` | GET | Full media admin page via `buildPageWebSite()` |
| `GET /dashboard/api/media/contents` | GET | Paginated photo grid HTML (`?directory=<id>&start=N&end=M`) |
| `GET /dashboard/api/media/directory/item` | GET | One directory `<li>` (`?id=`), JSON-wrapped |
| `GET /dashboard/api/media/modal` | GET | Media picker modal HTML (raw, no page shell) |
| `POST /dashboard/api/media/directory` | POST | `newpath` (3-60 `[A-Za-z0-9_-]`) → dir on disk + DB, owned by the caller. `409` if the caller already has one with that name. Returns rendered `<li>` HTML. |
| `POST /dashboard/api/media/directory/rename` | POST | `id`, `newname` → `rename()` + DB update (an absent folder on disk is fine). `403` not the owner, `400` invalid name, `409` name taken. Returns rendered `<li>` HTML. |
| `POST /dashboard/api/media/directory/delete` | POST | `id` → `rmdir()`, then DB delete. `403` not the owner, `409` while `media` records point at it or files remain on disk. `200 OK` on success. |
| `POST /dashboard/api/media/delete` | POST | `ids` → unlink every variant (plus the optimizer's original-name symlink) + delete records, for the items the caller may manage. `403` if none, `500` if every allowed id failed, else `200` |
| `POST /dashboard/api/media/move` | POST | `ids`, `dest_dir` → rename every variant + `$set dir_id`. `400` unknown destination, `403` destination or items not the caller's, or items of a different author than the destination |
| `POST /dashboard/api/media/upload` | POST | Multipart `file` + `media-directory-selected`; no/unknown directory → `default`, another author's → `403`. `.jpg/.jpeg/.png/.gif/.webp` with matching magic bytes, else `415`. Optimizer, DB insert; if the optimizer reports no output the upload is deleted (`415`). Returns `{"ok":true,"filename":"...","dir_id":"..."}`. |
| `GET /gallery/<id>` | GET | **Public** (no session). Epoch 3: thumbnail grid page. Epochs 1-2: paginated viewer with prev/next + thumbnails. Epochs -1/0: QR code to the gallery. |

---

## 8. Gallery page (`/gallery/<id>`)

A public, epoch-aware standalone page (not wrapped by `buildPageWebSite()`):

- **Epoch 3**: uses `gallery-page_epoch3.html` + `gallery-page-item_epoch3.html`. Each item is a clickable thumbnail linking to the `_full` variant. The page CSS uses a grid with `auto-fill, minmax(200px, 1fr)`.
- **Epochs 1-2**: assembled in `http_router.c` from `gallery-page-main`, `gallery-page-thumbstrip` and `gallery-page-thumb` templates. Shows the current image (`_half` for epoch 2, `_medium.gif` for epoch 1), prev/next links (wrapping around), and a strip of inline thumbnails (`_small`, `_micro.gif` on epoch 1) that wraps to the screen width; the active one has a 3 px border. Navigation via `?img=N`.
- **Epochs -1/0**: neither can show the photos, so the page (`gallery-page_epoch{-1,0}.html` +
  `gallery-page-qr_epoch{-1,0}.html`) shows a **QR code** the reader scans with a phone, plus the
  link as text. The QR encodes `/qr/<code>`, a short link to `/gallery/<id>` (made absolute with
  `public_url`, or the request's `Host`), because the full gallery URL made the QR too dense to
  fit a text browser's row. WML gets a WBMP image (`html/content/qr/gallery-<id>.wbmp`); epoch 0
  gets Unicode half-block text, or Latin-1 block glyphs for Cello. See
  [qr-and-short-links.md](qr-and-short-links.md).

Every epoch's page has a "Back to <entry title>" link to the entry the gallery belongs to
(`media_galleries.entry_id`, `cms_get_entry_backlink()`), falling back to `history.back()`.

All epochs use `cms_get_media_gallery(id_hex, &gallery)` to read the URL array from MongoDB.
