# Media Admin (`/dashboard/media`)

A file manager and image library for the CMS. Admins and authors can upload images, organize
them into directories, and select them from the entry editor. All routes require a valid session
cookie (`require_dashboard_session()`).

---

## 1. Data model (`src/db/cms_media.h/.c`)

### Collections

**`media`** — one document per uploaded file:

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

**`media_directories`** — one document per folder:

```jsonc
{
  "_id": ObjectId,
  "name": "Docs",               // folder name (3-60 chars, [a-zA-Z0-9_-])
  "parent": "posts",
  "author_id": ObjectId
}
```

**`media_galleries`** — created/updated by the content save route for each `gallery` block:

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
| `cms_create_media_directory(name, parent, author_id, out_id)` | Insert dir |
| `cms_rename_media_directory(id_hex, new_name)` | `$set` name |
| `cms_delete_media_directory(id_hex)` | Delete dir record |
| `cms_get_media_items(dir_id, skip, limit, &items, &count)` | Aggregation: `$lookup` author + dir, `$match` by `dir_id`, sort by `_id` desc, skip/limit |
| `cms_insert_media(filename, author_id, dir_id, out_id)` | Insert media record |
| `cms_upsert_media_gallery(gallery_id, entry_id, urls_csv, out_id)` | Create or update gallery document |
| `cms_get_media_gallery(id_hex, &gallery)` | Read one gallery document |

`cms_get_media_items()` uses a MongoDB aggregation pipeline: `$lookup` users (`author_id` →
`username` derived from `email` prefix), `$lookup` media_directories (`dir_id` → `name`),
`$match` by `dir_id` (if specified), `$sort { _id: -1 }`, `$skip`/`$limit`.

---

## 2. File storage

Uploaded files are stored at:
```
./html/content/posts/<username>/<dirname>/<filename>
```
where `<username>` is derived from the uploader's email (part before `@`, sanitized).
Files are served by the existing static file server at `/content/posts/...`.

Physical directory operations (`mkdir`, `rename`, `rmdir`) are performed by `http_router.c`
when creating, renaming, or deleting a directory. Directories must be empty to delete.

---

## 3. Image optimizer (`scripts/image-optimizer.sh`)

Runs after each upload via `popen()`. Requires `imagemagick` (magick), `jpegoptim`, `gifsicle`.

For each uploaded image, generates 5 variants and deletes the original:

| Variant | Max dimension | Format |
|---|---|---|
| `_full` | original | JPEG (photos) or GIF (≤256 colors) |
| `_half` | 1024 px | JPEG or GIF |
| `_small` | 300 px | JPEG or GIF — used for thumbnails |
| `_medium` | 600 px | always GIF — epoch 1/2 compatibility |
| `_micro` | 180 px | always GIF — epoch 1 / Mosaic compatibility |

Additionally creates a symlink `<original_name> → <basename>_half.<ext>` so the stored URL
(without suffix) resolves via the static file server.

**Color detection**: images with ≤256 unique colors generate all variants as GIF (with
Riemersma dithering + gifsicle optimization). Photos generate `_full/_half/_small` as JPEG
(jpegoptim, quality 80) and `_medium/_micro` as GIF for old-browser compatibility.

---

## 4. Page rendering (`src/modules/media_admin/media_admin.c`)

- `media_admin_page(epoch, dirs, dir_count, items, item_count)` — renders `dashboard/media/media_epoch3.html` with directory sidebar HTML + initial photo grid HTML.
- `media_admin_render_directories(dirs, count, epoch)` — renders each directory item via `media-directory_epoch3.html`, wrapped in `media-directory-container_epoch3.html`.
- `media_admin_render_items(items, count, epoch)` — renders each photo card via `item-photo_epoch3.html`, constructing the thumbnail URL as `posts/<author>/<dir>/<basename>_small<ext>` and the full URL as `posts/<author>/<dir>/<basename><ext>`.
- `media_admin_render_directory_item(dir, epoch)` — renders a single directory item (returned by the AJAX create/rename endpoints).
- `media_admin_modal(epoch, dirs, dir_count, items, item_count)` — wraps `media_admin_page()` output inside `media-modal_epoch3.html` (`.boat-rudder__modal` overlay) for the entry editor picker.

---

## 5. Templates (`html/themes/dark/dashboard/media/`)

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
- `newDirectoryBtnHandler()` — appends a text input to the directory list; `createNewDirectory()` POSTs on blur.
- `selectDirectory(id)` — sets `#media-directory-selected`, clears the grid, loads page 1.
- `renameDirectoryBtnHandler()` / `renameDirectory()` — toggle label/input; POST on blur.
- `deleteDirectoryBtnHandler()` / `deleteDirectory(id)` — confirm + POST.
- `validarCaracter(e)` — keypress validator for directory name input (`[a-zA-Z0-9_-]` only).

**Content loading**:
- `getMediaContents(directoryId, start, end)` — GET `/dashboard/api/media/contents?...`, appends returned HTML to `#media-content-dynamically`.
- Infinite scroll: `scroll` listener on `#media_content__charger`; loads next 30 items when near bottom.

**Upload**:
- `upload()` — iterates selected files (JPEG, PNG, GIF only), XHR POST to `/dashboard/api/media/upload` with progress bar per file; reloads directory on completion.
- Drag-and-drop zone: `dragover`/`dragleave`/`drop` on `#drop-zone`; file-input `change` listener.

**Selection** (for the entry editor picker):
- Click toggles `.selected` + checkbox; Shift+click for range selection.
- `getSelectedMediaIds()` — collects selected URLs in selection order; writes to:
  - `header` type: `#headerImageUrl` input + header image preview
  - `image-block` type: all lang text fields of `window._galleryTargetBlock`
  - `gallery-block` type: all lang text fields + calls `renderGalleryThumbs()`
  - Closes the modal.
- ESC key clears selection.

---

## 7. Routes

All routes require `require_dashboard_session()`. The modal and contents routes return raw HTML
(not wrapped by `buildPageWebSite()`).

| Route | Method | Behavior |
|---|---|---|
| `GET /dashboard/media` | GET | Full media admin page via `buildPageWebSite()` |
| `GET /dashboard/api/media/contents` | GET | Paginated photo grid HTML (`?directory=<id>&start=N&end=M`) |
| `GET /dashboard/api/media/modal` | GET | Media picker modal HTML (raw, no page shell) |
| `POST /dashboard/api/media/directory` | POST | Create dir on disk + DB. Returns rendered `<li>` HTML. |
| `POST /dashboard/api/media/directory/rename` | POST | `rename()` + DB update. Returns rendered `<li>` HTML. |
| `POST /dashboard/api/media/directory/delete` | POST | `rmdir()` + DB delete. `200 OK` on success. |
| `POST /dashboard/api/media/upload` | POST | Multipart upload, optimizer, DB insert. Returns `{"ok":true,"filename":"..."}`. |
| `GET /gallery/<id>` | GET | **Public** (no session). Epoch 3: thumbnail grid page. Epochs 1-2: paginated viewer with prev/next + thumbnails. Epochs -1/0: text link list. |

---

## 8. Gallery page (`/gallery/<id>`)

A public, epoch-aware standalone page (not wrapped by `buildPageWebSite()`):

- **Epoch 3**: uses `gallery-page_epoch3.html` + `gallery-page-item_epoch3.html`. Each item is a clickable thumbnail linking to the `_full` variant. The page CSS uses a grid with `auto-fill, minmax(200px, 1fr)`.
- **Epochs 1-2**: inline HTML generation in `http_router.c`. Shows the current image (`_half` for epoch 2, `_medium.gif` for epoch 1), prev/next links, and a thumbnail strip (all images at 80×60 px; active image has a 3 px border). Navigation via `?img=N`.
- **Epochs -1/0**: text list of image links wrapped in `gallery-page_epoch{-1,0}.html`.

All epochs use `cms_get_media_gallery(id_hex, &gallery)` to read the URL array from MongoDB.
