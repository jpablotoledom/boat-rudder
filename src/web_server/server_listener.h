#ifndef SERVER_LISTENER_H
#define SERVER_LISTENER_H

int  server_start(const char *root_dir, int ssl_enabled,
                  const char *ssl_cert, const char *ssl_key,
                  int http_port, int https_port);

void server_stop(void);

// The per-IP rate limit for an address learned after accept() - from a
// PROXY protocol header (connection_thread.c). Counts this connection and
// returns 1 when the address is (now) blocked. Unparseable text is never
// limited.
int  server_ip_rate_limited(const char *ip_text);

// Writes the best-effort plain-HTTP 429 the accept loop sends a rejected
// connection. Doesn't close the socket.
void server_send_429(int cs);

#endif // SERVER_LISTENER_H
