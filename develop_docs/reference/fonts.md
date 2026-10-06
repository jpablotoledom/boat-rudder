# Boat Rudder - Font Library

A small, site-wide library of uploaded font files that themes can use for the **epoch 3 navbar
logo text** (and the epoch 3 footer title, which shares the same CSS variable). Fonts are global;
each theme only stores the *name* of the font it picked. How the choice is applied is described
in [themes.md](themes.md#epoch-3-logo-font).

---

## Storage

| What | Where |
|---|---|
| Metadata | `fonts` collection: `{_id, name, filename}` ([data-model.md](data-model.md#fonts)) |
| Files | `html/assets/fonts/<filename>`, served by the static file server at `/assets/fonts/<filename>` |
| Built-in default | `html/assets/fonts/Milonga-Regular.ttf`, declared directly in each theme's `styles_epoch3.css` - **not** a `fonts` document, so it can't be deleted from the dashboard |

`name` is both the label shown in the dashboard and the literal CSS `font-family` value.

---

## Dashboard

`/dashboard/settings/fonts` - epoch 3, **admin only** (`fonts_admin_list()`,
`html/templates/dashboard/fonts/`).

| Action | Route | Behavior |
|---|---|---|
| List | `GET /dashboard/settings/fonts` | Table of uploaded fonts. Each name is rendered in its own font: the page carries one `@font-face` per font. `?error=` shows a message above the table. |
| Upload | `POST /dashboard/settings/fonts/upload` (multipart `name`, `file`) | See validation below; writes the file, inserts the document, redirects back. |
| Delete | `POST /dashboard/settings/fonts/<id>/delete` | Removes the file and the document. Themes that referenced it fall back to the default (the name no longer resolves to a file). |

### Validation

- `name`: only `[A-Za-z0-9 _-]` is kept (max 80 chars). It ends up both in a MongoDB document
  and inside CSS (`font-family:'<name>'`), so anything that could break out of either context is
  dropped.
- `file`: sanitized like theme assets (`[A-Za-z0-9._-]`, no leading `.`, no `..`) and must end in
  `.ttf`, `.otf`, `.woff` or `.woff2`.
- Missing name or invalid file → redirect to `/dashboard/settings/fonts?error=…`.
- An existing file with the same sanitized name is **overwritten**.

---

## Using a font

| Control | Stored as | Applies when |
|---|---|---|
| Logo panel, epoch 3, text mode → *Font* | `themes.logo.epoch3.font` = `uploaded:<name>` (or `system:<name>`) | Always, while that panel is in text mode |
| Themes → Colors panel → *Logo font* | `themes.logo_font` = `<name>` | When the logo panel isn't in text mode |
| neither | - | Milonga |

At render time `build_logo_font_css()` (`page_layout.c`) resolves the name with
`cms_get_font_filename_by_name()` and injects, inside the `{{THEME_COLORS}}` style block:

```css
@font-face{font-family:'<name>';src:url('/assets/fonts/<file>') format('<hint>');}
:root{--br-font-navbar-logo:'<name>';}
```

`<hint>` comes from `cms_font_format_for_filename()`: `.woff2` → `woff2`, `.woff` → `woff`,
`.ttf` → `truetype`, `.otf` → `opentype`, anything else → empty (ignored by browsers).

Only epoch 3 uses uploaded fonts. Epochs 1/2 render logos as images, epoch 0 and WML as text.

---

## Licensing note

Uploaded fonts are served publicly. Only upload fonts whose license allows web embedding.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
