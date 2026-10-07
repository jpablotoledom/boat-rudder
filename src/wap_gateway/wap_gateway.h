#ifndef WAP_GATEWAY_H
#define WAP_GATEWAY_H

// A WAP 1.x gateway that answers every request with this site, rendered as
// epoch -1 (WML) and compiled to WBXML (see wbxml.h) - so a vintage phone
// or PDA whose browser is hardwired to a long-dead operator gateway browses
// this site instead, whatever host it asks for (the path is kept; an empty
// one is the home page). Off unless configs/settings.conf says otherwise:
//
//   wap_gateway_enabled=1
//   wap_gateway_ips=                             # listen addresses; empty = all
//   wap_gateway_rover_port=49300                 # 0 disables
//   wap_gateway_wsp_port=9200                    # 0 disables
//
// Two framings, one per port:
//   - rover (49300): WTP over UDP as the Palm Neomar/Rover 1.5 browser speaks
//     it - ported from neomar-wap-proxy, including its quirk of a Result
//     header that mirrors the Invoke's 4 bytes. Tested there on a real Palm.
//   - wsp (9200): standard WAP 1.x connectionless WSP (a one-byte TID, then
//     the WSP PDU, no WTP). Per the spec, untested on real phones; phones
//     that insist on connection-oriented WSP (9201) aren't supported.
//
// Pages are fetched from this same server over loopback HTTP with
// ?preview_epoch=-1, so they're exactly what the WML epoch renders.
//
// The client's dead operator gateway IP never reaches this server: the
// device connects through a PC running trc-wap-relay, which claims that IP
// locally and relays the datagrams here (see that project's docs). This
// process needs no network privileges of its own.

// Opens the configured sockets and starts the gateway thread. A no-op
// returning 0 when disabled. Sockets that fail to bind are logged and
// skipped; returns -1 only if none could be opened at all.
int wap_gateway_start(void);

// Stops the thread and closes the sockets (safe to call when not started).
void wap_gateway_stop(void);

#endif
