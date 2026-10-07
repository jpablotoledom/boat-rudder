# Boat Rudder - QR Codes and Short Links

Retro browsers can't show what a modern one can - no video player before HTML5, no images at all
in a text browser or on many WAP phones. Wherever that happens, Boat Rudder renders a **QR code**
the reader scans with the phone next to the old machine, plus a printed link. To keep those codes
small enough to fit and scan, most of them encode a **short link** (`/qr/<code>`) instead of the
real, long URL.

---

## Where QR codes appear

| Content | Epoch −1 (WML) | Epoch 0 (text) | Epochs 1-2 | Epoch 3 |
|---|---|---|---|---|
| `youtube-embed` block | WBMP image of `https://youtu.be/<id>` | `[View video QR]` link → `/youtube-qr/<id>` | GIF image of `https://youtu.be/<id>` | `<iframe>` |
| `image` block | `[View image QR]` link → `/image-qr/<code>` | same | the image | the image |
| `/gallery/<id>` page | WBMP of `<base>/qr/<code>` → the gallery | text QR of the same | paginated viewer | grid + lightbox |

`<base>` is `public_url` when set, otherwise `scheme://Host` of the current request
(`absolute_location()` in `http_router.c`).

The epoch 0 pages `/youtube-qr/<id>` and `/image-qr/<code>` exist so a big text QR is the only
thing on screen: inlined in an article, a paginated text browser (Lynx and similar) could split
it across two screens and make it unscannable. They are **always rendered as epoch 0**, whatever
the visitor's epoch, and carry a `?back=<path>` link labeled "Back to <entry title>" (looked up
from the path, so the title doesn't bloat the QR page's URL).

---

## Short links

`src/db/short_links.c`, collection `short_links` (`{_id: <code>, target_path}` -
[data-model.md](data-model.md#short_links)).

| Function | Behavior |
|---|---|
| `short_link_get_or_create(target_path)` | Returns the existing code for `target_path`, or mints a new 7-character base62 code (libsodium `randombytes_uniform`, ~3.5·10¹² combinations) and inserts it. Idempotent per target. Retries up to 5 times on a duplicate-key error. `NULL` if MongoDB is unavailable. |
| `short_link_resolve(code)` | `target_path` for `code`, or `NULL`. |

Route: `GET /qr/<code>` → `302` to `target_path`, `404` if unknown.

**Why.** QR size grows with the encoded text. A gallery URL with its domain is easily 60+
characters, which forced a module grid large enough to overflow a Cello window's row width and
cut off the finder patterns a reader needs to lock on. `/qr/<code>` keeps every QR as small as
the code, independent of the real path's length - and makes the printed fallback link short
enough to type by hand.

Targets in use:

| Caller | Target |
|---|---|
| `/gallery/<id>` for epochs −1/0 | `/gallery/<id>` |
| `render_image()` for epochs −1/0 | the image's `_full` variant URL |

YouTube QRs don't use short links: `https://youtu.be/<id>` is already short. If MongoDB is down,
the gallery falls back to encoding the full gallery URL; the image block simply omits its QR link.

---

## QR renderers

`src/utils/qr_generator/` encodes with **libqrencode** (`qrencode_minimal.h` declares the two
symbols used, since the library ships no pkg-config file) and writes every format in pure C - no
ImageMagick.

| Function | Output | Used for |
|---|---|---|
| `generate_youtube_qr(url, html_root)` | GIF (own LZW encoder) at `html/content/qr/youtube-<id>.gif` | Epochs 1-2 video blocks |
| `generate_youtube_qr_wbmp(url, html_root)` | WBMP at `html/content/qr/youtube-<id>.wbmp` | WML video blocks |
| `generate_qr_wbmp(text, fs_path)` | WBMP at any path (gallery: `html/content/qr/gallery-<id>.wbmp`) | WML gallery page |
| `generate_qr_halfblock_text(text)` | `<pre>` of Unicode half-blocks (`▀ ▄ █`), two QR rows per text row so it comes out square | Epoch 0 in a UTF-8 terminal |
| `generate_qr_asciiblock_text(text)` | Same layout using Latin-1 `Û Ü ß`, which `utf8_to_latin1()` turns into bytes `0xDB/0xDC/0xDF` - block glyphs in the CP437 "Terminal" font | Epoch 0 when `request_needs_legacy_charset()` (Cello) |

- The choice between the two text forms follows the **real** browser (`request_charset`), not
  the rendered epoch, so previewing epoch 0 from a modern browser shows the Unicode form.
- Text QRs have no quiet-zone margin (a wide code must still fit the window) and end with a
  bordered note pointing to font-setup instructions. **That note currently hardcodes a URL of a
  specific site** (`qr_generator.c`, `write_qr_footer()`), which contradicts the "nothing
  site-specific in the source tree" rule - see [style-guide.md](style-guide.md).
- Image files are cached in `html/content/qr/` (gitignored). Each writer writes a private temp
  file and `rename()`s it into place, and the LZW dictionary is per call, because connection
  threads render concurrently.
- `extract_youtube_id()` accepts only `[A-Za-z0-9_-]`, since the id becomes part of a file path.

WBMP output goes through `write_wbmp()` (`src/utils/wbmp_writer.c`), shared with the theme logo
converter ([themes.md](themes.md#7-theme-assets-and-wbmp-conversion)).

---

## Role of `public_url`

`public_url` ([configuration.md](configuration.md#site-and-rendering)) is the base for the
absolute URLs encoded into gallery and image QRs. Set it when the site is reached through a
different name than the one the phone should use - typical when the retro machine browses a
LAN address or through a proxy, but the phone scanning the code is on the public internet.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
