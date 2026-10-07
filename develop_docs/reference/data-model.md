# Boat Rudder - Data Model

Every persistent piece of a site lives in **one MongoDB database** (`mongodb_db` in
[configuration.md](configuration.md)) - one database per site. This document is the single
catalog of its collections: their shape, who reads and writes them, and how they relate. Other
documents link here instead of repeating schemas.

Diagram: [diagrams/data-model.puml](../diagrams/data-model.puml)

All access goes through `src/db/` (plus `src/modules/analytics*/` for the two analytics
collections). Nothing else in the code base opens a collection.

---

## Conventions

| Convention | Meaning |
|---|---|
| `map<lang,string>` | A translated text field: an open sub-document keyed by ISO 639-1 codes (`{"en": "...", "es": "..."}`). Resolved per request by `resolve_lang_map()` (`src/db/bson_lang.c`) to the request's language, falling back to `en`, then `""`. Adding a language never needs a migration. |
| Epoch-keyed sub-document | A per-epoch value (`banner_html`, `footer_html`, `logo_html`, `logo`) is keyed `epoch_neg1`, `epoch0` … `epoch3` - BSON field names cannot start with `-`, so epoch −1 is spelled out (`epoch_field_name()` in `cms_themes.c`). |
| ObjectId ↔ hex | Every C struct carries ObjectIds as 24-char hex strings (`char id[25]` or `char *id`); conversion happens in `src/db/`. |
| Graceful degradation | Every reader returns an empty result (or a documented default) when MongoDB is down, so a page never fails because of the database - see [configuration.md](configuration.md#notes). |
| Indexes | Only the `sessions` indexes are created by the code (`session_manager_ensure_indexes()` at startup); every other collection has `_id` only. See [Index recommendations](#index-recommendations). |

---

## Collection catalog

| Collection | Purpose | Source (C) | Defined in |
|---|---|---|---|
| `users` | Dashboard accounts | `auth.c`, `cms_users_admin.c` | `mongodb_manager.h` |
| `sessions` | Server-side login sessions | `session_manager.c` | `mongodb_manager.h` |
| `entries` | Pages and blog posts, with embedded header and content blocks | `cms_entries.c`, `cms_entries_admin.c` | `mongodb_manager.h` |
| `entry_categories` | Blog categories | `cms_categories.c` | `mongodb_manager.h` |
| `languages` | Active content languages | `cms_languages.c` | `mongodb_manager.h` |
| `menu` | Navigation bar items | `cms_menu.c` | `mongodb_manager.h` |
| `media` | Uploaded image metadata | `cms_media.c` | `mongodb_manager.h` |
| `media_directories` | Per-author media folders | `cms_media.c` | `mongodb_manager.h` |
| `media_galleries` | Image lists behind `gallery` blocks | `cms_media_galleries.c` | `mongodb_manager.h` |
| `site_settings` | Singleton: site name, active theme | `cms_site_settings.c` | `cms_site_settings.c` |
| `themes` | Sparse per-theme overrides (colors, banner, footer, logo, CSS) | `cms_themes.c` | `cms_themes.c` |
| `fonts` | Uploaded font library | `cms_fonts.c` | `cms_fonts.c` |
| `short_links` | Short codes for QR codes | `short_links.c` | `short_links.c` |
| `page_visits_daily` | Site-wide analytics, one document per day | `analytics.c` (write), `analytics_view.c` (read) | both `.c` files |
| `entry_visits_daily` | Per-entry analytics, one document per entry per day | `analytics.c` (write), `analytics_view.c` (read) | both `.c` files |

Staging collections `page_visits_daily_import` / `entry_visits_daily_import` exist only
transiently while running the `2026-10-03-merge-analytics.js` migration, which drops them when
done (see [migrations.md](migrations.md)).

---

## Content

### `entries`

One document per page or blog post. Everything read and written together - routing, header,
blocks, translations - is embedded; only categories, authors and galleries are references.
Design rationale: [plans/cms-entry-model-plan.md](../plans/cms-entry-model-plan.md).

```jsonc
{
  "_id": ObjectId,
  "link": "my-post",                 // URL slug: /blog/<link> or /page/<link>
  "type": "blog" | "page",
  "enabled": true,                   // false = draft (see below)
  "created_by": ObjectId,            // -> users._id; ownership for the "author" role
  "categories": [ObjectId, ...],     // -> entry_categories._id
  "header": {
    "title":   map<lang,string>,
    "summary": map<lang,string>,
    "image_url": "/content/posts/<user>/<dir>/photo.jpg", // bare path, no size suffix
    "author_id": ObjectId,           // -> users._id, resolved to users.name at render time
    "date": ISODate,                 // shown as "YYYY-MM-DD"
    "hide_author": false             // true: public views omit the byline
  },
  "content": [
    { "_id": ObjectId, "type": "paragraph", "order": 0,
      "text": map<lang,string>, "extra_data": "note" }
  ]
}
```

| Field | Notes |
|---|---|
| `enabled` | `false` means **draft**: public routes answer `404` unless the visitor has a valid dashboard session, in which case the entry renders with a `[Draft]` title prefix and `Cache-Control: no-store` (`serve_cms_entry()` + `include_drafts`). New entries are created as drafts. |
| `content[].type` | One of `title`, `paragraph`, `byline`, `image`, `gallery`, `separator`, `link`, `list`, `youtube-embed`, `code-text`, `generic`, `table`, `social-networks`. Unknown types are skipped when rendering. Per-type `text`/`extra_data` formats: [rendering.md](rendering.md#content-block-types). |
| `content[].extra_data` | Untranslated, type-specific configuration (e.g. image `caption|width|align`, code-text language, gallery `_id`). |
| `content[].text` | Usually `map<lang,string>`; for `image` the value is the bare image path. |

Readers: `cms_entries.c` (public pages, blog lists, admin listing), `cms_entries_admin.c` (editor),
`cms_media_galleries.c` (gallery back-link), `qr_back_label()` in `http_router.c`.
Writers: `cms_entries_admin.c` only.

### `entry_categories`

```jsonc
{ "_id": ObjectId, "name": map<lang,string> }
```

The public URL `/blog/category/<slug>` is **derived** with `slugify(name)` in the request's
language - slugs are not stored, so renaming a category changes its URL. Deleting a category
leaves dangling ids in `entries.categories[]`; readers ignore ids that don't resolve.
CRUD: `cms_categories.c`. Resolved for display by `cms_entries.c`.

### `menu`

```jsonc
{ "_id": ObjectId, "link": "/page/about", "order": 1, "enabled": true,
  "name": map<lang,string> }
```

Public navbar: `enabled: true`, sorted by `order`, at most `MENU_ITEM_LIMIT` (20).
CRUD: `cms_menu.c`.

### `languages`

```jsonc
{ "_id": ObjectId, "code": "en", "name": "English", "is_default": true }
```

Exactly one document has `is_default: true`. Seeded with English at startup if the collection is
empty (`cms_languages_ensure_seeded()`). Codes are restricted to `LANGUAGE_CATALOG`
(`src/db/language_catalog.c`). The default language cannot be removed, nor the last one.

---

## Media

Full behavior: [media-admin.md](media-admin.md).

### `media`

```jsonc
{ "_id": ObjectId, "name": "photo.jpg", "date": "2026-10-01 12:00:00",
  "format": "jpg", "author_id": ObjectId, "dir_id": ObjectId }
```

`name` is the bare filename (the optimizer's `_full` suffix stripped). Files live at
`html/content/posts/<username>/<directory name>/<name>` plus the size variants
`_full/_half/_small` (same extension) and `_medium/_micro` (always `.gif`); `<username>` is the
part of the author's email before `@`. Listings join `users` and `media_directories` with an
aggregation `$lookup`, run through `mongodb_manager_aggregate()` (see
[Known driver issue](#known-driver-issue)).

### `media_directories`

```jsonc
{ "_id": ObjectId, "name": "trips", "parent": "posts", "author_id": ObjectId }
```

Directory names are 3-60 chars of `[a-zA-Z0-9_-]`. A directory named `default` is created
on demand per author when an upload arrives with no directory selected.

### `media_galleries`

```jsonc
{ "_id": ObjectId, "content": ["/content/posts/u/d/a.jpg", ...], "entry_id": ObjectId }
```

Created/updated by `POST /dashboard/api/entries/<id>/content` for every `gallery` block; the
gallery `_id` is stored back in that block's `extra_data`. `entry_id` powers the
"Back to <entry title>" link on `/gallery/<id>`. Galleries are not deleted when their block or
entry is.

---

## Users and sessions

### `users`

```jsonc
{ "_id": ObjectId, "name": "Jane", "email": "jane@example.org",
  "password": "$argon2id$...", "role": "admin" | "author" }
```

`password` is a libsodium `crypto_pwhash_str()` (Argon2id) hash. A missing `role` means
`admin` (backward compatibility). The last admin cannot be deleted or demoted. Roles are
described in [dashboard.md](dashboard.md).

### `sessions`

```jsonc
{ "_id": ObjectId, "user_id": ObjectId, "token": "<64 hex chars>",
  "created_at": ISODate, "expires_at": ISODate }
```

The token is 32 CSPRNG bytes, hex-encoded, and is the `session` cookie's value. Validation
checks `expires_at` in code. At startup `session_manager_ensure_indexes()` creates
`{token: 1}` (unique, `token_unique`) and a TTL index `{expires_at: 1}`
(`expireAfterSeconds: 0`, `expires_at_ttl`), so MongoDB deletes expired sessions on its own, and
removes the ones already expired. The dashboard's CSRF token is derived from `token`, not stored.

---

## Site personalization

Full behavior: [themes.md](themes.md) and [fonts.md](fonts.md).

### `site_settings`

A singleton (always updated with filter `{}` and `upsert: true`).

```jsonc
{ "_id": ObjectId, "site_name": "My Site", "active_theme": "dark" }
```

`site_name` defaults to `"Boat Rudder"` and replaces the `{{SITE_NAME}}` token in titles and
templates. `active_theme` is only honored if `html/themes/<key>/` exists.

### `themes`

**Sparse**: a document exists only for a theme an admin has customized; every missing field falls
back to the theme's on-disk files or to `THEME_DEFAULTS` in `cms_themes.c`.

```jsonc
{
  "_id": ObjectId,
  "key": "dark",                         // directory name under html/themes/
  "colors": {                            // 40 tokens, hyphenated names (see themes.md)
    "navbar-background": "#1a1a2ecc",    // *_background fields accept #rrggbbaa
    "code-keyword": "#ffff55", ...
  },
  "banner_html": { "epoch_neg1": "...", "epoch0": "...", ..., "epoch3": "..." },
  "footer_html": { ...same keys... },
  "logo_html":   { ...same keys... },    // legacy raw-markup logo, fallback only
  "logo": {                              // structured per-epoch logo (CmsLogoConfig)
    "epoch3": { "mode": 1, "text": "", "font": "uploaded:Milonga",
                "navbar_image": "", "footer_image": "" }, ...
  },
  "logo_font": "MyFont",                 // epoch 3 navbar font from the Colors panel
  "css_epoch3": "...",                   // full override of styles_epoch3.css; "" = use file
  "css_admin_epoch3": "..."              // full override of styles_admin_epoch3.css; "" = use file
}
```

`logo.<epoch>.mode` is the `CmsLogoMode` enum: `0` unset, `1` text, `2` image.

### `fonts`

```jsonc
{ "_id": ObjectId, "name": "My Font", "filename": "MyFont-Regular.woff2" }
```

Files live under `html/assets/fonts/`. `name` is both the display name and the CSS
`font-family` value, so it is restricted to `[A-Za-z0-9 _-]` on upload.

### `short_links`

```jsonc
{ "_id": "a1B2c3D", "target_path": "/gallery/66f..." }
```

`_id` is a 7-character base62 code (libsodium `randombytes_uniform`). `short_link_get_or_create()`
is idempotent per `target_path` and retries up to 5 times on a duplicate-key error. Full
behavior: [qr-and-short-links.md](qr-and-short-links.md).

---

## Analytics

Full behavior, privacy notes and the report: [analytics.md](analytics.md).

### `page_visits_daily`

One document per UTC day, upserted with a single `$inc` per visit.

```jsonc
{
  "_id": "2026-10-06",                 // also the range-query key
  "date": "2026-10-06", "year": 2026, "month": 10, "week": 41,  // ISO week
  "total": 123,
  "by_epoch":   { "epochwml": 2, "epoch0": 1, "epoch1": 0, "epoch2": 5, "epoch3": 115 },
  "by_browser": { "Chrome/124": 80, ... },
  "by_os":      { "Windows/10": 60, ... },
  "by_country": { "Chile": 40, "Unknown": 3, ... },
  "by_route":   { "home": 50, "blog": 40, "page": 20, "gallery": 5, "login": 1, "other": 7 }
}
```

### `entry_visits_daily`

One document per entry (or category) per day, only for URLs that name one.

```jsonc
{
  "_id": "blog__my-post__2026-10-06",
  "date": "2026-10-06", "year": 2026, "month": 10, "week": 41,
  "entry_type": "blog" | "page" | "category", "slug": "my-post",
  "total": 7,
  "by_epoch": { "epoch3": 6, "epoch2": 1 }
}
```

Neither collection stores an IP address or any per-visit record.

---

## Relationships

```
users 1───* sessions            (sessions.user_id)
users 1───* entries             (entries.created_by, entries.header.author_id)
users 1───* media               (media.author_id)
users 1───* media_directories   (media_directories.author_id)
media_directories 1───* media   (media.dir_id)
entry_categories *───* entries  (entries.categories[])
entries 1───* media_galleries   (media_galleries.entry_id; entries.content[].extra_data = gallery _id)
entries ···> media              (paths only: header.image_url, image/gallery blocks - no ObjectId)
themes.key ···> html/themes/<key>/        (directory, not a document)
site_settings.active_theme ···> themes.key / html/themes/<key>/
themes.logo.*.font / themes.logo_font ···> fonts.name
short_links.target_path ···> any public path
entry_visits_daily.slug ···> entries.link | slugify(entry_categories.name)
```

There are no foreign-key constraints: deleting a user, category, entry or media item never
cascades. Readers tolerate dangling references.

---

## Index recommendations

Apart from `sessions` (whose indexes the server creates itself), the code relies on the default
`_id` index only. Queries that would benefit from an index as a site grows (create them by hand
with `mongosh`; nothing in the code depends on them existing):

| Collection | Query | Suggested index |
|---|---|---|
| `entries` | `cms_get_entry_by_link()` by `link`; lists by `{type, enabled}` sorted by `header.date` | `{link: 1}`, `{type: 1, enabled: 1, "header.date": -1}` |
| `users` | `auth_login_user()` by `email` | `{email: 1}` unique (uniqueness is currently enforced only in code) |
| `short_links` | `find_code_for_path()` by `target_path` | `{target_path: 1}` unique |
| `media` | listings by `dir_id` | `{dir_id: 1, _id: -1}` |
| `entry_visits_daily` | report filters on `{year, month}`, `{year, week}`, `date` | `{date: 1}`, `{year: 1, month: 1}` |
| `page_visits_daily` | report filters on `{year, month}` / `{year, week}` | `{year: 1, month: 1}` |

---

## Known driver issue

`mongoc_collection_aggregate()` is broken in libmongoc **1.30.4-1+deb13u3** (Debian security
update, 2026-09-26): it fails client-side with `database name "<db>.<collection>" invalid` and
never sends the command. All aggregations therefore go through
`mongodb_manager_aggregate()` (`src/db/mongodb_manager.c`), which runs the `aggregate` command
directly and wraps the reply in a cursor. Today its callers are the two media listings in
`cms_media.c`. New code must use it too.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
