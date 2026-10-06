# Boat Rudder - Analytics

A built-in, cookie-less visit counter: every public page view increments a per-day aggregate in
MongoDB, and `/dashboard/analytics` reports on those aggregates by period, epoch, browser,
operating system, country and route. There is no raw event log, no client-side script and no
third-party service.

Ported from the-retro-center-old's `analytics.c`, adapted to Boat Rudder's routes and
`mongodb_manager.c`. Diagram: [diagrams/analytics-flow.puml](../diagrams/analytics-flow.puml)

---

## Source map

| File | Role |
|---|---|
| `src/modules/analytics/analytics.c/h` | `analytics_track_visit()`: filtering, classification, `$inc` upserts |
| `src/modules/analytics/geoip.c/h` | Optional GeoLite2 country lookup (`HAVE_MAXMINDDB`) |
| `src/utils/ua_parser.c/h` | User-Agent → browser and OS keys |
| `src/modules/analytics_view/analytics_view.c/h` | `/dashboard/analytics` report |
| `src/modules/analytics_view/country_continent.c/h` | Country name → continent, for grouping |
| `html/templates/dashboard/analytics/analytics_epoch3.html` | Report template |
| `scripts/migrations/2026-10-03-merge-analytics.js` | Imports buckets from another database by summing counters |

---

## Recording a visit

`http_route()` calls `analytics_track_visit(method, decoded_url, User-Agent, client_ip, epoch)`
**once per request, after the per-request state is set and before routing** - so it runs
regardless of which route (or error) ends up answering. `epoch` is `resolve_epoch()`'s result,
i.e. it honors `force_epoch` and `?preview_epoch=`.

### What is excluded

| Rule | Implementation |
|---|---|
| Any method other than `GET` (including `HEAD`) | first check |
| Empty URL | first check |
| URLs under `/dashboard` (the admin area would pollute its own report) | `SKIP_PREFIXES` |
| Static assets, by **file extension** anywhere in the path: `.css .js .png .jpg .jpeg .gif .ico .svg .woff .woff2 .ttf .otf .eot .webp .mp4 .webm .pdf .zip .gz .map .json .wbmp` | `STATIC_EXTENSIONS` |
| Bots and tools: User-Agent containing (case-insensitive) `bot`, `crawl`, `spider`, `slurp`, `mediapartners`, `facebookexternalhit`, `baiduspider`, `yandex`, `sogou`, `ia_archiver`, `archive.org`, `pingdom`, `uptimerobot`, `curl/`, `python-requests`, `wget/`, `java/`, `go-http` | `BOT_TOKENS` |

Everything else that is a `GET` is counted - including requests that end in `404`, redirects
such as `/qr/<code>` or `/language/set`, and the pages the dashboard's preview iframe loads (they
are ordinary public URLs with `?preview_epoch=`). Those land in the `other` route bucket or under
the previewed epoch.

The WAP gateway's loopback fetches are counted too (one per phone request), with epoch
`epochwml`. Whether their country is the phone's or `Unknown` depends on `127.0.0.1` being in
`trusted_proxies` (see [wap-gateway.md](wap-gateway.md#loopback-fetch)).

### Classification

| Dimension | Source | Key examples |
|---|---|---|
| Day | `gmtime()` - **UTC** | `"2026-10-06"`, plus `year`, `month`, ISO `week` (`%V`) |
| Epoch | `resolve_epoch()` | `epochwml`, `epoch0` … `epoch3` |
| Browser | `ua_parse_browser()` | `Chrome/124`, `Firefox/115`, `MSIE/6`, `Netscape/4`, `Mosaic/2`, `Cello`, `Lynx/2`, `w3m/0`, `UP_Browser/4`, `WinWAP/3`, `Unknown` |
| OS | `ua_parse_os()` | `Windows/10`, `Windows/8_1`, `Windows/95`, `Windows/3x`, `Mac/10`, `Linux/x86_64`, `Android/14`, `iOS/17`, `BeOS/5`, `OS2`, `Unknown` |
| Country | `geoip_lookup(client_ip)` | `Chile` (GeoLite2 English name), `CL` (ISO code when no English name), `Unknown` |
| Route | `classify_url()` | `home` (`/`), `login`, `blog` (`/blog`, `/blog/<slug>`, `/blog/category/<slug>`), `page` (`/page/<slug>`), `gallery`, `other` |
| Entry | `classify_url()` | `blog`/`page`/`category` + slug, for `/blog/<slug>`, `/page/<slug>`, `/blog/category/<slug>` only |

`ua_parser.c` is a priority-ordered chain of substring checks - no regex, no bundled UA
database. Order matters (Edge and Opera also contain `Chrome/`, so they're tested first). Versions
are the major number only. Every key goes through `mongo_key_sanitize()` (`.` and `$` → `_`)
because it becomes a dotted `$inc` path.

### Writing

Two `update_one({_id}, {$setOnInsert: {...}, $inc: {...}}, {upsert: true})` calls per counted
visit (the second only when the URL names an entry):

- `page_visits_daily`, `_id = "YYYY-MM-DD"`: `total`, `by_epoch.*`, `by_browser.*`, `by_os.*`,
  `by_country.*`, `by_route.*`.
- `entry_visits_daily`, `_id = "<entry_type>__<slug>__YYYY-MM-DD"`: `total`, `by_epoch.*`.

Exact shapes: [data-model.md](data-model.md#analytics). A day's document is the same size on day
1000 as on day 1 apart from new distinct keys, and there is nothing to prune.

The write is **synchronous**, on the connection thread, before the page is rendered: two MongoDB
round-trips per counted page view. If MongoDB is down, `mongodb_manager_get_collection()` returns
`NULL` and the visit is silently dropped; write errors are logged at `ERROR`.

---

## GeoIP (optional)

Country detection uses MaxMind's **GeoLite2-Country** database through **libmaxminddb**.

| Situation | Behavior |
|---|---|
| Built without libmaxminddb (`pkg-config libmaxminddb` not found by CMake) | `HAVE_MAXMINDDB` undefined: `geoip.c` compiles to stubs, startup logs that country detection is disabled, every country is `Unknown`. |
| Built with it, file missing or unreadable | `geoip_init()` logs a warning; every country is `Unknown`. |
| Built with it, file present | Database opened with `MMDB_MODE_MMAP` at startup; English name, else ISO code, else `Unknown` (private/loopback ranges, misses). |

- The path is fixed: `GEOIP_DEFAULT_DB_PATH` = `./data/GeoLite2-Country.mmdb`, relative to the
  working directory (not configurable in `settings.conf`). `scripts/install.sh` copies it to the
  install directory and warns if it's missing.
- `geoip_init()` runs after `mongodb_manager_init()` and before `server_start()`;
  `geoip_cleanup()` runs at shutdown after `server_stop()`.
- To update the database, replace the file and restart.
- **Licensing:** GeoLite2 is distributed by MaxMind under its GeoLite2 EULA, which restricts
  redistribution. See [third-party.md](third-party.md#geolite2) - note that the repository
  currently tracks `data/GeoLite2-Country.mmdb`.

---

## The report

`GET /dashboard/analytics` - epoch 3, **admin only** (`require_admin_session()`).
Implemented by `analytics_view()`; one template, `dashboard/analytics/analytics_epoch3.html`.

### Periods

| `period` | Other parameters | MongoDB filter on `page_visits_daily` | Label |
|---|---|---|---|
| `day` | `date=YYYY-MM-DD` (default today, UTC) | `{_id: date}` | the date |
| `week` | `year`, `week` (default current ISO week) | `{year, week}` | `Week W / YYYY` |
| `month` (default, also any unknown value) | `year`, `month` | `{year, month}` | `October 2026` |
| `year` | `year` | `{year}` | `2026` (buttons for this year and last year) |
| `all` | - | `{}` | `All Time` |
| `range` | `from`, `to` (`YYYY-MM-DD`) | `{_id: {$gte: from, $lte: to}}` (lexicographic = chronological) | `from - to` |

`entry_visits_daily` uses the same filters, with `date` instead of `_id` for `day`/`range`.

### Sections

| Section | Built by | Notes |
|---|---|---|
| Totals | `merge_daily_doc()` | Total visits and per-epoch counters (WML, 0, 1, 2, 3) |
| Routes | `build_kv_table()` | Flat table with percentage bars |
| Browsers, Operating systems | `build_grouped_kv_table()` | Two-level `<details>` tree: family (`Chrome`, `Windows`) with its summed count, expanding to versions. Native HTML, no JavaScript |
| Countries | `build_continent_kv_table()` | Same tree, grouped by continent via `country_continent()` at report time (works for buckets recorded before continents existed); unmapped names and `Unknown` stay ungrouped |
| Top articles | `build_top_articles_table()` | Top 20 `[type] slug` by total across the period, merged per slug |

Every key list grows on the heap without a cap (an "All Time" query easily sees hundreds of
browser versions). All values are HTML-encoded before output.

---

## Importing history

`scripts/migrations/2026-10-03-merge-analytics.js` merges buckets restored into
`page_visits_daily_import` / `entry_visits_daily_import` into the live collections by **adding**
every numeric counter, so visits already recorded on the same days are kept. It drops the staging
collections when done, so re-running it with nothing restored is a no-op. Usage:
[migrations.md](migrations.md#2026-10-03-merge-analyticsjs).

---

## Privacy

- **No IP address is stored.** The client IP is used only in memory, to resolve a country name
  (`geoip_lookup()` in `analytics_track_visit()`), and is never written to MongoDB.
- **No per-visit record exists**: only per-day counters. Individual visits can't be
  reconstructed, and a User-Agent is reduced to a browser family + major version and an OS key.
- **No cookies or identifiers** are set or read for analytics; there is no unique-visitor
  concept - `total` counts page views.
- The server log (`verbose_level=3` and above) does record each request line, and at `4` the
  client IP and User-Agent - that is a log-retention concern, handled by `logrotate`
  ([scripts.md](scripts.md#boat-rudderlogrotate)), not by analytics.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
