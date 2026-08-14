# Boat Rudder - Configuration

`configs/settings.conf` is the only configuration file. It is an INI-style `key=value` list
parsed by `src/utils/config_loader.c`; blank lines and lines starting with `#` are ignored, and
an absent key keeps its built-in default. The path can be overridden on the command line:

```
boat-rudder -c /path/to/settings.conf /path/to/html-root
```

**This document is the single reference for every key.** The other documents link here instead
of repeating the list, so there is exactly one place to update when a key is added.

> **Defaults vs. site values.** The defaults below are Boat Rudder's, not any particular site's.
> A site running on Boat Rudder overrides whatever it needs - its ports, its theme, its own
> MongoDB database - and those values live in that deployment's `settings.conf`, never in the
> source tree. See the naming note in [architecture.md](architecture.md).

---

## Full example

```ini
verbose_level=3           # 0=none 1=error 2=warn 3=info 4=debug

http_port=8080
https_port=8443
ssl_enabled=0             # 1 to enable HTTPS
ssl_cert=./ssl/cert.pem
ssl_key=./ssl/key.pem

trusted_proxies=          # comma-separated IPs, e.g. 127.0.0.1,10.0.0.1

theme=dark                # active theme under html/themes/<theme>/
lang=Eng                  # content language fallback (see below)
public_url=               # public base URL (reserved for SEO/canonical links)

#force_epoch=3            # force a browser epoch (-1..3); omit to auto-detect

mongodb_uri=mongodb://localhost:27017
mongodb_db=boat_rudder    # one database per site; boat_rudder is only the default

session_ttl_seconds=86400 # session cookie lifetime (default 24h)
```

---

## Keys

| Key | Type | Default | Description |
|---|---|---|---|
| `verbose_level` | int | `3` | Log verbosity: `0`=none `1`=error `2`=warn `3`=info `4`=debug |
| `http_port` | int | `8080` | HTTP listening port. Ports < 1024 require root on Linux; `rundebug` re-executes itself with `sudo` when needed. |
| `https_port` | int | `8443` | HTTPS listening port. Only used when `ssl_enabled=1`. |
| `ssl_enabled` | int | `0` | `1` enables the HTTPS socket. Requires a valid `ssl_cert` and `ssl_key`. |
| `ssl_cert` | string | `./ssl/cert.pem` | PEM certificate path, relative to the working directory. |
| `ssl_key` | string | `./ssl/key.pem` | PEM private key path, relative to the working directory. |
| `trusted_proxies` | string | *(empty)* | Comma-separated reverse-proxy IPs. `X-Real-IP` / `X-Forwarded-For` are honored **only** from these peers; every other connection uses its raw socket address. Empty means "trust no proxy headers" - the safe default when the server is exposed directly. |
| `theme` | string | `dark` | Active theme. Every template resolves as `./html/themes/<theme>/...` via `generate_url_theme()`. |
| `lang` | string | `Eng` | Content language **fallback**. Used only when MongoDB is unavailable or no `languages` document has `is_default:true`; otherwise the content language comes from the `languages` collection (`/dashboard/languages`). |
| `public_url` | string | *(empty)* | Public base URL of the site. Reserved for SEO/canonical links - not yet consumed by any renderer. |
| `force_epoch` | int | *(unset = auto-detect)* | Forces the browser epoch (`-1`=WML, `0`=pre-standard, `1`=early, `2`=middle, `3`=modern), bypassing `detect_epoch()`. Any value outside `-1..3` keeps auto-detection. Useful for checking a retro layout from a modern browser. |
| `mongodb_uri` | string | `mongodb://localhost:27017` | MongoDB connection string, opened once at startup into a `mongoc_client_pool_t`. |
| `mongodb_db` | string | `boat_rudder` | Database name for this site - one database per site. Also read by `scripts/mongodb_dump.sh` and `mongodb_restore.sh`, so backups follow whichever site a checkout is configured for. |
| `session_ttl_seconds` | int | `86400` | Session cookie and `sessions.expires_at` lifetime, in seconds. |

---

## Notes

**MongoDB is optional at startup, not at runtime.** If `mongodb_manager_init()` fails, the server
keeps running and logs the failure: static files and the epoch-rendered shell still work, but
every database-backed page - the dashboard, `/blog`, `/page/<link>`, `/gallery/<id>` - degrades
to a `503` error page rendered in the visitor's own epoch.

**The file is read live.** `rundebug` starts the binary from the project root with
`-c ./configs/settings.conf`, so editing this file and restarting is enough - no copy step, no
recompile. The installed service reads its own copy at
`/usr/local/bin/boat-rudder/configs/settings.conf`, placed there by `install`.

**Paths are relative to the working directory**, not to the config file. `generate_url_theme()`
always resolves `./html/themes/<theme>/...` from the process's working directory, independently
of the root directory passed as the last CLI argument (which is what the static file server
serves from).

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
