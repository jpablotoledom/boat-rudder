#include "server_listener.h"
#include "connection_thread.h"
#include "tls_context.h"
#include "../utils/config_loader.h"
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
typedef struct {
    in_addr_t ip;
    time_t    last_conn;
    time_t    last_rejected;
    int       count;
    int       blocked;
} ip_entry_t;

// malloc'd (calloc'd) in server_start() from ddos_max_ips, freed in
// server_stop(). Touched by both the accept loop (this file's main thread)
// and ip_table_cleanup_thread() below, so every access goes through
// ip_table_mutex - unlike the original port of this table, which was only
// ever read/written from the single accept-loop thread and had no lock.
static ip_entry_t     *ip_table = NULL;
static pthread_mutex_t ip_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static int too_many_connections(struct in_addr client_ip) {
    time_t    now = time(NULL);
    in_addr_t ip  = client_ip.s_addr;

    pthread_mutex_lock(&ip_table_mutex);

    for (int i = 0; i < ddos_max_ips; i++) {
        ip_entry_t *e = &ip_table[i];
        if (e->ip != ip) continue;

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
                LOG_WARN("Blocking IP %s for %d s", inet_ntoa(client_ip), ddos_rate_window_secs * 2);
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

    // New IP - find an empty slot first
    for (int i = 0; i < ddos_max_ips; i++) {
        if (ip_table[i].ip == 0) {
            ip_table[i] = (ip_entry_t){
                .ip = ip, .count = 1, .last_conn = now, .blocked = 0
            };
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
    ip_table[oldest_idx] = (ip_entry_t){
        .ip = ip, .count = 1, .last_conn = now, .blocked = 0
    };
    pthread_mutex_unlock(&ip_table_mutex);
    return 0;
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
            if (e->ip == 0) continue;

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

static void reject_http_connection(int cs) {
    // Best-effort: SIGPIPE is already ignored (signal() call in
    // server_start()), so a client that has already hung up just makes this
    // write() fail (EPIPE) rather than crash - ignored either way.
    write(cs, RESPONSE_429, sizeof(RESPONSE_429) - 1);
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
                if (too_many_connections(addr.sin_addr)) {
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
