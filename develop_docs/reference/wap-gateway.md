# Boat Rudder - WAP Gateway

`src/wap_gateway/` is a minimal **WAP 1.x gateway** built into the server. It lets a real
vintage phone or PDA - whose browser is hardwired to a long-dead operator gateway - browse the
site as epoch −1 (WML), compiled to the binary **WBXML** format those devices actually expect.

The path of the requested URI is kept (an empty one is the home page) and its host goes along
as the fetch's `Host` header. By default every page comes from this same server, which serves
its own site whatever the host. With `wap_upstream` the pages come from another HTTP server
instead - a front that routes by `Host` (HAProxy), so **one gateway serves every site on a
machine**, each phone getting the site its URL names; `boat-rudder --wap-only` runs the gateway
alone for that ([below](#several-sites-wap_upstream-and---wap-only)).

Diagram: [diagrams/sequence-wap-gateway.puml](../diagrams/sequence-wap-gateway.puml)

---

## Why it exists

A WAP 1.x browser does not speak HTTP. It sends WSP (Wireless Session Protocol) requests over
UDP to a gateway, which fetches the page over HTTP, compiles WML to WBXML and replies in WSP. The
operators' gateways those devices were configured for have been gone for two decades, so without
one the epoch −1 rendering only reaches WML-capable *emulators* over HTTP. This module is that
missing gateway, reduced to what's needed to serve one site.

Ported from **neomar-wap-proxy** (framing, WBXML compiler) and tested end-to-end on a real Palm
running Neomar/Rover 1.5.

---

## Topology

```
 Phone / PDA ──(UDP to its hardwired gateway IP)──► PC running trc-wap-relay
                                                     │ claims that dead IP locally,
                                                     │ forwards datagrams unchanged
                                                     ▼
                                   boat-rudder WAP gateway (UDP 49300 / 9200)
                                                     │ GET <path>?preview_epoch=-1&wml_pages=all
                                                     ▼
                                   boat-rudder HTTP listener (127.0.0.1:http_port)
```

The device's dead gateway IP never reaches Boat Rudder: **trc-wap-relay** (a separate project)
runs on the machine the device connects through (serial/IR/PPP), claims that IP there and relays
the traffic to `wap_gateway_ips`. Boat Rudder therefore needs no network privileges or routing
tricks of its own.

---

## Lifecycle

| Step | Where | Notes |
|---|---|---|
| Start | `main.c` → `wap_gateway_start()` **after** `server_start()` | The gateway fetches every page from the HTTP listener, so that must be up first. A failure is logged and the server continues without the gateway. |
| Sockets | `open_for_ip()` | For each address in `wap_gateway_ips` (or `0.0.0.0` when empty): one UDP socket on `wap_gateway_rover_port` and one on `wap_gateway_wsp_port` (a port of `0` is skipped). At most 16 sockets. A bind failure is logged (with a hint when the address isn't local) and skipped; start fails only if no socket opened. |
| Thread | `gateway_loop()` | One thread, `poll()` over all sockets with a 500 ms timeout, one datagram in → one datagram out, handled synchronously. |
| Stop | `main.c` → `wap_gateway_stop()` **before** `server_stop()` | Clears the running flag, joins the thread (≤ 500 ms), closes the sockets. |

---

## Request handling

```
recvfrom() ─► handle_datagram()
                ├─ framing check (rover: WTP Invoke; wsp: TID byte)
                ├─ PDU type must be 0x40..0x44 (Get/Options/Head/Delete/Trace) or 0x60 (Post)
                ├─ read URI (uintvar length, ≤ 1000 bytes); a Post: content type, headers, body
                ├─ rate_allow(source IP)  ── over limit → silently dropped
                └─ handle_request()
                     ├─ __page=N read from the URI, then removed
                     ├─ local_path(): drop scheme://host and #fragment; uri_host(): keep the host
                     ├─ fetch_page(host, path + "preview_epoch=-1&wml_pages=all")  (≤ 3 redirects;
                     │    a Post goes as an HTTP POST, the redirects after it as GETs)
                     ├─ 200 text/vnd.wap.wml  → wbxml_compile_page(page N) → WSP Reply (wmlc)
                     ├─ 200 image/vnd.wap.wbmp → WSP Reply (wbmp) as-is
                     ├─ 404 → "Not found" card
                     ├─ unreachable → "The site is not responding" card
                     └─ anything else → "This content is not available on WAP" card
              ─► sendto() the reply, from the socket the request arrived on
```

Anything that isn't a well-formed GET-family request or Post in the socket's framing is ignored -
the client retries or times out exactly as with a lost packet.

### Forms (WSP Post)

What makes a form work on the phone is in the compiled deck (`wbxml.c`): `<go method="post">`
is the attribute token `0x1C` (`method="get"` `0x1B`) - WML 1.1 has no plain `method` token,
and dropped, the phone sends a GET with the fields in the URL; and WML variables are WBXML
references - `$(name)` `EXT_I_2` + the name (`:e` `EXT_I_0`, `:u` `EXT_I_1`), `$$` a plain `$` -
since left as text a phone posts the literal "(name)". A deck that fits `WAP_PAGE_BYTES` whole
goes out as one page, with no room kept for Prev/Next links: a form split across two fetches
would show one challenge and send another.

A WML `<go method="post">` arrives as a WSP **Post** PDU (`0x60`, WAP-230 §8.2.3.2): URI length,
headers length, the URI, the content type and headers, then the body. `wsp_post_headers()` reads
the content type (a well-known code - `0x12` is `application/x-www-form-urlencoded` - or a text,
plain or in a general form) and the **User-Agent** header (`0x29`); the other headers are
skipped. The body is forwarded as an HTTP `POST` with that content type and the phone's own
User-Agent (else `BoatRudder-WAP-Gateway`), so a comment records the phone it was written on;
the redirect after it is followed with GETs, and the page it leads to is compiled with *that*
page's URI (its Prev/Next and relative links are its own). The whole Post must fit one datagram
(≤ 4096 bytes): WTP segmentation isn't implemented. That is the reason for WAP's 300-character
comment field ([comments.md](comments.md)).

### Framings

| Port (default) | Framing | Request | Reply header |
|---|---|---|---|
| `49300` (`wap_gateway_rover_port`) | **Palm Neomar/Rover 1.5** - WTP over UDP | 4-byte WTP Invoke header (PDU type 1 in bits 6-3 of byte 0), then the WSP PDU | The Invoke's 4 bytes mirrored with only the PDU type switched to Result (2) - a Rover quirk (it rejects anything else), documented in neomar-wap-proxy's `docs/PROTOCOL.md §2.2` |
| `9200` (`wap_gateway_wsp_port`) | **Standard connectionless WSP** (WAP 1.x) | 1-byte TID, then the WSP PDU | The TID echoed |

The WSP Reply is always: `0x04` (Reply), `0x20` (200 OK), headers length `1`, Content-Type as a
short integer - `0x94` for `application/vnd.wap.wmlc`, `0xA1` for `image/vnd.wap.wbmp` - then
the body.

### Page fetch

`fetch_page()` opens a plain TCP connection to `fetch_target()` - `wap_upstream` when set, else
this server's own listener (`listen_address` when bound to one, else `127.0.0.1`, on
`http_port`) - and sends:

```
GET <path>?preview_epoch=-1&wml_pages=all HTTP/1.0
Host: <the URI's host, or 127.0.0.1 when it has none>
User-Agent: BoatRudder-WAP-Gateway
Accept: text/vnd.wap.wml, image/vnd.wap.wbmp
X-Forwarded-For: <phone's source IP>
```

- `preview_epoch=-1` forces the WML rendering regardless of User-Agent, so the phone gets exactly
  what the epoch −1 templates produce over HTTP.
- `wml_pages=all` asks for the whole deck unpaginated - the gateway paginates itself (below).
- `X-Forwarded-For` is honored **only if `127.0.0.1` is in `trusted_proxies`**. Add it if you
  want analytics and GeoIP to see the phone's address rather than loopback.
- With `proxy_protocol=1` (a site behind a front) and the listener's address in
  `trusted_proxies`, the request is preceded by a PROXY v1 header -
  `PROXY TCP4 <phone> <listener> 0 <http_port>` - so the phone's address also reaches the per-IP
  rate limiter. Without that trust the header would get the connection closed, so the gateway
  sends none and the request is served as a direct loopback connection.
- The host is `uri_host()` of the requested URI: lowercased, and only a plain DNS name or IPv4
  address with an optional port - anything else (user info is dropped first) sends
  `127.0.0.1`, so nothing but a host name can reach the header.
- Limits: 10 s socket timeout, 1 MiB response, 3 redirect hops (an absolute `Location` sets the
  host for the next hop; its path is kept the same way).

Because the fetch is an ordinary HTTP request, it goes through the HTTP listener's own defenses
(connection cap, per-IP rate limit) and is recorded by analytics like any other visit, with
`epochwml` as its epoch.

### Several sites: `wap_upstream` and `--wap-only`

A phone's gateway setting is one IP and port, and `trc-wap-relay` forwards to one address, so
on a machine with several sites one gateway has to serve them all. Every WAP request carries the
full URL - `http://thejoyrobot.com/blog` - so the host it names is enough to route it:

```
phone ─UDP─► boat-rudder --wap-only ─HTTP, Host: thejoyrobot.com, PROXY v1 <phone>─►
             wap_upstream (HAProxy, accept-proxy) ─by Host, send-proxy-v2─► that site
```

| Key | |
|---|---|
| `wap_upstream` | `<IPv4>:<port>` of the server pages are fetched from. Empty: this server |
| `wap_upstream_proxy_protocol` | `1`: start each fetch with a PROXY v1 header carrying the phone's address (HAProxy's `accept-proxy` requires one, then passes the address on to the site) |

`boat-rudder [-c <config>] --wap-only` starts the gateway alone - no HTTP listener, MongoDB or
GeoIP - and requires `wap_gateway_enabled=1` and `wap_upstream`. The gateway only ever fetches
from `wap_upstream`, never from the host a phone names, so it can't be used as an open proxy: a
host the front doesn't know gets the front's default site. The multi-br project runs it this way,
as one service shared by every site on the machine.

---

## WBXML compilation and pagination

`src/wap_gateway/wbxml.c` parses the WML deck into a small tree and compiles it to WBXML:
WML 1.1 tag/attribute code page 0, every string inline (`STR_I`), UTF-8 charset. Unknown tags are
dropped but their text is kept. Every site-relative `href`/`src` is prefixed with the requested
URI's `scheme://host`, so the client never has to resolve a relative link itself.

**Single-packet pages.** A connectionless WAP reply is one UDP datagram, and some clients (Palm
Rover) won't reassemble anything bigger - Rover renders nothing past a single ~975-byte packet.
So the deck's first card is split into pages of at most **`WAP_PAGE_BYTES` = 860 compiled
bytes**, each a one-card deck ending in `<< Prev` / `Next >>` links that re-request the same URI
with `__page=N`. Text blocks are split at word boundaries to fit.

The same limit and the same splitter (`wml_paginate()`) also apply to WML served **directly over
HTTP** (emulators, WAP 2.0 browsers): `build_epoch_response.c` paginates every epoch −1 response
unless `wml_pages=all` was requested - see
[rendering.md](rendering.md#wml-pagination). One limit for every client for now; a per-device
table could replace it later.

Error and "not available" replies are one-card decks built by `wbxml_message_card()`.

---

## Abuse protection

UDP has no handshake: a request with a **spoofed** source address gets its reply sent to that
address, and replies are up to ~20× the request size. An open, unlimited gateway is therefore a
free traffic amplifier.

`rate_allow()` answers at most **`wap_gateway_rate_limit` requests per minute per source IP**
(default 60; `0` disables the limit). State lives in a fixed 256-entry table touched only by the
gateway thread (no locking); when full, the least recently started window gives up its slot.
The first dropped request of each window is logged; the rest are dropped silently. A real phone
makes a handful of requests a minute and never notices.

Recommendations:

- Bind only the address(es) the relay forwards to (`wap_gateway_ips`), not every interface.
- Keep the rate limit on.
- Firewall UDP 49300/9200 from the public internet unless you intend to accept relays from
  elsewhere.

---

## Configuration

All keys are documented in [configuration.md](configuration.md#wap-gateway):
`wap_gateway_enabled`, `wap_gateway_ips`, `wap_gateway_rover_port`, `wap_gateway_wsp_port`,
`wap_gateway_rate_limit`. The gateway also depends on `http_port`, `listen_address`,
`trusted_proxies` and `proxy_protocol` - or, with `wap_upstream`, on that server alone.

---

## Limitations

| Limitation | Notes |
|---|---|
| Connection-oriented WSP (UDP 9201) | Not supported. Phones that insist on it can't use this gateway. |
| Connectionless WSP (9200) | Implemented per the spec, **untested on real phones**. |
| WTP segmentation/reassembly (SAR) | Not implemented - hence the single-packet pages. |
| WTLS (9202/9203) | Not supported. |
| POST | A WSP Post is forwarded as an HTTP POST (blog comments, their confirmation), only within one datagram. Untested on a real Palm Rover. The dashboard and login are epoch 3 only anyway. |
| IPv6 | Sockets are IPv4 only. |
| Per-device page size | One `WAP_PAGE_BYTES` for every client. |

---

## Source map

| File | Role |
|---|---|
| `wap_gateway.h/.c` | Sockets, thread, framings, rate limit, loopback fetch, WSP replies |
| `wbxml.h/.c` | WML parser, WBXML compiler, pagination (`wbxml_compile_page()`, `wml_paginate()`), `__page` helpers, message cards |
| `utils/request_wml.h/.c` | Per-request `wml_pages` / raw target, read by the HTTP response layer |
| `utils/build_epoch_response.c` | HTTP-side WML pagination with the same limit |

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
