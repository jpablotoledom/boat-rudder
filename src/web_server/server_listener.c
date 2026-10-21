#include "server_listener.h"
#include "connection_thread.h"
#include "tls_context.h"
#include "../utils/config_loader.h"
#include "../utils/http_utils.h"
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
static int             active_connections = 0;
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  conn_cond  = PTHREAD_COND_INITIALIZER;

int try_register_connection(void) {
    int ok = 0;
    pthread_mutex_lock(&conn_mutex);
    if (active_connections < ddos_max_connections) { active_connections++; ok = 1; }
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
// Keyed by the 16-byte IPv6 form of the address (IPv4 as ::ffff:a.b.c.d):
// the socket's peer is always IPv4, but a PROXY protocol header (see
// connection_thread.c) can carry an IPv6 client.
typedef struct {
    unsigned char ip[16];
    int       used;
    time_t    last_conn;
    time_t    last_rejected;
    int       count;
    int       blocked;
} ip_entry_t;

// malloc'd (calloc'd) in server_start() from ddos_max_ips, freed in
// server_stop(). Touched by the accept loop, connection threads (after a
// PROXY header - server_ip_rate_limited()) and ip_table_cleanup_thread()
// below, so every access goes through ip_table_mutex.
static ip_entry_t     *ip_table = NULL;
static pthread_mutex_t ip_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ipv4_key(struct in_addr addr, unsigned char key[16]) {
    memset(key, 0, 10);
    key[10] = 0xFF;
    key[11] = 0xFF;
    memcpy(key + 12, &addr.s_addr, 4);
}

static int too_many_connections(const unsigned char key[16], const char *ip_text) {
    time_t now = time(NULL);

    pthread_mutex_lock(&ip_table_mutex);

    for (int i = 0; i < ddos_max_ips; i++) {
        ip_entry_t *e = &ip_table[i];
        if (!e->used || memcmp(e->ip, key, 16) != 0) continue;

        if (e->blocked) {
            if ((now - e->last_rejected) > ddos_rate_window_secs * 2) {
                e->blocked = 0;
                e->count   = 0;
            } else {
                pthread_mutex_unlock(&ip_table_mutex);
                return 1;
            }
        }

        if ((now - e->last_conn) < ddos_rate_window_secs) {
            e->count++;
            e->last_conn = now;
            if (e->count > ddos_rate_limit) {
                e->blocked      = 1;
                e->last_rejected = now;
                LOG_WARN("Blocking IP %s for %d s", ip_text, ddos_rate_window_secs * 2);
                pthread_mutex_unlock(&ip_table_mutex);
                return 1;
            }
            pthread_mutex_unlock(&ip_table_mutex);
            return 0;
        }

        e->count     = 1;
        e->last_conn = now;
        pthread_mutex_unlock(&ip_table_mutex);
        return 0;
    }

    ip_entry_t fresh = { .used = 1, .count = 1, .last_conn = now };
    memcpy(fresh.ip, key, 16);

    // New IP - find an empty slot first
    for (int i = 0; i < ddos_max_ips; i++) {
        if (!ip_table[i].used) {
            ip_table[i] = fresh;
            pthread_mutex_unlock(&ip_table_mutex);
            return 0;
        }
    }

    // Table full - evict the entry with the oldest last_conn (LRU). In
    // steady operation the cleanup thread (ip_table_cleanup_thread()) keeps
    // the table from actually reaching this point by freeing stale entries
    // on its own schedule; this LRU fallback only matters between cleanup
    // passes or if ddos_max_ips is set too low for real traffic.
    int oldest_idx = 0;
    for (int i = 1; i < ddos_max_ips; i++) {
        if (ip_table[i].last_conn < ip_table[oldest_idx].last_conn)
            oldest_idx = i;
    }
    ip_table[oldest_idx] = fresh;
    pthread_mutex_unlock(&ip_table_mutex);
    return 0;
}

int server_ip_rate_limited(const char *ip_text) {
    unsigned char key[16];
    struct in_addr v4;
    if (inet_pton(AF_INET, ip_text, &v4) == 1) ipv4_key(v4, key);
    else if (inet_pton(AF_INET6, ip_text, key) != 1) return 0;
    return too_many_connections(key, ip_text);
}

// Whether the accept loop leaves the per-IP check to the connection thread:
// with proxy_protocol=1 a trusted peer is the front, whose own address
// stands for every visitor - counted at accept(), the first busy minute
// would ban all of them at once. The check runs instead against the address
// in the PROXY header (connection_thread.c).
static int rate_limit_after_header(struct in_addr peer) {
    if (!proxy_protocol) return 0;
    char text[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &peer, text, sizeof(text)) && is_trusted_proxy(text);
}

static int rate_limited_at_accept(struct in_addr peer) {
    if (rate_limit_after_header(peer)) return 0;
    unsigned char key[16];
    char text[INET_ADDRSTRLEN] = "?";
    ipv4_key(peer, key);
    inet_ntop(AF_INET, &peer, text, sizeof(text));
    return too_many_connections(key, text);
}

// Periodically frees ip_table entries that have gone quiet
// (ddos_ip_stale_secs with no new connection) and proactively lifts expired
// bans, so a long-running process doesn't permanently fill its ddos_max_ips
// slots with IPs that stopped connecting long ago (the original port of
// this table never did this - entries lived for the life of the process).
static _Atomic int     cleanup_running = 1;
static pthread_mutex_t cleanup_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cleanup_wait_cond  = PTHREAD_COND_INITIALIZER;

static void *ip_table_cleanup_thread(void *arg) {
    (void)arg;

    while (atomic_load(&cleanup_running)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += ddos_cleanup_interval_secs;

        pthread_mutex_lock(&cleanup_wait_mutex);
        pthread_cond_timedwait(&cleanup_wait_cond, &cleanup_wait_mutex, &deadline);
        pthread_mutex_unlock(&cleanup_wait_mutex);

        if (!atomic_load(&cleanup_running)) break;

        time_t now = time(NULL);
        int    freed = 0;

        pthread_mutex_lock(&ip_table_mutex);
        for (int i = 0; i < ddos_max_ips; i++) {
            ip_entry_t *e = &ip_table[i];
            if (!e->used) continue;

            if (e->blocked && (now - e->last_rejected) > ddos_rate_window_secs * 2)
                e->blocked = 0;

            if (!e->blocked && (now - e->last_conn) > ddos_ip_stale_secs) {
                *e = (ip_entry_t){0};
                freed++;
            }
        }
        pthread_mutex_unlock(&ip_table_mutex);

        if (freed > 0) LOG_INFO("ip_table cleanup: freed %d stale entr%s", freed, freed == 1 ? "y" : "ies");
    }

    return NULL;
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
    if (listen_address[0] && inet_pton(AF_INET, listen_address, &addr.sin_addr) != 1) {
        LOG_ERROR("listen_address '%s' is not an IPv4 address", listen_address);
        close(fd); return -1;
    }

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

// Sent, best-effort, before closing a plain-HTTP connection rejected by
// too_many_connections()/try_register_connection() - a real 429 behaves
// better with clients/CDNs than an opaque connection reset. Not sent on the
// HTTPS listener: rejecting at accept() is cheap specifically because it
// skips the TLS handshake, and completing that handshake just to deliver a
// 429 would hand the very CPU cost this defense exists to avoid back to
// whoever (or whatever) is triggering the rejection - so HTTPS keeps the
// plain socket close it already had.
static const char RESPONSE_429[] =
    "HTTP/1.1 429 Too Many Requests\r\n"
    "Content-Type: text/plain\r\n"
    "Retry-After: 10\r\n"
    "Content-Length: 19\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Too Many Requests\n";

void server_send_429(int cs) {
    // Best-effort: SIGPIPE is already ignored (signal() call in
    // server_start()), so a client that has already hung up just makes this
    // write() fail (EPIPE) rather than crash - ignored either way.
    ssize_t ignored = write(cs, RESPONSE_429, sizeof(RESPONSE_429) - 1);
    (void)ignored;
}

static void reject_http_connection(int cs) {
    server_send_429(cs);
    close(cs);
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

static pthread_t cleanup_thread_id;
static int       cleanup_thread_started = 0;

// The accept loop itself, run on its own thread (see accept_thread_id
// below) so server_start() can return once the listeners are up instead of
// blocking forever - main.c's shutdown wait loop and server_stop() call are
// otherwise unreachable on a real SIGTERM/SIGINT (main.c's handle_shutdown
// only ever touches its own local `running` flag; this module's `running`,
// the one this loop actually checks, was previously never set to 0 except
// from inside server_stop(), which nothing could ever call). g_root_dir/
// g_ssl_enabled are set once in server_start() before this thread starts
// and never change for the life of the process, so reading them here
// unsynchronized is safe.
static const char *g_root_dir    = NULL;
static int          g_ssl_enabled = 0;
static pthread_t     accept_thread_id;
static int            accept_thread_started = 0;

static void *accept_loop_thread(void *arg) {
    (void)arg;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    fd_set readfds;
    int max_sd = server_fd_http;
    if (g_ssl_enabled && server_fd_https > max_sd) max_sd = server_fd_https;

    while (atomic_load(&running)) {
        FD_ZERO(&readfds);
        FD_SET(server_fd_http, &readfds);
        if (g_ssl_enabled) FD_SET(server_fd_https, &readfds);

        // A 1s timeout, not NULL: closing server_fd_http/https from
        // server_stop() (another thread) does NOT reliably wake a blocked
        // select() here (confirmed by hand - it can block forever even
        // after both fds are closed), so server_stop()'s pthread_join() on
        // this thread would otherwise hang indefinitely on shutdown. The
        // timeout bounds how long a SIGTERM/SIGINT takes to actually stop
        // the process to ~1s in the worst case, at the cost of one wakeup/s
        // while idle. select() may mutate `tv`, so it's reset every pass.
        struct timeval tv = {1, 0};
        int activity = select(max_sd + 1, &readfds, NULL, NULL, &tv);
        if (!atomic_load(&running)) break;
        if (activity == 0) continue; // timeout, no fd ready - just re-check `running`
        if (activity < 0) {
            if (errno != EINTR) LOG_ERROR("select: %s", strerror(errno));
            continue;
        }

        if (FD_ISSET(server_fd_http, &readfds)) {
            int cs = accept(server_fd_http, (struct sockaddr *)&addr, &addrlen);
            if (cs >= 0) {
                if (rate_limited_at_accept(addr.sin_addr)) {
                    LOG_WARN("Rate limit: dropping %s", inet_ntoa(addr.sin_addr));
                    reject_http_connection(cs);
                } else if (!try_register_connection()) {
                    LOG_WARN("Connection cap reached, dropping");
                    reject_http_connection(cs);
                } else {
#ifdef SO_NOSIGPIPE
                    int v = 1; setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, &v, sizeof(v));
#endif
                    spawn_thread(cs, NULL, g_root_dir);
                }
            }
        }

        if (g_ssl_enabled && FD_ISSET(server_fd_https, &readfds)) {
            int cs = accept(server_fd_https, (struct sockaddr *)&addr, &addrlen);
            if (cs >= 0) {
                if (rate_limited_at_accept(addr.sin_addr)) {
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
                        spawn_thread(cs, ssl, g_root_dir);
                    }
                }
            }
        }
    }

    return NULL;
}

int server_start(const char *root_dir, int ssl_enabled,
                 const char *ssl_cert, const char *ssl_key,
                 int http_port, int https_port) {
    signal(SIGPIPE, SIG_IGN);

    ip_table = calloc((size_t)ddos_max_ips, sizeof(ip_entry_t));
    if (!ip_table) {
        LOG_ERROR("calloc ip_table failed (ddos_max_ips=%d)", ddos_max_ips);
        return -1;
    }

    atomic_store(&cleanup_running, 1);
    if (pthread_create(&cleanup_thread_id, NULL, ip_table_cleanup_thread, NULL) != 0) {
        LOG_ERROR("pthread_create (ip_table cleanup) failed: %s", strerror(errno));
        free(ip_table);
        ip_table = NULL;
        return -1;
    }
    cleanup_thread_started = 1;

    const char *bind_text = listen_address[0] ? listen_address : "0.0.0.0";
    if (proxy_protocol && !trusted_proxies[0])
        LOG_WARN("proxy_protocol=1 with no trusted_proxies: every PROXY header will be refused");

    server_fd_http = make_listener(http_port);
    if (server_fd_http < 0) return -1;
    LOG_INFO("HTTP listening on %s:%d%s", bind_text, http_port,
             proxy_protocol ? " (PROXY protocol)" : "");

    if (ssl_enabled) {
        server_fd_https = make_listener(https_port);
        if (server_fd_https < 0) return -1;

        ssl_ctx = tls_create_context(ssl_cert, ssl_key);
        if (!ssl_ctx) return -1;
        LOG_INFO("HTTPS listening on %s:%d%s", bind_text, https_port,
                 proxy_protocol ? " (PROXY protocol)" : "");
    }

    g_root_dir    = root_dir;
    g_ssl_enabled = ssl_enabled;

    if (pthread_create(&accept_thread_id, NULL, accept_loop_thread, NULL) != 0) {
        LOG_ERROR("pthread_create (accept loop) failed: %s", strerror(errno));
        return -1;
    }
    accept_thread_started = 1;

    return 0;
}

void server_stop(void) {
    atomic_store(&running, 0);
    if (server_fd_http  >= 0) { close(server_fd_http);  server_fd_http  = -1; }
    if (server_fd_https >= 0) { close(server_fd_https); server_fd_https = -1; }

    if (accept_thread_started) {
        pthread_join(accept_thread_id, NULL);
        accept_thread_started = 0;
    }

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

    if (cleanup_thread_started) {
        atomic_store(&cleanup_running, 0);
        pthread_mutex_lock(&cleanup_wait_mutex);
        pthread_cond_signal(&cleanup_wait_cond);
        pthread_mutex_unlock(&cleanup_wait_mutex);
        pthread_join(cleanup_thread_id, NULL);
        cleanup_thread_started = 0;
    }

    free(ip_table);
    ip_table = NULL;
}
