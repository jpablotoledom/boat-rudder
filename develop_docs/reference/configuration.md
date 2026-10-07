# Boat Rudder - Configuration

`configs/settings.conf` is the only configuration file. It is an INI-style `key=value` list
parsed by `src/utils/config_loader.c`; blank lines and lines starting with `#` are ignored, and
an absent key keeps its built-in default. The path can be overridden on the command line:

```
boat-rudder -c /path/to/settings.conf /path/to/html-root
```

**This document is the single reference for every key.** The other documents link here instead
of repeating the list, so there is exactly one place to update when a key is added. Every key is
declared (with its doc comment) in `src/utils/config_loader.h` and given its default in
`src/utils/config_loader.c`.

> **Defaults vs. site values.** The defaults below are Boat Rudder's, not any particular site's.
> A site running on Boat Rudder overrides whatever it needs - its ports, its theme, its own
> MongoDB database - and those values live in that deployment's `settings.conf`, never in the
> source tree. See the naming note in [architecture.md](architecture.md).
>
> The `configs/settings.conf` checked into the repository currently carries a *development
> host's* values (`http_port=80`, `ssl_enabled=1`, `verbose_level=4`, a LAN `public_url`,
> `wap_gateway_enabled=1`), not the built-in defaults listed here. Treat the table below, not
> that file, as the reference for defaults.

---

## Full example

All keys, at their built-in defaults:

```ini
verbose_level=3           # 0=none 1=error 2=warn 3=info 4=debug

http_port=8080
https_port=8443
ssl_enabled=0             # 1 to enable HTTPS
ssl_cert=./ssl/cert.pem
ssl_key=./ssl/key.pem

trusted_proxies=          # comma-separated IPs, e.g. 127.0.0.1,10.0.0.1

theme=dark                # last-resort theme fallback (see below)
lang=Eng                  # content language fallback (see below)
public_url=               # absolute base for QR-code URLs, e.g. https://example.org

#force_epoch=3            # force a browser epoch (-1..3); omit to auto-detect

mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder    # one database per site; boat_rudder is only the default

session_ttl_seconds=86400 # session cookie lifetime (default 24h)

# Anti-DDoS (see security.md)
ddos_max_connections=200
ddos_rate_window_secs=5
ddos_rate_limit=500
ddos_max_ips=1024
ddos_cleanup_interval_secs=60
ddos_ip_stale_secs=300

connection_io_timeout_secs=5   # per read/write call - slow-loris defense

# WAP 1.x gateway (see wap-gateway.md)
wap_gateway_enabled=0
wap_gateway_ips=               # listen addresses, comma-separated; empty = all
wap_gateway_rover_port=49300   # Palm Neomar/Rover WTP framing; 0 disables
wap_gateway_wsp_port=9200      # connectionless WSP framing; 0 disables
wap_gateway_rate_limit=60      # requests/min per source IP; 0 = unlimited
```

---

## Keys

### Server and logging

| Key | Type | Default | Description |
|---|---|---|---|
| `verbose_level` | int | `3` | Log verbosity: `0`=none `1`=error `2`=warn `3`=info `4`=debug |
| `http_port` | int | `8080` | HTTP listening port. Ports < 1024 require root on Linux; `rundebug` re-executes itself with `sudo` when needed. The WAP gateway fetches pages from this port over loopback. |
| `https_port` | int | `8443` | HTTPS listening port. Only used when `ssl_enabled=1`. |
| `ssl_enabled` | int | `0` | `1` enables the HTTPS socket. Requires a valid `ssl_cert` and `ssl_key`. Also adds `; Secure` to the session cookie. |
| `ssl_cert` | string | `./ssl/cert.pem` | PEM certificate path, relative to the working directory. |
| `ssl_key` | string | `./ssl/key.pem` | PEM private key path, relative to the working directory. |
| `trusted_proxies` | string | *(empty)* | Comma-separated reverse-proxy IPs. `X-Real-IP` / `X-Forwarded-For` are honored **only** from these peers; every other connection uses its raw socket address. Empty means "trust no proxy headers" - the safe default when the server is exposed directly. Add `127.0.0.1` if the WAP gateway is enabled and you want analytics to see the phone's address instead of loopback (see [wap-gateway.md](wap-gateway.md)). |

### Site and rendering

| Key | Type | Default | Description |
|---|---|---|---|
| `theme` | string | `dark` | **Last-resort theme fallback**, not "the active theme". Per request the theme is resolved as `?theme=` → `theme` cookie → `site_settings.active_theme` (set from `/dashboard/settings/themes`) → this key (`src/utils/request_theme.h`). Each template path is then looked up first in `html/themes/<theme>/...` and, if the theme doesn't override it, in the shared `html/templates/...` (`generate_url_theme()`). See [themes.md](themes.md). |
| `lang` | string | `Eng` | Content language **fallback**. Used only when MongoDB is unavailable or no `languages` document has `is_default:true`; otherwise the content language comes from the `languages` collection (`/dashboard/languages`), then per visitor from `?lang=` / the `lang` cookie. |
| `public_url` | string | *(empty)* | Public base URL of the site, without trailing slash. Used to build the absolute URLs encoded in QR codes (`/gallery/<id>` for epochs -1/0, `/image-qr/<code>`), so a phone scanning the code reaches the public host rather than whatever `Host` the retro browser used. When empty, the URL is built from the request's own `Host` header. See [qr-and-short-links.md](qr-and-short-links.md). |
| `force_epoch` | int | *(unset = auto-detect)* | Forces the browser epoch (`-1`=WML, `0`=pre-standard, `1`=early, `2`=middle, `3`=modern) for **every** request, bypassing both `?preview_epoch=` and `detect_epoch()`. Any value outside `-1..3` keeps auto-detection. Useful for checking a retro layout from a modern browser - but note it also locks the dashboard out unless set to `3`, since the dashboard is epoch 3 only. |

### Database and sessions

| Key | Type | Default | Description |
|---|---|---|---|
| `mongodb_uri` | string | `mongodb://localhost:27017` | MongoDB connection string, opened once at startup into a `mongoc_client_pool_t`. |
| `mongodb_db` | string | `boat_rudder` | Database name for this site - one database per site. Also read by `scripts/mongodb_dump.sh` and `mongodb_restore.sh`, so backups follow whichever site a checkout is configured for. |
| `session_ttl_seconds` | int | `86400` | Session cookie and `sessions.expires_at` lifetime, in seconds. |

### Anti-DDoS and timeouts

Enforced in `src/web_server/server_listener.c` (accept loop) and
`src/web_server/connection_thread.c`. How they interact is described in
[security.md](security.md#connection-level-defenses).

| Key | Type | Default | Description |
|---|---|---|---|
| `ddos_max_connections` | int | `200` | Global cap on simultaneously open connections (HTTP + HTTPS). Connections over the cap are rejected at `accept()`: HTTP gets a best-effort `429`, HTTPS a plain close. |
| `ddos_rate_window_secs` | int | `5` | Per-IP rate window. Connections from one IP closer together than this count toward `ddos_rate_limit`; a longer gap resets the count. A blocked IP stays blocked for **2 ×** this window. |
| `ddos_rate_limit` | int | `500` | Connections per IP within the window before that IP is temporarily blocked. |
| `ddos_max_ips` | int | `1024` | Size of the per-IP tracking table (`calloc`'d once in `server_start()`). When full, the least-recently-seen entry is evicted (LRU). |
| `ddos_cleanup_interval_secs` | int | `60` | How often the cleanup thread scans the IP table. |
| `ddos_ip_stale_secs` | int | `300` | An IP with no connection for this long (and not blocked) is freed from the table by the cleanup thread; expired bans are lifted on the same pass. |
| `connection_io_timeout_secs` | int | `5` | `SO_RCVTIMEO`/`SO_SNDTIMEO` on every client socket - the slow-loris defense. Applies **per read/write call**, not to the whole transfer, so a slow-but-steady upload is unaffected. Also bounds how long `server_stop()` waits for in-flight TLS connections. |

### WAP gateway

The UDP gateway that serves epoch -1 as compiled WBXML to real WAP 1.x devices. Full description in
[wap-gateway.md](wap-gateway.md).

| Key | Type | Default | Description |
|---|---|---|---|
| `wap_gateway_enabled` | int | `0` | `1` starts the gateway thread after the HTTP listener is up. `0` makes `wap_gateway_start()` a no-op. |
| `wap_gateway_ips` | string | *(empty)* | Comma-separated local IPv4 addresses to bind. Empty means one socket per port on `0.0.0.0`. Listing addresses binds one socket per address and port, so on a multi-homed host replies leave from the address the client sent to. An address that isn't local is logged and skipped. |
| `wap_gateway_rover_port` | int | `49300` | UDP port for the Palm Neomar/Rover 1.5 WTP framing. `0` disables it. |
| `wap_gateway_wsp_port` | int | `9200` | UDP port for standard WAP 1.x connectionless WSP. `0` disables it. Experimental: untested on real phones. |
| `wap_gateway_rate_limit` | int | `60` | Maximum requests answered per minute per source IP; `0` = unlimited. Exists because UDP replies are ~20× the request size, so an unlimited gateway is a traffic amplifier for spoofed sources. |

---

## Notes

**MongoDB is optional at startup, not at runtime.** If `mongodb_manager_init()` fails, the server
keeps running and logs the failure: static files and the epoch-rendered shell still work.
`/login` and every `/dashboard*` route answer `503` in the visitor's own epoch; `/page/<link>`,
`/blog/<link>` and `/gallery/<id>` answer `404` (no entry can be found); listings such as `/blog`
render their empty state; analytics, short links and theme overrides are silently skipped.

**The file is read once, at startup.** `rundebug` starts the binary from the project root with
`-c ./configs/settings.conf`, so editing this file and restarting is enough - no copy step, no
recompile. The installed service reads its own copy at
`/usr/local/bin/boat-rudder/configs/settings.conf`, placed there by `install` **only on the first
install**: a reinstall keeps the host's file, writes the repository's version next to it as
`settings.conf.dist`, and lists any key the installed file lacks (see
[scripts.md](scripts.md#install)).

**Paths are relative to the working directory**, not to the config file. `generate_url_theme()`
always resolves `./html/themes/<theme>/...` and `./html/templates/...` from the process's working
directory, independently of the root directory passed as the last CLI argument (which is what the
static file server serves from).

**Not configurable (compile-time or fixed paths):**

| Value | Where | Notes |
|---|---|---|
| GeoLite2 database path | `GEOIP_DEFAULT_DB_PATH` in `src/modules/analytics/geoip.h` | Fixed to `./data/GeoLite2-Country.mmdb`, relative to the working directory. Missing file → every visit is counted as country `Unknown`. See [analytics.md](analytics.md). |
| Request header buffer | `RAW_REQUEST_SIZE` = 32 KiB (`http_constants.h`) | Larger header blocks get `431`. |
| Request body cap | `MAX_BODY_SIZE` = 10 MiB (`http_constants.h`) | Larger `Content-Length` gets `413`. |
| Theme banner/footer/CSS form field | `THEME_ASSET_HTML_MAX` = 128 KiB (`http_constants.h`) | Longer values are truncated. |
| WML page size | `WAP_PAGE_BYTES` = 860 compiled bytes (`wap_gateway/wbxml.h`) | Applies to both the gateway and WML served over HTTP. |

**The WAP gateway needs an external relay.** Vintage phones are hardwired to a long-dead operator
gateway IP; `trc-wap-relay` (a separate project), running on the PC the device connects through,
claims that IP and forwards the datagrams to `wap_gateway_ips`. Boat Rudder itself needs no
special network privileges for this.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
