# Site Settings (banner, colors, logo, favicon, site name) - Feasibility & Plan

> **Status**: proposal, not implemented. This is a documentation-only deliverable -
> no DB schema, C code, or templates described here exist yet.

Diagram: [diagrams/site-settings-components.puml](../diagrams/site-settings-components.puml)

## 1. Goal

Let an admin change, from `/dashboard`, a handful of site-identity elements without
editing files or redeploying:

- the site name ("Boat Rudder" / "BOAT RUDDER", currently a hardcoded literal in ~16
  templates),
- the home banner image,
- the favicon,
- an optional logo image,
- a small number of "brand" accent colors.

## 2. Current state (why this isn't uniform across epochs)

Boat Rudder's 5 epochs (`EPOCH_WML`=-1, pre-standard=0, `EPOCH_EARLY`=1,
`EPOCH_MIDDLE`=2, `EPOCH_MODERN`=3) are **hand-tuned per-epoch markup**, not one
responsive design - this is the project's core retro-compatibility philosophy (see
[architecture.md](../reference/architecture.md)). That means "centralize the banner /
colors" doesn't mean the same thing in every epoch:

| Element | epoch -1 (WML) | epoch 0 | epoch 1 | epoch 2 | epoch 3 |
|---|---|---|---|---|---|
| **Banner** | none - `<p align="center"><b>BOAT RUDDER</b></p>`, text only | none - `<h1>BOAT RUDDER</h1>` + tagline, text only | decorative `<table>` with 3 fixed images (`background.gif` x2, `floor.jpg`) **and** the "BOAT RUDDER" text in a `<font>` - not a single swappable image | same decorative table as epoch1, plus an `<h1>` title | **single `<img>`**: `<div class="boat-rudder__mainbanner"><img src="/themes/dark/assets/mainbanner/slider_epoch3.jpg"></div>` |
| **Favicon** | n/a (WML has no `<link>`) | n/a (no favicon ref in template) | n/a (no favicon ref in template) | `<link rel="icon" href="/favicon.ico">` in `page_epoch2.html`-style templates | `<link rel="icon" href="/favicon.ico">` in `container_epoch3.html` / `page_epoch3.html` |
| **Logo (`<img>`)** | none | none | none | none | none - site identity is text-only everywhere today |
| **Colors** | n/a - WML has no color model | n/a - plain HTML, no `bgcolor`/CSS | inline HTML attributes: `<body bgcolor="#000080" text="#FFFFFF" link="#56E9FD" vlink="#9999FF">` | hardcoded hex in `styles_epoch2.css` (~21 colors), no `:root`/custom properties | hardcoded hex in `styles_epoch3.css` (~76 colors, eclectic neon palette), no `:root`/custom properties |
| **Site name** | literal `BOAT RUDDER` in `container_epoch-1.html`, `home-content_epoch-1.html`, `page_epoch-1.html`, `slider_epoch-1.html` | same, 4 files | same, 4 files (`<font><b>BOAT RUDDER</b></font>`) | same, 4 files (`<h1>BOAT RUDDER</h1>` etc.) | same, 4 files + `menu/menu_epoch3.html` |

No media library exists (`media`/`media_directories` are still "proposed, not
implemented" per [cms-entry-model-plan.md](cms-entry-model-plan.md) §2.3), so any
image setting (banner/logo/favicon) is realistically a **URL/path string**, the same
convention already used for `entries.header.image_url` in the
[entry editor](../reference/entry-editor.md) - "paste a URL to an asset under
`/themes/dark/assets/...`", not "upload a file".

## 3. Recommendation / scope

**Phase 1 (this plan's concrete proposal), epoch3 + cheap cross-epoch text swap:**

- **Site name**: move the literal "Boat Rudder"/"BOAT RUDDER" string into
  `site_settings.site_name` and pass it through `render_template()` in all ~16
  templates across all 5 epochs. This is mechanical (find/replace a literal with a
  `%s` placeholder + one new argument per `render_template()` call) and gives every
  epoch a real win.
- **Favicon**: `site_settings.favicon_url` replaces the hardcoded
  `/favicon.ico` href in `container_epoch3.html` / `page_epoch3.html` (and, while
  touching those templates, epoch2's equivalents). Epoch -1/0/1 have no favicon
  concept - out of scope, nothing to change.
- **Banner image**: `site_settings.banner_image_url` replaces the hardcoded
  `slider_epoch3.jpg` `<img src>` in `slider_epoch3.html` only. Epoch -1/0/1/2 are left
  as-is (see §2 - there's no single image to swap there without a much larger
  re-template of the decorative table).
- **Logo**: `site_settings.logo_url`, optional. If set, `container_epoch3.html` (and
  `menu_epoch3.html`, where the site name currently lives) renders an `<img>` next to
  the site name; if empty (default), behavior is unchanged (text-only, as today).
  Epoch -1/0/1/2: out of scope (no existing logo concept, and images don't fit those
  layouts' philosophy).
- **Colors**: introduce **2-3 named CSS custom properties** in `styles_epoch3.css`
  (e.g. `--br-color-accent`, `--br-color-bg`, `--br-color-text`) covering the
  most-reused values, set via a small inline `<style>:root{...}</style>` fragment
  rendered into `container_epoch3.html` from `site_settings.colors`. This is **not**
  "recolor every one of the ~76 hardcoded hex values" - it's a few brand accents that
  an admin can tweak. Epoch -1/0 have no color model (out of scope). Epoch1's colors
  are inline `<body bgcolor=... text=... link=... vlink=...>` HTML attributes - these
  *could* be templated too (cheap, 4 more `render_template()` args on
  `container_epoch1.html`), but are a separate, smaller follow-up since they're a
  different mechanism (HTML attributes, not CSS). Epoch2's CSS hex values are left
  alone (would need its own `:root` introduction, same shape as epoch3 but a separate
  pass).

**Explicitly out of scope for this plan** (future phases, not blocking Phase 1):

- Re-templating epoch1/epoch2's decorative banner table to isolate a single swappable
  image.
- Full color-token coverage of `styles_epoch2.css`/`styles_epoch3.css` (every hex
  value).
- A media library / file-upload UI for banner/logo/favicon (still URL-based, per
  `cms-entry-model-plan.md` §2.3).
- Epoch1 inline `bgcolor`/`text`/`link`/`vlink` attributes (separate, smaller
  follow-up noted above).

**Tradeoff**: Phase 1 gives a real, useful win (name, favicon, banner, logo, a couple
of accent colors - all on the modern epoch3 experience most visitors see, plus the
site-name swap everywhere) for a moderate amount of work. Chasing full parity across
all 5 epochs would multiply the templating work several times over for designs that
are intentionally minimal/historical (WML and pre-standard HTML have no concept of
"banner image" or "color scheme" at all).

## 4. Proposed schema: `site_settings` (new collection, singleton)

```jsonc
// db.site_settings - exactly one document
{
  "_id": ObjectId,
  "site_name": "Boat Rudder",
  "banner_image_url": "/themes/dark/assets/mainbanner/slider_epoch3.jpg",
  "favicon_url": "/favicon.ico",
  "logo_url": "",            // "" = no logo, text-only (current behavior)
  "colors": {
    "accent": "#7b0a74",
    "background": "#000000",
    "text": "#ffffff"
  }
}
```

This is a new pattern for boat-rudder (existing collections - `languages`, `menu`,
`entry_categories` - are all multi-document lists). The closest precedent for "read
config with a hardcoded fallback if missing/unreachable" is `menu()`'s
`FALLBACK_ITEMS` (`src/modules/menu/menu.c`): if `site_settings` is empty or MongoDB is
down, `cms_get_site_settings()` returns the **current hardcoded literals** (today's
`site_name="Boat Rudder"`, `banner_image_url="/themes/dark/assets/mainbanner/slider_epoch3.jpg"`,
`favicon_url="/favicon.ico"`, `logo_url=""`, and the 3 colors read off the literals
already in `styles_epoch3.css`) - so an empty/unreachable DB never breaks the site,
it just looks exactly like it does today.

## 5. Proposed DB layer: `src/db/cms_site_settings.h` / `.c`

```c
typedef struct {
    char site_name[128];
    char banner_image_url[256];
    char favicon_url[256];
    char logo_url[256];
    char color_accent[8];      // "#rrggbb"
    char color_background[8];
    char color_text[8];
} CmsSiteSettings;

// db.site_settings.findOne({}). Fills `out` with the stored document, or with the
// hardcoded current-behavior defaults if the collection is empty/unreachable -
// always returns 1 (same "never empty" guarantee as menu()'s FALLBACK_ITEMS).
int cms_get_site_settings(CmsSiteSettings *out);

// db.site_settings.updateOne({}, {$set: {...}}, {upsert: true}) - singleton
// upsert, so the first save creates the document.
int cms_update_site_settings(const CmsSiteSettings *settings);
```

Fixed-size `char[]` fields (mirroring `header_date[16]` in `CmsEntryEdit`) are enough
here - these are short admin-entered strings, no need for the dynamic
`text_values[]`/`map<lang,string>` machinery used for translated entry content (site
name/colors/URLs are untranslated, single values).

## 6. Proposed admin UI: `/dashboard/settings`

A **single-document form**, not a list-with-rows like categories/menu/entries - closer
in shape to the entry editor's header sidebar than to `entries_admin`'s table.

- New module `src/modules/settings_admin/` (`settings_admin.h`/`.c`):
  `settings_admin_page(epoch, const CmsSiteSettings *settings)` renders
  `dashboard/settings/settings_epoch3.html` with the current values pre-filled into
  `<input>` fields (site name, banner/favicon/logo URLs, 3 color pickers `<input
  type="color">`).
- New template `html/themes/dark/dashboard/settings/settings_epoch3.html`
  (epoch3 only, same no-embedded-HTML convention - loaded via
  `generate_url_theme()` + `read_file_to_string()` + `render_template()`).
- Routes (mirroring categories/menu - form POST + redirect, **not** AJAX like the
  entry editor, since this is one small form saved as a whole):
  - `GET /dashboard/settings` - `epoch != EPOCH_MODERN` -> `302 /dashboard`;
    `require_dashboard_session()` -> `503`/`302 /login`/proceed; otherwise
    `cms_get_site_settings()` + `settings_admin_page()`, wrapped in
    `buildPageWebSite(epoch, "Boat Rudder - Dashboard", content)`.
  - `POST /dashboard/settings` - same guards; parses the form fields with
    `parse_urlencoded_field()`, calls `cms_update_site_settings()`, redirects back to
    `/dashboard/settings`.
- `/dashboard` gains a "Site settings" link (alongside the existing
  Categories/Languages/Menu/Entries maintainer links), per
  [architecture.md, "Dashboard maintainers"](../reference/architecture.md#dashboard-maintainers-entries-categories-languages-and-menu).

## 7. Rendering changes (consumers of `site_settings`)

Per [the diagram](../diagrams/site-settings-components.puml):

- **`container()`** (`src/modules/container/container.c`): calls
  `cms_get_site_settings()`, and `render_template()` on `container_epoch3.html` gains
  new `%s` placeholders for `site_name` (footer + page-title areas), `favicon_url`
  (the `<link rel="icon">` href), `logo_url` (conditionally-rendered `<img>`, empty
  string if no logo), and a new inline `<style>:root{ --br-color-accent: %s; ... }
  </style>` fragment consumed by `styles_epoch3.css` rules that opt in to
  `var(--br-color-accent, <current-hardcoded-value>)` (the fallback value keeps every
  rule working even before any rule is migrated to use the variable).
- **`slider()`** (`src/modules/slider/slider.c`): calls `cms_get_site_settings()`,
  passes `banner_image_url` as a new `%s` into `slider_epoch3.html`'s `<img src="%s">`.
  Epoch -1/0/1/2 templates are untouched (no new placeholder, `slider()` doesn't need
  the DB call for those epochs).
- **`menu()`** (`src/modules/menu/menu.c`): epoch3's `menu_epoch3.html` has a
  hardcoded "BOAT RUDDER" site-name link - gains a `site_name` `%s` placeholder.
- The other ~12 occurrences of the literal "Boat Rudder"/"BOAT RUDDER" (epoch -1/0/1/2
  `container`/`home-content`/`page`/`slider` templates) each gain one `%s` placeholder
  and one new `render_template()` argument (`site_name`) in their respective module
  (`container()`, `home_content()`, `slider()`, and `page` is rendered via
  `buildPageWebSite()` so `container()`'s value can be reused/passed through).

Every render-path consumer calls `cms_get_site_settings()` per request - same
per-request-DB-read pattern already used by `menu()` (reads the `menu` collection on
every call, with `FALLBACK_ITEMS` if empty/down). No caching layer is introduced; if
this becomes a measurable cost later, that's a separate, isolated optimization
(e.g. a TTL cache in `cms_site_settings.c`) that doesn't change the schema or API.

## 8. Open questions

1. **Color picker UX**: `<input type="color">` gives a hex value directly - good fit
   for `colors.accent`/`background`/`text` as plain `#rrggbb` strings. Confirm 3
   colors is the right starting set (vs. e.g. just 1 "accent" color)?
2. **Site-name propagation scope**: Phase 1 proposes touching all ~16 templates (cheap
   per-file, but it's still ~16 files + their module's `render_template()` call
   signature). Alternative: ship `site_name` for epoch3 only first (4-5 files), defer
   epoch -1/0/1/2 to a quick follow-up? Leaning towards doing all 16 together since
   each change is mechanical and it avoids a half-migrated state.
3. **Validation**: should `cms_update_site_settings()` validate `*_url` fields look
   like paths/URLs, or accept any string (consistent with the existing
   no-validation convention for `entries.header.image_url`)? Leaning towards no
   validation, matching existing convention.
4. **Logo placement**: if `logo_url` is set, does it render *instead of* or
   *alongside* the "Boat Rudder" text in `container_epoch3.html`'s footer /
   `menu_epoch3.html`'s site-name link? Proposal: alongside (logo `<img>` + text),
   simplest template change, avoids a "text-only vs. logo-only" branch in C.
5. **Epoch1 colors / epoch2 CSS tokens / epoch1-2 banner re-template**: confirmed
   out of scope for Phase 1 (§3) - worth a one-line mention in
   [architecture.md](../reference/architecture.md) as future work once Phase 1 lands,
   same as the entry editor's "Future work" section.

## 9. Relationship to Boat Rudder's current code

Nothing here is implemented. Once approved, the natural implementation order mirrors
the entry editor's (per [entry-editor.md](../reference/entry-editor.md)): DB layer
(`cms_site_settings.h/.c`) -> admin module + templates
(`settings_admin/`, `dashboard/settings/settings_epoch3.html`) -> routes
(`/dashboard/settings` GET/POST) -> `/dashboard` link -> consumer changes
(`container`, `slider`, `menu`, plus the ~12 site-name-only templates) -> CMake
wiring -> `architecture.md` updates -> end-to-end verification (admin saves new
values, confirm they appear on `/`, `/page/<link>`, favicon, and that an
empty/missing `site_settings` document reproduces today's exact output).
