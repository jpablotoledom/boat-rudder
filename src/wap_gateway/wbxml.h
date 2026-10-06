#ifndef WBXML_H
#define WBXML_H

#include <stddef.h>

// WML -> WBXML (compiled WML, application/vnd.wap.wmlc) for WAP 1.x clients
// behind the WAP gateway (wap_gateway.h). Ported from neomar-wap-proxy's
// compiler: WML 1.1 tag/attribute code page 0, every string inline (STR_I),
// UTF-8 charset; unknown tags are dropped but their content is kept.
//
// Paginated for single-packet replies: a WAP 1.x connectionless reply is one
// UDP datagram, and some clients (Palm Rover) won't reassemble anything
// bigger - so the deck's first card is split into pages of at most
// `page_bytes` compiled bytes, each a one-card deck ending in "<< Prev" /
// "Next >>" links that re-request `uri` with a "__page=N" query parameter.

typedef struct {
    unsigned char *data;
    size_t len;
} WbxmlBuf;

// Compiles page `page` (0-based, clamped) of `wml`. `wml` is the deck
// source; `latin1` says its bytes are ISO-8859-1 rather than UTF-8 (WML from
// this server's epoch -1 is Latin-1). `uri` is the absolute URI the client
// requested - its scheme://host prefixes every site-relative href/src so the
// client never has to resolve one itself, and its path builds the Prev/Next
// links. Returns 0 and fills *out (caller frees out->data), -1 on failure.
int wbxml_compile_page(const char *wml, size_t wml_len, int latin1,
                       const char *uri, int page, size_t page_bytes, WbxmlBuf *out);

// Largest page, in compiled (WBXML) bytes, for every epoch -1 client - the
// gateway's packets and the WML decks served over HTTP alike. A Palm Rover
// renders nothing past a single ~975-byte packet; one limit for everyone
// for now (a per-User-Agent table could replace it later).
#define WAP_PAGE_BYTES 860

// Same pagination, as WML text, for decks served straight over HTTP: page
// `page` of `wml` (UTF-8, or ISO-8859-1 with `latin1`; links left as they
// are; the result is always UTF-8), with "<< Prev"/"Next >>"
// built from `uri` (the request's own path?query) and `xml_encoding` named
// in the prolog (the encoding the caller will send it in). Returns 0 with
// *out = the new deck (caller frees), or with *out = NULL when it all fits a
// single page (keep the original); -1 on failure.
int wml_paginate(const char *wml, size_t wml_len, int latin1, const char *uri, int page,
                 size_t page_bytes, const char *xml_encoding, char **out);

// One-card deck with `title` and a single paragraph of `text` (UTF-8) - for
// error and "not available" replies. Same ownership as above.
int wbxml_message_card(const char *title, const char *text, WbxmlBuf *out);

// "__page=N" from `uri` (0 when absent), and `uri` with it set to `page`
// (removed for page 0) into out (out_size bytes).
int  wbxml_page_param(const char *uri);
void wbxml_set_page_param(const char *uri, int page, char *out, size_t out_size);

#endif
