# ADR: WAP gateway and analytics (retroactive)

> **Status**: accepted and implemented (commits `b4f3347`, `13dc9dc`, `4fe591c`, `7c55aef`,
> 2026-09/10). Both features were ported from sibling projects without a plan of their own; this
> record captures the decisions after the fact so they can be revisited deliberately.
> References: [wap-gateway.md](../reference/wap-gateway.md),
> [analytics.md](../reference/analytics.md).

---

## 1. WAP gateway

### Context

Epoch −1 rendered WML over HTTP, which only emulators and WAP 2.0 browsers can fetch. Real WAP
1.x devices speak WSP/WTP over UDP to an operator gateway that no longer exists.
`neomar-wap-proxy` already implemented that gateway as a standalone proxy, tested on a real Palm.

### Decisions

| # | Decision | Alternatives considered | Why |
|---|---|---|---|
| 1 | **Embed the gateway in the server** (own thread, off by default) | Keep a separate proxy process in front of the site | One deployment, one config file; the gateway only ever serves this site, so it doesn't need to be a general proxy |
| 2 | **Fetch pages over loopback HTTP** (`?preview_epoch=-1&wml_pages=all`) | Call the renderers directly from the gateway thread | Rendering depends on per-request thread-local state set by the router; going through HTTP reuses the router, the anti-DDoS limits, analytics and every route unchanged |
| 3 | **Ignore the requested host**, keep the path | Act as a real proxy for any site | Devices are hardwired to dead hosts; the point is to reach *this* site. Also removes any open-proxy risk |
| 4 | **Single-packet pages of 860 compiled bytes**, same limit over HTTP | Implement WTP segmentation/reassembly (SAR) | Palm Rover renders nothing past one ~975-byte packet; SAR is complex and still unsupported by some clients. Sharing the limit with HTTP makes pages identical on both paths |
| 5 | **Two framings** (Rover WTP on 49300, connectionless WSP on 9200); no connection-oriented WSP (9201) | Full WAP stack | Rover is the tested device; connectionless WSP is cheap to add; 9201 needs session state and WTP class 2 |
| 6 | **External relay (`trc-wap-relay`)** claims the device's dead gateway IP | Bind that IP on the server, NAT rules | Keeps the server unprivileged and its network config untouched; the relay runs where the device connects |
| 7 | **Per-source-IP rate limit** (60/min default) | None; rely on firewalling | UDP replies are ~20× the request: an open gateway is an amplifier for spoofed sources |

### Consequences

- One slow loopback fetch delays every phone (single gateway thread).
- Analytics count each phone request; the phone's IP is only visible if `127.0.0.1` is a
  trusted proxy.
- Pagination moved out of `entry_page()` into the response layer for *all* WML, replacing the old
  per-entry `?page=` scheme.

---

## 2. Analytics

### Context

The site needed visit statistics that also show what the project is about - which *epochs*
visit - without third-party trackers (which most retro browsers couldn't run anyway) and without
storing personal data. `the-retro-center-old` already had a day-bucket tracker with the same
shape, and its historical data was worth keeping.

### Decisions

| # | Decision | Alternatives considered | Why |
|---|---|---|---|
| 1 | **Server-side counting** in the router, before dispatch | Client-side script, log parsing | Works for every epoch (no JS needed), no external service, no offline batch job |
| 2 | **Day buckets** (`$inc` on one document per day, plus one per entry per day) | Raw event log + aggregation | Constant size per day, nothing to prune, cheap reports; same shape as TRC so its data merges in |
| 3 | **No IP stored**; IP used in memory for the country only | Store IP / hashed IP | Privacy by design; unique visitors are deliberately not measured |
| 4 | **GeoLite2 optional** (`HAVE_MAXMINDDB`, fixed `./data/` path) | Mandatory dependency, online geo API | Builds and runs without it ("Unknown"); no network calls per visit |
| 5 | **Heuristic UA parser** (substring chain) | UA database library | Zero dependencies; good enough for families + major versions, including vintage browsers a generic library wouldn't know |
| 6 | **Synchronous writes** on the request thread | Background writer/queue | Simplest; acceptable at this site's traffic. Revisit if MongoDB latency shows up in page times |
| 7 | **Merge migration that sums counters** | `mongorestore --drop` | Keeps visits already recorded by the running server on overlapping days |

### Consequences

- Every counted page view costs two MongoDB round-trips.
- Preview-iframe and gateway requests are counted (under their resolved epoch).
- `data/GeoLite2-Country.mmdb` ended up tracked in git, which conflicts with MaxMind's license -
  see [third-party.md](../reference/third-party.md#geolite2).

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
