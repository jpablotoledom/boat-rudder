# Plan: machine translation in the entry editor

> **Status**: implemented (2026-11-02). Reference: [entry-editor.md](../reference/entry-editor.md#translation).

---

## 1. What is asked

Write an entry in one language and press a button that fills in the others: the title, the
summary and each block's text, through a translation service.

Decided:

1. **Two providers**: **Azure AI Translator** (its F0 tier: 2 million characters a month, free
   for good) and a **local translator** - a LibreTranslate-compatible server on the editor's own computer. A desktop app that runs one comes later; Boat Rudder only needs the option to be ready.
2. **Only empty fields are filled**: a translation already written is never overwritten.
3. **Saving**: with autosave on, the result is saved right away; otherwise it waits in the form
   for *Save all*, like any other edit.

## 2. Design

### 2.1 Where each provider runs

| Provider | Who calls it | Why |
|---|---|---|
| Azure | **The server**, `POST /dashboard/api/translate` | The API key stays on the server; the browser never sees it |
| Local | **The browser**, straight to its URL (default `http://localhost:5000`) | The server can't reach the editor's computer - the site is on a VPS, the translator on the desk |

Both answer the same thing to the editor's script - a list of texts in, their translations out -
so the translation loop is the same and only `translateBatch()` differs.

The local translator works from a site served over HTTPS because browsers treat
`http://localhost` / `127.0.0.1` as a secure origin (no mixed-content block). It has to answer
CORS (LibreTranslate does); Chrome may ask the editor once to allow access to the local network.

### 2.2 Settings (`/dashboard/settings`, panel "Translation")

Administrador only. Stored in `site_settings`:

```
translate_provider:     "" | "azure" | "local"  // "" = off: no button in the editor
translate_azure_key:    "..."                    // write-only in the form
translate_azure_region: "westeurope"             // the resource's region, "" for a global one
translate_local_url: "http://localhost:5000"
translate_local_key: "..."                       // optional, LibreTranslate's api_key
```

- The Azure key is never sent back to the page: the form shows *A key is saved (…abcd)* and an
  empty field; leaving it empty keeps the stored key. The region goes in the
  `Ocp-Apim-Subscription-Region` header, so it may only hold letters, digits and dashes.
- In the database, not `settings.conf`: each site has its own key, and it can never end up in
  a commit.
- `configs/settings.conf`'s optional `translate_azure_url` replaces Azure's address (a proxy, or
  a test server).

### 2.3 The editor

A **Translate** button in the top bar, next to the language tabs, shown only when a provider is
set and the site has more than one language. It opens a small panel:

- **From**: the language tab being edited.
- **To**: every other language, checked by default.
- **Translate** → for each target language, fill each **empty** field whose source isn't empty:

| Field | Sent as |
|---|---|
| Header title, summary | text |
| `title`, `link` blocks | text |
| `list` | line by line, so its lines survive |
| `paragraph` | HTML - tags kept |
| `table` | cell by cell, so the `\|` separators survive |
| `image`, `gallery`, `byline`, `youtube-embed`, `code-text`, `separator`, `social-networks`, `generic` | not translated (paths, names, URLs, code, raw HTML) |

Progress shows in the save status (*Translating to English…*). The result goes into the fields (and
the rich-text editor of an opened paragraph), the entry is marked changed, and with autosave on
`editorSaveAll()` runs at the end. An error stops the run and says which field failed; what was
already filled stays.

Language codes: Boat Rudder's ISO 639-1 codes, which Azure takes as they are - only Chinese
needs its script, `zh-Hans`.

### 2.4 `POST /dashboard/api/translate`

`require_dashboard_session()` (any role that edits - not a viewer: it spends the site's quota),
CSRF like every dashboard POST. Fields `text` (repeated: up to 50, 50,000 characters in all - Azure's own per-request limit), `source`,
`target` (codes of the site's languages), `format` (`text` | `html`) →
`{"ok":true,"texts":[...]}` or `{"ok":false,"error":"..."}`. The editor batches its texts, so an
entry is one or two calls per language. Only when the provider is Azure: with `local` the browser calls its
own server.

## 3. Security

| Concern | Answer |
|---|---|
| The Azure key leaking | Server side only, never rendered, not in `settings.conf` |
| Spending the quota from outside | Session + CSRF; a viewer can't call it |
| Translated text injecting markup | Text fields are plain values; a paragraph's HTML is what that block stores anyway, and it's the editor's own text |
| The local URL | Only the browser calls it; the server never fetches it (no SSRF) |

## 4. Work

| # | Piece | Files |
|---|---|---|
| 1 | Settings read/write | `src/db/cms_site_settings.{c,h}` |
| 2 | Azure client (libcurl, JSON via libbson) | `src/utils/translator.{c,h}` |
| 3 | Settings panel + POST | `site_settings_admin`, `settings-translation_epoch3.html`, `http_router.c` |
| 4 | API route | `http_router.c` |
| 5 | Editor button, panel, loop | `entry_editor.c`, `container_epoch3.html`, CSS |
| 6 | Tests: a mock Azure and a mock local server | manual script |
| 7 | Docs: entry-editor.md, dashboard.md, data-model.md, configuration.md, routes.md, CHANGELOG | `develop_docs/` |

## 5. The local translator's contract (for the desktop app)

Anything that answers this works:

```
POST <url>/translate
Content-Type: application/json
{"q": "...", "source": "es", "target": "en", "format": "text" | "html", "api_key": "..."}

200 {"translatedText": "..."}
error {"error": "..."}
```

with `Access-Control-Allow-Origin` for the site (or `*`) and an answer to the `OPTIONS`
preflight.
