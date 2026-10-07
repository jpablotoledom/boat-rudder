# Boat Rudder - Third-Party and Ported Code

What in this repository was not written for it from scratch: vendored source, data files from
other vendors, external libraries it links against, and code ported from sibling projects. Keep
this list current whenever something is vendored, ported or added as a dependency.

Boat Rudder's own license is in [`LICENSE`](../../LICENSE) (all rights reserved; public for
viewing and evaluation). Third-party components keep their own licenses.

---

## Vendored source

| Component | Location | Version | License | Used for |
|---|---|---|---|---|
| **stb_image** (Sean Barrett) | `src/third_party/stb_image.h` | v2.30 | Public domain (Unlicense) **or** MIT, at the user's choice | Decoding PNG/JPEG uploads for the WBMP logo converter. Compiled once, in `src/utils/image_convert/image_convert.c`, with `STB_IMAGE_IMPLEMENTATION`, `STBI_ONLY_PNG`, `STBI_ONLY_JPEG` |
| **libqrencode declarations** | `src/utils/qr_generator/qrencode_minimal.h` | - | Own code (declarations of libqrencode's public API) | libqrencode ships no pkg-config file and the `-dev` package may be absent; this header declares the two symbols used (`QRcode_encodeString`, `QRcode_free`) so only the runtime library is needed |

### Rules for vendored code

- Keep it **unmodified** under `src/third_party/` so it can be upgraded by replacing the file;
  configure it with macros at the single inclusion point, as `image_convert.c` does.
- Record the version and license here.
- It is exempt from [style-guide.md](style-guide.md) formatting rules, but not from review: only
  the subset actually needed should be compiled in (`STBI_ONLY_*`).

---

## Data files

### GeoLite2

| | |
|---|---|
| File | `data/GeoLite2-Country.mmdb` |
| Vendor | MaxMind, Inc. |
| License | GeoLite2 End User License Agreement (requires attribution, restricts redistribution and requires keeping the database up to date / deleting outdated copies) |
| Used by | `src/modules/analytics/geoip.c` (optional, see [analytics.md](analytics.md#geoip-optional)) |
| Install | `scripts/install.sh` copies it to the install directory if present |

**The file is currently tracked in git.** Publishing it in a public repository is redistribution
under MaxMind's terms. The recommended setup is to keep `data/*.mmdb` out of version control and
have each deployment download its own copy with a MaxMind account (for example with
`geoipupdate`), placing it at `./data/GeoLite2-Country.mmdb`. Attribution, if the site displays
GeoLite2-derived data publicly: *"This product includes GeoLite2 data created by MaxMind,
available from https://www.maxmind.com."*

### Fonts

`html/assets/fonts/Milonga-Regular.ttf` - the default epoch 3 logo font (Google Fonts, SIL Open
Font License 1.1). Fonts uploaded through the dashboard are the site operator's responsibility
([fonts.md](fonts.md)).

---

## Linked libraries

| Library | Required | Purpose | Build detection |
|---|---|---|---|
| OpenSSL (`libssl`, `libcrypto`) | yes | TLS | `find_package(OpenSSL)` |
| POSIX threads | yes | Connection, accept, cleanup, gateway threads | `find_package(Threads)` |
| libmongoc 1.x (+ libbson) | yes | MongoDB | `pkg-config libmongoc-1.0` |
| libsodium | yes | Argon2id, CSPRNG | `pkg-config libsodium` |
| libqrencode | yes | QR matrices | `find_library(qrencode)` (no pkg-config file) |
| libm | yes | Math (image scaling) | linked explicitly |
| libmaxminddb | **optional** | GeoIP country lookup; defines `HAVE_MAXMINDDB` | `pkg-config libmaxminddb` |

Known driver issue: libmongoc 1.30.4-1+deb13u3 breaks `mongoc_collection_aggregate()` - see
[data-model.md](data-model.md#known-driver-issue).

---

## Code ported from other projects

| Origin | What | Where now | Adaptations |
|---|---|---|---|
| **neomar-wap-proxy** | WTP/WSP framing for Palm Neomar/Rover (including the Result header that mirrors the Invoke's 4 bytes) and the WML → WBXML compiler | `src/wap_gateway/wap_gateway.c`, `src/wap_gateway/wbxml.c` | Embedded as a thread; fetches pages over loopback HTTP from this server; single-packet pagination shared with the HTTP WML path; per-IP rate limit |
| **trc-wap-relay** | Not ported - an external companion program that claims the device's dead gateway IP and relays UDP to this server | separate project | See [wap-gateway.md](wap-gateway.md#topology) |
| **the-retro-center-old** (TRC) | Analytics day-bucket model and tracker | `src/modules/analytics/analytics.c` | Route classification for Boat Rudder's URLs, `mongodb_manager`, GeoIP split into `geoip.c` |
| **the-retro-center-old** (TRC) | QR generator | `src/utils/qr_generator/` | GIF/WBMP writers rewritten in pure C (the original shelled out to ImageMagick), per-call LZW tables and atomic writes for thread safety, text renderers for epoch 0 |
| **the-retro-center-old** (TRC) | Analytics data | via `scripts/migrations/2026-10-03-merge-analytics.js` | Counters summed into the live collections |
| **base-http-server** | Static file server, listener, TLS, router skeleton | `src/web_server/` | Grew into the CMS router; anti-DDoS made configurable, cleanup thread and mutex added |

When porting more code, note the origin in the file's header comment and add a row here.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
