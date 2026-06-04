#ifndef SERVER_LISTENER_H
#define SERVER_LISTENER_H

int  server_start(const char *root_dir, int ssl_enabled,
                  const char *ssl_cert, const char *ssl_key,
                  int http_port, int https_port);

void server_stop(void);

#endif // SERVER_LISTENER_H
