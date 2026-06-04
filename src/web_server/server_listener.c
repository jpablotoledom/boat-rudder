#include "server_listener.h"
#include "connection_thread.h"
#include "tls_context.h"
#include "../utils/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

#define THREAD_STACK_SIZE (2 * 1024 * 1024)

static int           server_fd_http  = -1;
static int           server_fd_https = -1;
static SSL_CTX      *ssl_ctx         = NULL;
static _Atomic int   running         = 1;

// --- Connection counter + shutdown cond ---
#define MAX_CONNECTIONS 200
static int             active_connections = 0;
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  conn_cond  = PTHREAD_COND_INITIALIZER;

int try_register_connection(void) {
    int ok = 0;
    pthread_mutex_lock(&conn_mutex);
    if (active_connections < MAX_CONNECTIONS) { active_connections++; ok = 1; }
    pthread_mutex_unlock(&conn_mutex);
    return ok;
}

void unregister_connection(void) {
    pthread_mutex_lock(&conn_mutex);
    if (active_connections > 0) active_connections--;
    // Signal server_stop() if all connections have drained.
    if (active_connections == 0) pthread_cond_signal(&conn_cond);
    pthread_mutex_unlock(&conn_mutex);
}

// --- Per-IP rate limiting with LRU eviction ---
#define MAX_IPS     1024
#define RATE_WINDOW 5     // seconds
#define RATE_LIMIT  500   // connections per RATE_WINDOW

typedef struct {
    in_addr_t ip;
    time_t    last_conn;
    time_t    last_rejected;
    int       count;
    int       blocked;
} ip_entry_t;

static ip_entry_t ip_table[MAX_IPS];

static int too_many_connections(struct in_addr client_ip) {
    time_t    now = time(NULL);
    in_addr_t ip  = client_ip.s_addr;

    for (int i = 0; i < MAX_IPS; i++) {
        ip_entry_t *e = &ip_table[i];
        if (e->ip != ip) continue;

        if (e->blocked) {
            if ((now - e->last_rejected) > RATE_WINDOW * 2) {
                e->blocked = 0;
                e->count   = 0;
            } else {
                return 1;
            }
        }

        if ((now - e->last_conn) < RATE_WINDOW) {
            e->count++;
            e->last_conn = now;
            if (e->count > RATE_LIMIT) {
                e->blocked      = 1;
                e->last_rejected = now;
                LOG_WARN("Blocking IP %s for %d s", inet_ntoa(client_ip), RATE_WINDOW * 2);
                return 1;
            }
            return 0;
        }

        e->count     = 1;
        e->last_conn = now;
        return 0;
    }

    // New IP - find an empty slot first
    for (int i = 0; i < MAX_IPS; i++) {
        if (ip_table[i].ip == 0) {
            ip_table[i] = (ip_entry_t){
                .ip = ip, .count = 1, .last_conn = now, .blocked = 0
            };
            return 0;
        }
    }

    // Table full - evict the entry with the oldest last_conn (LRU)
    int oldest_idx = 0;
    for (int i = 1; i < MAX_IPS; i++) {
        if (ip_table[i].last_conn < ip_table[oldest_idx].last_conn)
            oldest_idx = i;
    }
    ip_table[oldest_idx] = (ip_entry_t){
        .ip = ip, .count = 1, .last_conn = now, .blocked = 0
    };
    return 0;
}

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERROR("socket: %s", strerror(errno)); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind(port=%d): %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, 128) < 0) {
        LOG_ERROR("listen(port=%d): %s", port, strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

static void spawn_thread(int client_socket, SSL *ssl, const char *root_dir) {
    struct thread_args *args = malloc(sizeof(*args));
    if (!args) {
        LOG_ERROR("malloc thread_args failed");
        if (ssl) SSL_free(ssl);
        close(client_socket);
        unregister_connection();
        return;
    }
    args->client_socket  = client_socket;
    args->root_directory = root_dir;
    args->ssl            = ssl;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);

    pthread_t tid;
    int rc = pthread_create(&tid, &attr, connection_thread, args);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        LOG_ERROR("pthread_create: %s", strerror(rc));
        if (ssl) SSL_free(ssl);
        close(client_socket);
        free(args);
        unregister_connection();
    } else {
        pthread_detach(tid);
    }
}

int server_start(const char *root_dir, int ssl_enabled,
                 const char *ssl_cert, const char *ssl_key,
                 int http_port, int https_port) {
    signal(SIGPIPE, SIG_IGN);

    server_fd_http = make_listener(http_port);
    if (server_fd_http < 0) return -1;
    LOG_INFO("HTTP listening on port %d", http_port);

    if (ssl_enabled) {
        server_fd_https = make_listener(https_port);
        if (server_fd_https < 0) return -1;

        ssl_ctx = tls_create_context(ssl_cert, ssl_key);
        if (!ssl_ctx) return -1;
        LOG_INFO("HTTPS listening on port %d", https_port);
    }

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    fd_set readfds;
    int max_sd = server_fd_http;
    if (ssl_enabled && server_fd_https > max_sd) max_sd = server_fd_https;

    while (atomic_load(&running)) {
        FD_ZERO(&readfds);
        FD_SET(server_fd_http, &readfds);
        if (ssl_enabled) FD_SET(server_fd_https, &readfds);

        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            LOG_ERROR("select: %s", strerror(errno));
            continue;
        }

        if (FD_ISSET(server_fd_http, &readfds)) {
            int cs = accept(server_fd_http, (struct sockaddr *)&addr, &addrlen);
            if (cs >= 0) {
                if (too_many_connections(addr.sin_addr)) {
                    LOG_WARN("Rate limit: dropping %s", inet_ntoa(addr.sin_addr));
                    close(cs);
                } else if (!try_register_connection()) {
                    LOG_WARN("Connection cap reached, dropping");
                    close(cs);
                } else {
#ifdef SO_NOSIGPIPE
                    int v = 1; setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, &v, sizeof(v));
#endif
                    spawn_thread(cs, NULL, root_dir);
                }
            }
        }

        if (ssl_enabled && FD_ISSET(server_fd_https, &readfds)) {
            int cs = accept(server_fd_https, (struct sockaddr *)&addr, &addrlen);
            if (cs >= 0) {
                if (too_many_connections(addr.sin_addr)) {
                    LOG_WARN("Rate limit (TLS): dropping %s", inet_ntoa(addr.sin_addr));
                    close(cs);
                } else if (!try_register_connection()) {
                    LOG_WARN("Connection cap (TLS) reached, dropping");
                    close(cs);
                } else {
#ifdef SO_NOSIGPIPE
                    int v = 1; setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, &v, sizeof(v));
#endif
                    SSL *ssl = SSL_new(ssl_ctx);
                    if (!ssl) {
                        LOG_ERROR("SSL_new failed");
                        close(cs);
                        unregister_connection();
                    } else {
                        SSL_set_fd(ssl, cs);
                        spawn_thread(cs, ssl, root_dir);
                    }
                }
            }
        }
    }

    return 0;
}

void server_stop(void) {
    atomic_store(&running, 0);
    if (server_fd_http  >= 0) { close(server_fd_http);  server_fd_http  = -1; }
    if (server_fd_https >= 0) { close(server_fd_https); server_fd_https = -1; }

    if (ssl_ctx) {
        // Wait for active threads to finish using ssl_ctx (bounded by socket timeout = 5 s).
        // Prevents use-after-free when server_stop() races with in-flight TLS handshakes.
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;

        pthread_mutex_lock(&conn_mutex);
        while (active_connections > 0) {
            if (pthread_cond_timedwait(&conn_cond, &conn_mutex, &deadline) == ETIMEDOUT) {
                LOG_WARN("server_stop: timeout waiting for %d connection(s) to close",
                         active_connections);
                break;
            }
        }
        pthread_mutex_unlock(&conn_mutex);

        tls_free_context(ssl_ctx);
        ssl_ctx = NULL;
    }
}
