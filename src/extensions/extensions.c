#define _GNU_SOURCE

#include "extensions.h"
#include "../db/cms_extensions.h"
#include "../utils/config_loader.h"
#include "../utils/log.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EXTENSIONS        32
#define MANIFEST_NAME         "extension.conf"
#define MAX_RESPONSE_BYTES    (4 * 1024 * 1024)
#define FORWARD_SEND_TIMEOUT  5    // seconds
#define FORWARD_RECV_TIMEOUT  15   // seconds - a slow handler, not a dead one
#define STOP_GRACE_SECONDS    3    // SIGTERM, then SIGKILL
#define CRASH_WINDOW_SECONDS  60
#define CRASH_LIMIT           5    // exits within the window before giving up

extern char **environ;

typedef struct {
    ExtensionInfo info;
    char   exec_path[PATH_MAX];
    char   dir[PATH_MAX];
    char   socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    pid_t  pid;
    time_t term_sent;        // SIGTERM sent at, 0 = not stopping
    time_t next_start;       // backoff after an exit
    int    crashes;
    time_t crash_window;
    int    present;          // seen by the last scan
} Extension;

static Extension         g_ext[MAX_EXTENSIONS];
static size_t            g_count;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t         g_thread;
static atomic_int        g_running;
static char              g_root[PATH_MAX];       // absolute ./extensions
static char              g_socket_root[PATH_MAX]; // absolute ./run/extensions
static atomic_ulong      g_request_seq;

// ---- Manifest -----------------------------------------------------------------

static int valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > EXTENSION_NAME_MAX) return 0;
    for (; *s; s++)
        if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

// Reads <dir>/extension.conf into `ext`. On a problem, ext->info.state is
// EXTENSION_INVALID and ext->info.problem says why; the name always comes
// from the directory, so an invalid extension is still listed.
static void read_manifest(Extension *ext, const char *dir_name) {
    ExtensionInfo *in = &ext->info;
    char enabled = (char)in->enabled;
    memset(in, 0, sizeof(*in));
    in->enabled = enabled;
    snprintf(in->name, sizeof(in->name), "%s", dir_name);
    snprintf(ext->dir, sizeof(ext->dir), "%.3000s/%.31s", g_root, dir_name);
    ext->exec_path[0] = '\0';

    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/" MANIFEST_NAME, ext->dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        in->state = EXTENSION_INVALID;
        snprintf(in->problem, sizeof(in->problem), "no %s", MANIFEST_NAME);
        return;
    }

    char name[64] = "", exec[256] = "", nav[256] = "";
    int protocol = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1);
        if      (!strcmp(key, "name"))        snprintf(name, sizeof(name), "%s", val);
        else if (!strcmp(key, "title"))       snprintf(in->title, sizeof(in->title), "%s", val);
        else if (!strcmp(key, "description")) snprintf(in->description, sizeof(in->description), "%s", val);
        else if (!strcmp(key, "version"))     snprintf(in->version, sizeof(in->version), "%s", val);
        else if (!strcmp(key, "protocol"))    protocol = atoi(val);
        else if (!strcmp(key, "exec"))        snprintf(exec, sizeof(exec), "%s", val);
        else if (!strcmp(key, "nav"))         snprintf(nav, sizeof(nav), "%s", val);
    }
    fclose(f);

    const char *problem = NULL;
    if (strcmp(name, dir_name) != 0)          problem = "name= must match the directory name";
    else if (!in->title[0])                   problem = "title= is missing";
    else if (!in->version[0])                 problem = "version= is missing";
    else if (protocol != EXTENSION_PROTOCOL)  problem = "unsupported protocol= (this Boat Rudder speaks 1)";
    else if (!exec[0] || exec[0] == '/' || strstr(exec, "..")) problem = "exec= must be a path inside the extension's directory";

    if (!problem) {
        snprintf(ext->exec_path, sizeof(ext->exec_path), "%.3800s/%.255s", ext->dir, exec);
        if (access(ext->exec_path, X_OK) != 0) problem = "exec= names no executable file - not built?";
    }
    if (!problem) {
        int n = snprintf(ext->socket_path, sizeof(ext->socket_path), "%s/%s.sock", g_socket_root, dir_name);
        if (n < 0 || (size_t)n >= sizeof(ext->socket_path)) problem = "socket path too long - install Boat Rudder in a shorter directory";
    }
    if (!problem && nav[0]) {
        char *bar = strchr(nav, '|');
        const char *label = nav, *p = "/";
        if (bar) { *bar = '\0'; p = trim(bar + 1); }
        if (p[0] != '/' || strstr(p, "..") || strpbrk(p, "\"'<> ")) {
            problem = "nav= path must start with '/'";
        } else {
            snprintf(in->nav_label, sizeof(in->nav_label), "%s", trim((char *)label));
            snprintf(in->nav_path, sizeof(in->nav_path), "%s", p);
        }
    }

    if (problem) {
        in->state = EXTENSION_INVALID;
        snprintf(in->problem, sizeof(in->problem), "%s", problem);
    } else {
        in->state = EXTENSION_DISABLED;
    }
}

// ---- Processes ----------------------------------------------------------------

// The SIGCHLD handler in main.c reaps every child as it exits, so a process
// that is gone is simply no longer there to signal.
static int process_alive(pid_t pid) {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int socket_answers(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

// Everything the child needs is built here, before fork(): between fork()
// and execve() a multi-threaded parent's child may only call
// async-signal-safe functions.
static pid_t spawn(Extension *ext) {
    char env_name[64], env_dir[PATH_MAX + 32], env_sock[160], env_uri[300], env_db[96], env_log[32];
    snprintf(env_name, sizeof(env_name), "BR_EXTENSION=%s", ext->info.name);
    snprintf(env_dir, sizeof(env_dir), "BR_EXTENSION_DIR=%s", ext->dir);
    snprintf(env_sock, sizeof(env_sock), "BR_SOCKET=%s", ext->socket_path);
    snprintf(env_uri, sizeof(env_uri), "BR_MONGODB_URI=%s", mongodb_uri);
    snprintf(env_db, sizeof(env_db), "BR_MONGODB_DB=%s", mongodb_db);
    snprintf(env_log, sizeof(env_log), "BR_LOG_LEVEL=%d", verbose_level);

    size_t n = 0;
    while (environ && environ[n]) n++;
    char **envp = calloc(n + 8, sizeof(char *));
    if (!envp) return -1;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "BR_", 3) != 0) envp[k++] = environ[i];
    envp[k++] = env_name; envp[k++] = env_dir; envp[k++] = env_sock;
    envp[k++] = env_uri;  envp[k++] = env_db;  envp[k++] = env_log;
    envp[k] = NULL;
    char *argv[] = { ext->exec_path, NULL };

    unlink(ext->socket_path);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        // Dies with the server (the supervisor thread, which forked it, lives
        // as long as the server does).
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(1);
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        sigaction(SIGPIPE, &dfl, NULL);   // the server ignores it; exec keeps "ignored"
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        if (chdir(ext->dir) != 0) _exit(126);
        execve(ext->exec_path, argv, envp);
        _exit(127);
    }
    free(envp);
    return pid;
}

static void request_stop(Extension *ext, time_t now) {
    if (ext->pid <= 0) return;
    if (!process_alive(ext->pid)) { ext->pid = 0; ext->term_sent = 0; return; }
    if (!ext->term_sent) {
        kill(ext->pid, SIGTERM);
        ext->term_sent = now;
    } else if (now - ext->term_sent >= STOP_GRACE_SECONDS) {
        kill(ext->pid, SIGKILL);
    }
}

// One supervisor tick for one extension, under g_lock.
static void reconcile(Extension *ext, time_t now) {
    ExtensionInfo *in = &ext->info;
    int wanted = in->enabled && in->state != EXTENSION_INVALID && ext->present;

    if (!wanted) {
        request_stop(ext, now);
        if (in->state != EXTENSION_INVALID) {
            in->state = EXTENSION_DISABLED;
            in->problem[0] = '\0';
        }
        return;
    }
    if (in->state == EXTENSION_FAILED && !ext->pid) return;   // until re-enabled

    if (ext->pid && !process_alive(ext->pid)) {
        LOG_WARN("Extension %s exited", in->name);
        ext->pid = 0;
        if (now - ext->crash_window > CRASH_WINDOW_SECONDS) {
            ext->crash_window = now;
            ext->crashes = 0;
        }
        if (++ext->crashes >= CRASH_LIMIT) {
            in->state = EXTENSION_FAILED;
            snprintf(in->problem, sizeof(in->problem),
                     "exited %d times within %d s - see the server log; disable and enable it to retry",
                     CRASH_LIMIT, CRASH_WINDOW_SECONDS);
            LOG_ERROR("Extension %s: %s", in->name, in->problem);
            return;
        }
        ext->next_start = now + ext->crashes;   // 1 s, 2 s, ... between restarts
        in->state = EXTENSION_STARTING;
    }

    if (!ext->pid) {
        if (now < ext->next_start) return;
        ext->pid = spawn(ext);
        if (ext->pid < 0) {
            ext->pid = 0;
            in->state = EXTENSION_FAILED;
            snprintf(in->problem, sizeof(in->problem), "could not be launched: %s", strerror(errno));
            return;
        }
        LOG_INFO("Extension %s %s started (pid %d)", in->name, in->version, (int)ext->pid);
        in->state = EXTENSION_STARTING;
        in->problem[0] = '\0';
        return;
    }

    in->state = socket_answers(ext->socket_path) ? EXTENSION_RUNNING : EXTENSION_STARTING;
}

static void *supervisor(void *arg) {
    (void)arg;
    while (atomic_load(&g_running)) {
        time_t now = time(NULL);
        pthread_mutex_lock(&g_lock);
        for (size_t i = 0; i < g_count; i++) reconcile(&g_ext[i], now);
        pthread_mutex_unlock(&g_lock);
        sleep(1);
    }
    return NULL;
}

// ---- Registry -----------------------------------------------------------------

static Extension *find(const char *name) {
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_ext[i].info.name, name) == 0) return &g_ext[i];
    return NULL;
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const Extension *)a)->info.name, ((const Extension *)b)->info.name);
}

// Under g_lock. An extension whose directory is gone is dropped once its
// process has stopped (the supervisor stops it, since it isn't present).
static void scan_locked(void) {
    for (size_t i = 0; i < g_count; i++) g_ext[i].present = 0;

    DIR *d = opendir(g_root);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) {
        if (!valid_name(de->d_name)) continue;
        char path[PATH_MAX + 64];
        struct stat st;
        snprintf(path, sizeof(path), "%.3000s/%.255s", g_root, de->d_name);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        Extension *ext = find(de->d_name);
        if (!ext) {
            if (g_count >= MAX_EXTENSIONS) {
                LOG_WARN("Extensions: more than %d installed - %s ignored", MAX_EXTENSIONS, de->d_name);
                continue;
            }
            ext = &g_ext[g_count++];
            memset(ext, 0, sizeof(*ext));
        }
        // A running extension keeps its state; the manifest is re-read for
        // the next start and for the admin page.
        ExtensionState was = ext->info.state;
        char problem[sizeof(ext->info.problem)];
        snprintf(problem, sizeof(problem), "%s", ext->info.problem);
        read_manifest(ext, de->d_name);
        if (ext->info.state != EXTENSION_INVALID && ext->pid) {
            ext->info.state = was;
            snprintf(ext->info.problem, sizeof(ext->info.problem), "%s", problem);
        } else if (ext->info.state != EXTENSION_INVALID && was == EXTENSION_FAILED) {
            ext->info.state = was;
            snprintf(ext->info.problem, sizeof(ext->info.problem), "%s", problem);
        }
        ext->present = 1;
    }
    if (d) closedir(d);

    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        if (!g_ext[i].present && !g_ext[i].pid) continue;
        if (kept != i) g_ext[kept] = g_ext[i];
        kept++;
    }
    g_count = kept;
    qsort(g_ext, g_count, sizeof(Extension), by_name);
}

static void absolute_dir(const char *rel, char *out, size_t size) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    snprintf(out, size, "%s/%s", cwd, rel + (strncmp(rel, "./", 2) == 0 ? 2 : 0));
}

int extensions_start(void) {
    absolute_dir(EXTENSIONS_DIR, g_root, sizeof(g_root));
    absolute_dir(EXTENSIONS_SOCKET_DIR, g_socket_root, sizeof(g_socket_root));
    mkdir("./run", 0750);
    mkdir(EXTENSIONS_SOCKET_DIR, 0750);

    char enabled[MAX_EXTENSIONS][33];
    int n = cms_extensions_enabled(enabled, MAX_EXTENSIONS);

    pthread_mutex_lock(&g_lock);
    scan_locked();
    for (int i = 0; i < n; i++) {
        Extension *ext = find(enabled[i]);
        if (ext) ext->info.enabled = 1;
    }
    size_t count = g_count;
    pthread_mutex_unlock(&g_lock);

    if (count > 0) LOG_INFO("Extensions     : %zu installed in %s", count, g_root);

    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, supervisor, NULL) != 0) {
        atomic_store(&g_running, 0);
        LOG_ERROR("Extensions: supervisor thread could not start");
        return -1;
    }
    return 0;
}

void extensions_stop(void) {
    if (!atomic_exchange(&g_running, 0)) return;
    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_count; i++)
        if (g_ext[i].pid > 0) kill(g_ext[i].pid, SIGTERM);
    for (int t = 0; t < STOP_GRACE_SECONDS * 10; t++) {
        int alive = 0;
        for (size_t i = 0; i < g_count; i++) alive |= process_alive(g_ext[i].pid);
        if (!alive) break;
        usleep(100000);
    }
    for (size_t i = 0; i < g_count; i++) {
        if (process_alive(g_ext[i].pid)) kill(g_ext[i].pid, SIGKILL);
        g_ext[i].pid = 0;
        unlink(g_ext[i].socket_path);
    }
    pthread_mutex_unlock(&g_lock);
}

void extensions_rescan(void) {
    pthread_mutex_lock(&g_lock);
    scan_locked();
    pthread_mutex_unlock(&g_lock);
}

size_t extensions_list(ExtensionInfo **out) {
    *out = NULL;
    pthread_mutex_lock(&g_lock);
    size_t count = 0;
    for (size_t i = 0; i < g_count; i++) count += g_ext[i].present;
    if (count > 0) *out = calloc(count, sizeof(ExtensionInfo));
    size_t k = 0;
    for (size_t i = 0; *out && i < g_count; i++)
        if (g_ext[i].present) (*out)[k++] = g_ext[i].info;
    pthread_mutex_unlock(&g_lock);
    return *out ? count : 0;
}

int extensions_get(const char *name, ExtensionInfo *out) {
    pthread_mutex_lock(&g_lock);
    Extension *ext = find(name);
    int found = ext && ext->present;
    if (found) *out = ext->info;
    pthread_mutex_unlock(&g_lock);
    return found;
}

int extensions_set_enabled(const char *name, int enabled) {
    pthread_mutex_lock(&g_lock);
    Extension *ext = find(name);
    int ok = ext && ext->present && ext->info.state != EXTENSION_INVALID;
    pthread_mutex_unlock(&g_lock);
    if (!ok) return -1;

    if (cms_extensions_set_enabled(name, enabled) != 0) return -2;

    pthread_mutex_lock(&g_lock);
    ext = find(name);
    if (ext) {
        ext->info.enabled = enabled != 0;
        if (enabled) {   // a fresh start: forget earlier failures
            ext->crashes = 0;
            ext->next_start = 0;
            if (ext->info.state == EXTENSION_FAILED) {
                ext->info.state = EXTENSION_DISABLED;
                ext->info.problem[0] = '\0';
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    LOG_INFO("Extension %s %s", name, enabled ? "enabled" : "disabled");
    return 0;
}

// ---- Forwarding ---------------------------------------------------------------

// A header value can't carry a line break: anything below 0x20 (and DEL)
// becomes a space.
static void header_value(char *out, size_t size, const char *v) {
    size_t k = 0;
    for (; v && *v && k + 1 < size; v++)
        out[k++] = ((unsigned char)*v < 0x20 || *v == 0x7f) ? ' ' : *v;
    out[k] = '\0';
}

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// Value of `name` in the header block [p, end), copied into out ("" if absent).
static void response_header(const char *p, const char *end, const char *name, char *out, size_t size) {
    out[0] = '\0';
    size_t nl = strlen(name);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        if ((size_t)(eol - p) > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *e = eol;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ')) e--;
            snprintf(out, size, "%.*s", (int)(e - v), v);
            return;
        }
        p = eol + 1;
    }
}

int extensions_forward(const char *name, const ExtensionRequest *req, ExtensionResponse *out) {
    memset(out, 0, sizeof(*out));

    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    pthread_mutex_lock(&g_lock);
    Extension *ext = find(name);
    int running = ext && ext->info.enabled && ext->info.state == EXTENSION_RUNNING;
    if (running) snprintf(socket_path, sizeof(socket_path), "%s", ext->socket_path);
    pthread_mutex_unlock(&g_lock);
    if (!running) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval snd = { .tv_sec = FORWARD_SEND_TIMEOUT }, rcv = { .tv_sec = FORWARD_RECV_TIMEOUT };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("Extension %s: connect failed: %s", name, strerror(errno));
        close(fd);
        return -1;
    }

    char request_id[32];
    if (req->request_id && req->request_id[0])
        snprintf(request_id, sizeof(request_id), "%s", req->request_id);
    else
        snprintf(request_id, sizeof(request_id), "%lx-%lx", (unsigned long)time(NULL),
                 (unsigned long)atomic_fetch_add(&g_request_seq, 1));

    char mount[128], target[2048], ctype[256], uid[64], uname[256], role[32], lang[32], theme[64];
    header_value(mount, sizeof(mount), req->mount);
    header_value(target, sizeof(target), req->target);
    header_value(ctype, sizeof(ctype), req->content_type);
    header_value(uid, sizeof(uid), req->user_id);
    header_value(uname, sizeof(uname), req->user_name);
    header_value(role, sizeof(role), req->role);
    header_value(lang, sizeof(lang), req->lang);
    header_value(theme, sizeof(theme), req->theme);
    for (char *c = target; *c; c++) if (*c == ' ') *c = '+';

    char head[4096];
    int hl = snprintf(head, sizeof(head),
        "%s %s HTTP/1.1\r\n"
        "Host: boat-rudder\r\n"
        "X-BR-Protocol: %d\r\n"
        "X-BR-Request-Id: %s\r\n"
        "X-BR-Mount: %s\r\n"
        "X-BR-User-Id: %s\r\n"
        "X-BR-User-Name: %s\r\n"
        "X-BR-Role: %s\r\n"
        "X-BR-Lang: %s\r\n"
        "X-BR-Theme: %s\r\n"
        "X-BR-Epoch: %d\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        req->method, target[0] ? target : "/", EXTENSION_PROTOCOL, request_id, mount,
        uid, uname, role, lang, theme, req->epoch,
        ctype[0] ? "Content-Type: " : "", ctype, ctype[0] ? "\r\n" : "",
        req->body ? req->body_len : 0);
    if (hl < 0 || (size_t)hl >= sizeof(head) ||
        write_all(fd, head, (size_t)hl) != 0 ||
        (req->body && req->body_len && write_all(fd, req->body, req->body_len) != 0)) {
        LOG_WARN("Extension %s: request %s could not be sent", name, request_id);
        close(fd);
        return -1;
    }

    size_t cap = 16384, len = 0;
    char *buf = malloc(cap + 1);
    int rc = buf ? 0 : -1;
    while (rc == 0) {
        if (len == cap) {
            if (cap >= MAX_RESPONSE_BYTES) { rc = -2; break; }
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { rc = -1; break; }
            buf = nb;
        }
        ssize_t r = recv(fd, buf + len, cap - len, 0);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("Extension %s: no answer to %s (%s)", name, request_id, strerror(errno));
            rc = -1;
            break;
        }
        len += (size_t)r;
    }
    close(fd);
    if (rc != 0) { free(buf); return rc; }
    buf[len] = '\0';

    int status = 0;
    char *sep = strstr(buf, "\r\n\r\n");
    size_t sep_len = 4;
    if (!sep) { sep = strstr(buf, "\n\n"); sep_len = 2; }
    if (!sep || sscanf(buf, "HTTP/1.%*d %d", &status) != 1 || status < 100 || status > 599) {
        LOG_WARN("Extension %s: malformed answer to %s", name, request_id);
        free(buf);
        return -2;
    }

    out->status = status;
    response_header(buf, sep, "Content-Type", out->content_type, sizeof(out->content_type));
    response_header(buf, sep, "X-BR-Title", out->title, sizeof(out->title));
    response_header(buf, sep, "Location", out->location, sizeof(out->location));

    char *body = sep + sep_len;
    out->body_len = len - (size_t)(body - buf);
    memmove(buf, body, out->body_len + 1);
    out->body = buf;
    LOG_DEBUG("Extension %s: %s %s -> %d (%s)", name, req->method, target, status, request_id);
    return 0;
}
