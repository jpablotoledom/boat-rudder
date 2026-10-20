#include "updates_admin.h"
#include "../../utils/config_loader.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/log.h"
#include "../../utils/markdown_lite.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include "../../utils/version.h"
#include <bson/bson.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK_MAX_AGE_SECS  3600
#define MAX_RELEASES        30
#define MAX_RESPONSE_BYTES  (4 * 1024 * 1024)

typedef struct {
    char  version[64];     // from the tag: "v0.0.4" and "v.0.0.3" give "0.0.4", "0.0.3"
    char  tag[64];
    char  date[11];        // YYYY-MM-DD
    char  url[256];        // the release page, https://github.com/... or ""
    char *name;
    char *notes;           // Markdown
    bool  prerelease;
    bool  installable;     // has boat-rudder-<version>-src.tar.gz and its .minisig
} Release;

// The last fetch, shared by every request; guarded by cache_lock.
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static Release cache_releases[MAX_RELEASES];
static int     cache_count;
static char    cache_error[200];
static time_t  cache_checked_at;

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *html_escape(const char *src) {
    size_t n = 0;
    for (const char *p = src; *p; p++) n += (*p == '&' || *p == '<' || *p == '>' || *p == '"' || *p == '\'') ? 6 : 1;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    char *o = out;
    for (const char *p = src; *p; p++) {
        const char *e = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" :
                        *p == '"' ? "&quot;" : *p == '\'' ? "&#39;" : NULL;
        if (e) { size_t k = strlen(e); memcpy(o, e, k); o += k; } else *o++ = *p;
    }
    *o = '\0';
    return out;
}

// Only [A-Za-z0-9._-] and, when `slash_ok`, exactly one '/' - what goes into
// the curl command line and into URLs.
static bool safe_word(const char *s, bool slash_ok) {
    int slashes = 0;
    if (!s[0]) return false;
    for (const char *p = s; *p; p++) {
        if (*p == '/' && slash_ok) { slashes++; continue; }
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return false;
    }
    return !slash_ok || slashes == 1;
}

// "1.2.3" into up to 4 numbers. Returns how many, or 0 when `s` isn't a
// dotted version.
static int parse_version(const char *s, long out[4]) {
    int n = 0;
    while (*s) {
        if (n == 4 || !isdigit((unsigned char)*s)) return 0;
        char *end;
        out[n++] = strtol(s, &end, 10);
        s = end;
        if (*s == '.') { s++; if (!*s) return 0; } else if (*s) return 0;
    }
    return n;
}

// <0, 0, >0 as a is older, the same, newer than b; unparseable versions sort
// below every real one.
static int version_cmp(const char *a, const char *b) {
    long va[4] = {0}, vb[4] = {0};
    int na = parse_version(a, va), nb = parse_version(b, vb);
    if (!na || !nb) return na - nb;
    for (int i = 0; i < 4; i++)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

static int release_newest_first(const void *a, const void *b) {
    return version_cmp(((const Release *)b)->version, ((const Release *)a)->version);
}

static void free_releases(Release *list, int count) {
    for (int i = 0; i < count; i++) {
        free(list[i].name);
        free(list[i].notes);
    }
}

static void copy_utf8(bson_iter_t *it, char *dst, size_t size) {
    if (BSON_ITER_HOLDS_UTF8(it)) {
        snprintf(dst, size, "%s", bson_iter_utf8(it, NULL));
    }
}

// One element of GitHub's release array into `r`. False for drafts and tags
// that aren't a version.
static bool parse_release(const bson_t *doc, Release *r) {
    memset(r, 0, sizeof(*r));
    char want_pkg[96] = "", want_sig[104] = "";
    bool has_pkg = false, has_sig = false;
    bson_iter_t it;

    if (bson_iter_init_find(&it, doc, "draft") && BSON_ITER_HOLDS_BOOL(&it) && bson_iter_bool(&it))
        return false;
    if (bson_iter_init_find(&it, doc, "tag_name")) copy_utf8(&it, r->tag, sizeof(r->tag));

    const char *v = r->tag;
    while (*v == 'v' || *v == 'V' || *v == '.') v++;
    snprintf(r->version, sizeof(r->version), "%s", v);
    long parts[4];
    if (!parse_version(r->version, parts) || !safe_word(r->tag, false)) return false;

    if (bson_iter_init_find(&it, doc, "published_at")) copy_utf8(&it, r->date, sizeof(r->date));
    if (bson_iter_init_find(&it, doc, "html_url")) copy_utf8(&it, r->url, sizeof(r->url));
    if (strncmp(r->url, "https://github.com/", 19) != 0) r->url[0] = '\0';
    if (bson_iter_init_find(&it, doc, "prerelease") && BSON_ITER_HOLDS_BOOL(&it))
        r->prerelease = bson_iter_bool(&it);

    bson_iter_t field;
    if (bson_iter_init_find(&field, doc, "name") && BSON_ITER_HOLDS_UTF8(&field))
        r->name = strdup(bson_iter_utf8(&field, NULL));
    if (bson_iter_init_find(&field, doc, "body") && BSON_ITER_HOLDS_UTF8(&field))
        r->notes = strdup(bson_iter_utf8(&field, NULL));

    snprintf(want_pkg, sizeof(want_pkg), "boat-rudder-%s-src.tar.gz", r->version);
    snprintf(want_sig, sizeof(want_sig), "%s.minisig", want_pkg);
    bson_iter_t assets, asset;
    if (bson_iter_init_find(&it, doc, "assets") && BSON_ITER_HOLDS_ARRAY(&it) &&
        bson_iter_recurse(&it, &assets)) {
        while (bson_iter_next(&assets)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&assets) || !bson_iter_recurse(&assets, &asset)) continue;
            if (!bson_iter_find(&asset, "name") || !BSON_ITER_HOLDS_UTF8(&asset)) continue;
            const char *n = bson_iter_utf8(&asset, NULL);
            if (strcmp(n, want_pkg) == 0) has_pkg = true;
            if (strcmp(n, want_sig) == 0) has_sig = true;
        }
    }
    r->installable = has_pkg && has_sig;
    return true;
}

// Runs curl against the GitHub API. Returns the response body (malloc'd) on
// HTTP 200, else NULL with `error` set. The exit status can't be used -
// main.c's SIGCHLD handler reaps every child, pclose() included - so curl
// appends the HTTP status as a last line ("000" when there was no answer) and
// its own error message comes through stderr.
static char *fetch_releases_json(char *error, size_t error_size) {
    if (!safe_word(update_repo, true)) {
        snprintf(error, error_size, "update_repo in settings.conf is not a GitHub \"owner/name\".");
        return NULL;
    }
    const char *ua_version = safe_word(BOAT_RUDDER_VERSION, false) ? BOAT_RUDDER_VERSION : "unknown";

    char command[512];
    snprintf(command, sizeof(command),
             "curl -sSL --proto =https --max-time 10 "
             "-H 'Accept: application/vnd.github+json' -H 'User-Agent: boat-rudder/%s' "
             "-w '\\n%%{http_code}' 'https://api.github.com/repos/%s/releases?per_page=%d' 2>&1",
             ua_version, update_repo, MAX_RELEASES);

    FILE *pipe = popen(command, "r");
    if (!pipe) {
        snprintf(error, error_size, "Couldn't run curl.");
        return NULL;
    }
    size_t cap = 64 * 1024, len = 0;
    char *out = malloc(cap);
    while (out) {
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_RESPONSE_BYTES) break;
            char *grown = realloc(out, cap * 2);
            if (!grown) { free(out); out = NULL; break; }
            out = grown;
            cap *= 2;
        }
        size_t got = fread(out + len, 1, 4096, pipe);
        if (got == 0) break;
        len += got;
    }
    pclose(pipe);
    if (!out) {
        snprintf(error, error_size, "Out of memory reading GitHub's answer.");
        return NULL;
    }
    out[len] = '\0';

    char *last = strrchr(out, '\n');
    const char *code_str = last ? last + 1 : "";
    int code = (strlen(code_str) == 3 && isdigit((unsigned char)code_str[0])) ? atoi(code_str) : -1;

    if (code == 200) {
        *last = '\0';
        return out;
    }
    if (code == 403 || code == 429) {
        snprintf(error, error_size, "GitHub's limit of requests for this server's address was reached - try again in an hour.");
    } else if (code == 404) {
        snprintf(error, error_size, "github.com/%s was not found (update_repo in settings.conf).", update_repo);
    } else if (code > 0) {
        snprintf(error, error_size, "GitHub answered HTTP %d.", code);
    } else {
        // No answer: the output is curl's or the shell's message ("curl: (6)
        // Could not resolve host...", "sh: 1: curl: not found").
        size_t n = strcspn(out, "\n");
        snprintf(error, error_size, "%.*s", (int)(n > 160 ? 160 : n), n ? out : "No answer from GitHub.");
    }
    LOG_WARN("updates: release check failed: %s", error);
    free(out);
    return NULL;
}

static void refresh_cache(void) {
    char error[200] = "";
    Release fresh[MAX_RELEASES];
    int count = 0;

    char *json = fetch_releases_json(error, sizeof(error));
    if (json) {
        bson_t doc;
        bson_error_t bson_error;
        if (bson_init_from_json(&doc, json, -1, &bson_error)) {
            bson_iter_t it;
            if (bson_iter_init(&it, &doc)) {
                while (count < MAX_RELEASES && bson_iter_next(&it)) {
                    if (!BSON_ITER_HOLDS_DOCUMENT(&it)) continue;
                    uint32_t length;
                    const uint8_t *data;
                    bson_iter_document(&it, &length, &data);
                    bson_t release;
                    if (bson_init_static(&release, data, length) && parse_release(&release, &fresh[count]))
                        count++;
                }
            }
            bson_destroy(&doc);
            qsort(fresh, (size_t)count, sizeof(Release), release_newest_first);
        } else {
            snprintf(error, sizeof(error), "GitHub's answer is not the expected JSON.");
            LOG_WARN("updates: unparseable release list: %s", bson_error.message);
        }
        free(json);
    }

    pthread_mutex_lock(&cache_lock);
    free_releases(cache_releases, cache_count);
    memcpy(cache_releases, fresh, sizeof(Release) * (size_t)count);
    cache_count = count;
    snprintf(cache_error, sizeof(cache_error), "%s", error);
    cache_checked_at = time(NULL);
    pthread_mutex_unlock(&cache_lock);
}

void updates_admin_check_now(void) {
    refresh_cache();
}

static const char *const BADGE_INSTALLED  = "<span class=\"boat-rudder-badge boat-rudder-badge--accent\">Installed</span>";
static const char *const BADGE_NEW        = "<span class=\"boat-rudder-badge boat-rudder-badge--accent\">New</span>";
static const char *const BADGE_PRERELEASE = "<span class=\"boat-rudder-badge\">Pre-release</span>";
static const char *const BADGE_UNSIGNED   = "<span class=\"boat-rudder-badge\">No signed package</span>";

// One release's <details>; caller holds cache_lock.
static char *render_release(const char *tpl, const Release *r) {
    int cmp = version_cmp(r->version, BOAT_RUDDER_VERSION);
    char badges[512] = "";
    if (cmp == 0) strcat(badges, BADGE_INSTALLED);
    if (cmp > 0 && r->installable) strcat(badges, BADGE_NEW);
    if (r->prerelease) strcat(badges, BADGE_PRERELEASE);
    if (!r->installable) strcat(badges, BADGE_UNSIGNED);

    char link_base[256];
    snprintf(link_base, sizeof(link_base), "https://github.com/%s/blob/%s/", update_repo, r->tag);
    char *notes = markdown_to_html(r->notes ? r->notes : "", link_base);
    char *name  = html_escape(r->name && r->name[0] ? r->name : r->tag);
    char *url   = html_escape(r->url[0] ? r->url : "https://github.com/");
    char *html  = notes && name && url
        ? render_template(tpl, cmp >= 0 ? " open" : "", r->version, badges, r->date, name, url, notes)
        : NULL;
    free(notes);
    free(name);
    free(url);
    return html;
}

// ---- The updater's state (.boat-rudder/ in the working directory) ----------

#define STATE_DIR          ".boat-rudder"
#define UPDATER_PATH       "scripts/boat-rudder-update"
#define QUEUED_TIMEOUT     120          // a request the updater never picked up
#define RUNNING_TIMEOUT    (2 * 3600)   // a run that stopped reporting
#define LOG_TAIL_BYTES     6000

typedef struct {
    char id[32];
    char action[16];      // install | rollback
    char version[64];
    char from[64];
    char user[128];
    char state[16];       // queued | running | done | failed | rolled-back | none
    char stage[32];
    char message[512];
    char started_at[32];
    char updated_at[32];
    bool active;          // queued or running, and not stale
} Progress;

// The last run, from progress.json. A queued or running run that stopped
// reporting long ago is shown as failed, so it can't block the page forever.
static void read_progress(Progress *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->state, sizeof(p->state), "none");

    struct stat st;
    char *json = stat(STATE_DIR "/progress.json", &st) == 0 ? read_file_to_string(STATE_DIR "/progress.json") : NULL;
    if (!json) return;
    bson_t doc;
    bson_error_t error;
    if (bson_init_from_json(&doc, json, -1, &error)) {
        struct { const char *key; char *dst; size_t size; } fields[] = {
            { "id", p->id, sizeof(p->id) },             { "action", p->action, sizeof(p->action) },
            { "version", p->version, sizeof(p->version) }, { "from", p->from, sizeof(p->from) },
            { "user", p->user, sizeof(p->user) },       { "state", p->state, sizeof(p->state) },
            { "stage", p->stage, sizeof(p->stage) },    { "message", p->message, sizeof(p->message) },
            { "started_at", p->started_at, sizeof(p->started_at) },
            { "updated_at", p->updated_at, sizeof(p->updated_at) },
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            bson_iter_t it;
            if (bson_iter_init_find(&it, &doc, fields[i].key)) copy_utf8(&it, fields[i].dst, fields[i].size);
        }
        bson_destroy(&doc);
    }
    free(json);

    time_t age = time(NULL) - st.st_mtime;
    bool queued = strcmp(p->state, "queued") == 0, running = strcmp(p->state, "running") == 0;
    if ((queued && age > QUEUED_TIMEOUT) || (running && age > RUNNING_TIMEOUT)) {
        snprintf(p->message, sizeof(p->message), queued
                 ? "The updater never started - see the server log."
                 : "The updater stopped reporting - see .boat-rudder/last-run.log on the server.");
        snprintf(p->state, sizeof(p->state), "failed");
    }
    p->active = strcmp(p->state, "queued") == 0 || strcmp(p->state, "running") == 0;
}

// The last lines of the run's log, printable ASCII only (it goes into JSON
// and HTML). Malloc'd, "" when there is none.
static char *read_log_tail(void) {
    char *log = read_file_to_string(STATE_DIR "/last-run.log");
    if (!log) return strdup("");
    size_t n = strlen(log);
    const char *start = n > LOG_TAIL_BYTES ? log + n - LOG_TAIL_BYTES : log;
    if (start != log) { const char *nl = strchr(start, '\n'); if (nl) start = nl + 1; }
    char *out = strdup(start);
    free(log);
    if (out)
        for (char *c = out; *c; c++)
            if ((*c < 0x20 && *c != '\n') || (unsigned char)*c >= 0x7f) *c = '?';
    return out;
}

// Why this server can't update itself, or NULL when it can: it must run from
// an install (install.sh wrote .boat-rudder/), with the updater and
// systemd-run present.
static const char *apply_unavailable_reason(void) {
    if (access(STATE_DIR "/manifest.sha256", R_OK) != 0)
        return "This server isn't running from an install made by install.sh (no .boat-rudder/ here).";
    if (access(UPDATER_PATH, X_OK) != 0)
        return "scripts/boat-rudder-update is missing from this install.";
    if (access("/usr/bin/systemd-run", X_OK) != 0 && access("/bin/systemd-run", X_OK) != 0)
        return "systemd-run isn't available on this host.";
    return NULL;
}

// The newest update that can still be undone - .boat-rudder/backups/
// <date>-<from>-to-<to>/ without a rolled-back marker - when it is the update
// that installed the running version. Fills `from`/`date`; false if none.
static bool rollback_target(char *from, size_t from_size, char *date, size_t date_size) {
    DIR *dir = opendir(STATE_DIR "/backups");
    if (!dir) return false;
    char newest[256] = "";
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(newest)) continue;
        if (strcmp(e->d_name, newest) > 0) {
            char marker[512];
            snprintf(marker, sizeof(marker), STATE_DIR "/backups/%s/rolled-back", e->d_name);
            if (access(marker, F_OK) != 0) snprintf(newest, sizeof(newest), "%s", e->d_name);
        }
    }
    closedir(dir);

    // 20261015-093000-0.0.4-to-0.0.5
    char *to = strstr(newest, "-to-");
    if (!newest[0] || !to || strlen(newest) < 17 || strcmp(to + 4, BOAT_RUDDER_VERSION) != 0) return false;
    *to = '\0';
    size_t from_len = strlen(newest + 16);
    if (from_len == 0 || from_len >= from_size) return false;   // not a version this page made
    memcpy(from, newest + 16, from_len + 1);
    snprintf(date, date_size, "%.4s-%.2s-%.2s %.2s:%.2s UTC", newest, newest + 4, newest + 6, newest + 9, newest + 11);
    long parts[4];
    return parse_version(from, parts) > 0;
}

// Only characters that can sit inside single quotes on a shell command line
// and mean nothing there.
static bool shell_safe(const char *s, const char *extra) {
    for (const char *c = s; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr(extra, *c)) return false;
    return s[0] != '\0';
}

static int write_file_atomic(const char *path, const char *content, mode_t mode) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t len = strlen(content);
    bool ok = write(fd, content, len) == (ssize_t)len;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

// The transient unit's name: BR_UPDATE_UNIT when this server's unit sets it
// - one name per install, so installs sharing a host can each run an update
// while one install still can't run two at once - else boat-rudder-update.
static void updater_unit_name(char *out, size_t out_size) {
    const char *unit = getenv("BR_UPDATE_UNIT");
    if (unit && strlen(unit) < 80 && shell_safe(unit, "_.@-"))
        snprintf(out, out_size, "%s", unit);
    else
        snprintf(out, out_size, "boat-rudder-update");
}

// Starts `scripts/boat-rudder-update run-request` as a transient unit (see
// updater_unit_name()) - a unit of its own, so stopping this server's
// service doesn't stop it, and a second one can't start while it runs.
// BR_* variables of this server's environment (BR_SERVICE and the like set
// by its unit, test setups) go with it. As with curl, the exit status can't
// be read (main.c reaps every child): with --quiet, systemd-run prints
// nothing unless it failed.
static bool launch_updater(void) {
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)) || !shell_safe(cwd, "/._-")) {
        LOG_WARN("updates: install directory name not usable on a command line");
        return false;
    }
    char unit[96];
    updater_unit_name(unit, sizeof(unit));
    char command[4096];
    int n = snprintf(command, sizeof(command),
                     "systemd-run %s--unit=%s --description='Boat Rudder update' "
                     "--collect --quiet --setenv=BR_INSTALL_DIR='%s'",
                     geteuid() == 0 ? "" : "--user ", unit, cwd);
    extern char **environ;
    for (char **env = environ; *env && n > 0 && (size_t)n < sizeof(command) - 600; env++) {
        if (strncmp(*env, "BR_", 3) != 0 || strncmp(*env, "BR_INSTALL_DIR=", 15) == 0) continue;
        if (!shell_safe(*env, "_=/.:,@+-")) continue;
        n += snprintf(command + n, sizeof(command) - (size_t)n, " --setenv='%s'", *env);
    }
    snprintf(command + n, sizeof(command) - (size_t)n, " '%s/" UPDATER_PATH "' run-request 2>&1", cwd);

    FILE *pipe = popen(command, "r");
    if (!pipe) return false;
    char output[512] = "";
    size_t got = fread(output, 1, sizeof(output) - 1, pipe);
    output[got] = '\0';
    pclose(pipe);
    if (got > 0) {
        output[strcspn(output, "\n")] = '\0';
        LOG_WARN("updates: couldn't start the updater: %s", output);
        return false;
    }
    return true;
}

const char *updates_admin_request(const char *action, const char *version, const char *user_email) {
    if (apply_unavailable_reason()) return "unavailable";
    Progress current;
    read_progress(&current);
    if (current.active) return "busy";

    bool install = strcmp(action, "install") == 0;
    char target[64] = "", date[32];
    if (install) {
        bool known = false;
        pthread_mutex_lock(&cache_lock);
        for (int i = 0; i < cache_count && version; i++)
            if (cache_releases[i].installable && strcmp(cache_releases[i].version, version) == 0)
                known = version_cmp(version, BOAT_RUDDER_VERSION) > 0;
        pthread_mutex_unlock(&cache_lock);
        if (!known) return "version";
        snprintf(target, sizeof(target), "%s", version);
    } else if (strcmp(action, "rollback") != 0 || !rollback_target(target, sizeof(target), date, sizeof(date))) {
        return "rollback";
    }

    char id[32], now_iso[32];
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(now_iso, sizeof(now_iso), "%Y-%m-%dT%H:%M:%SZ", &t);
    snprintf(id, sizeof(id), "%lld", (long long)now);

    bson_t request = BSON_INITIALIZER, progress = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&request, "id", id);
    BSON_APPEND_UTF8(&request, "action", install ? "install" : "rollback");
    BSON_APPEND_UTF8(&request, "version", install ? target : "");
    BSON_APPEND_UTF8(&request, "user", user_email ? user_email : "");
    BSON_APPEND_UTF8(&progress, "id", id);
    BSON_APPEND_UTF8(&progress, "action", install ? "install" : "rollback");
    BSON_APPEND_UTF8(&progress, "version", install ? target : "");
    BSON_APPEND_UTF8(&progress, "from", BOAT_RUDDER_VERSION);
    BSON_APPEND_UTF8(&progress, "user", user_email ? user_email : "");
    BSON_APPEND_UTF8(&progress, "state", "queued");
    BSON_APPEND_UTF8(&progress, "stage", "");
    BSON_APPEND_UTF8(&progress, "message", "");
    BSON_APPEND_UTF8(&progress, "started_at", now_iso);
    BSON_APPEND_UTF8(&progress, "updated_at", now_iso);
    char *request_json  = bson_as_relaxed_extended_json(&request, NULL);
    char *progress_json = bson_as_relaxed_extended_json(&progress, NULL);
    bson_destroy(&request);
    bson_destroy(&progress);

    const char *error = NULL;
    if (!request_json || !progress_json ||
        write_file_atomic(STATE_DIR "/request.json", request_json, 0600) != 0 ||
        write_file_atomic(STATE_DIR "/progress.json", progress_json, 0644) != 0) {
        error = "launch";
    } else if (!launch_updater()) {
        unlink(STATE_DIR "/request.json");
        unlink(STATE_DIR "/progress.json");
        error = "launch";
    } else {
        LOG_INFO("updates: %s %s requested by %s", install ? "install" : "rollback", target, user_email);
    }
    bson_free(request_json);
    bson_free(progress_json);
    return error;
}

char *updates_admin_progress_json(void) {
    Progress p;
    read_progress(&p);
    char *log = read_log_tail();
    bson_t doc = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&doc, "id", p.id);
    BSON_APPEND_UTF8(&doc, "state", p.state);
    BSON_APPEND_UTF8(&doc, "stage", p.stage);
    BSON_APPEND_UTF8(&doc, "message", p.message);
    BSON_APPEND_UTF8(&doc, "log", log ? log : "");
    char *json = bson_as_relaxed_extended_json(&doc, NULL);
    char *copy = json ? strdup(json) : NULL;
    bson_free(json);
    bson_destroy(&doc);
    free(log);
    return copy;
}

// ---- Rendering --------------------------------------------------------------

static const char *const INSTALL_STAGES[]  = { "Package", "Build", "Plan", "Backup", "Apply", NULL };
static const char *const ROLLBACK_STAGES[] = { "Rollback", NULL };

// The run's stages as <li>s: done, current (running or where it failed) or
// pending; a run that rolled itself back gets a last "Rolling back" item.
static char *render_stages(const Progress *p) {
    const char *const *stages = strcmp(p->action, "rollback") == 0 ? ROLLBACK_STAGES : INSTALL_STAGES;
    bool finished_ok = strcmp(p->state, "done") == 0;
    bool rolled_back = strcmp(p->state, "rolled-back") == 0 || strcmp(p->stage, "Rolling back") == 0;
    int current = -1;
    for (int i = 0; stages[i]; i++)
        if (strcmp(stages[i], p->stage) == 0) current = i;
    if (rolled_back) current = 4;   // Apply failed

    char *html = strdup("");
    for (int i = 0; stages[i] && html; i++) {
        const char *mod = finished_ok || i < current ? "done"
                        : i == current ? (p->active ? "current" : "failed") : "pending";
        char *item = render_template("<li class=\"boat-rudder-updates__stage boat-rudder-updates__stage--%s\" "
                                     "data-stage=\"%s\">%s</li>", mod, stages[i], stages[i]);
        html = item ? str_append(html, item) : (free(html), NULL);
        free(item);
    }
    if (html && rolled_back)
        html = str_append(html, "<li class=\"boat-rudder-updates__stage boat-rudder-updates__stage--current\" "
                                "data-stage=\"Rolling back\">Rolling back</li>");
    return html;
}

// The last run's card: live while it runs (html/assets/js/updates.js polls
// it), its result once it's over. "" when there was no run.
static char *render_progress(const char *tpl, const Progress *p) {
    if (strcmp(p->state, "none") == 0) return strdup("");
    char title[192];
    bool rollback = strcmp(p->action, "rollback") == 0;
    if (p->active)
        snprintf(title, sizeof(title), rollback ? "Rolling back..." : "Updating to %s...", p->version);
    else if (strcmp(p->state, "done") == 0)
        snprintf(title, sizeof(title), rollback ? "Rolled back to %s" : "Updated to %s", rollback ? BOAT_RUDDER_VERSION : p->version);
    else if (strcmp(p->state, "rolled-back") == 0)
        snprintf(title, sizeof(title), "The update to %s failed and was undone - %s is running", p->version, p->from);
    else
        snprintf(title, sizeof(title), rollback ? "The rollback failed" : "The update to %s failed", p->version);

    char by[320];
    snprintf(by, sizeof(by), "Requested by %s, %.10s %.5s UTC", p->user[0] ? p->user : "?", p->started_at,
             strlen(p->started_at) > 11 ? p->started_at + 11 : "");
    char *stages  = render_stages(p);
    char *message = html_escape(p->message);
    char *by_html = html_escape(by);
    char *title_h = html_escape(title);
    char *log     = read_log_tail();
    char *log_h   = log ? html_escape(log) : NULL;
    char *html = stages && message && by_html && title_h && log_h
        ? render_template(tpl, p->active ? "active" : p->state, title_h, stages, message, by_html, log_h, p->id,
                          !p->active && strcmp(p->state, "done") != 0 ? " open" : "")
        : NULL;
    free(stages); free(message); free(by_html); free(title_h); free(log); free(log_h);
    return html;
}

// The status card's line and detail; caller holds cache_lock.
static char *render_status(const char *status_tpl, const char *available_tpl, const char *install_tpl,
                           const Progress *progress) {
    long parts[4];
    if (!parse_version(BOAT_RUDDER_VERSION, parts))
        return render_template(status_tpl, "error", "Unknown version",
                               "<p>This build doesn't know its version (no VERSION file when it was compiled).</p>");
    if (cache_error[0]) {
        char *message = html_escape(cache_error);
        char *detail  = message ? render_template("<p>%s</p>", message) : NULL;
        char *html    = detail ? render_template(status_tpl, "error", "Couldn't check for updates", detail) : NULL;
        free(message);
        free(detail);
        return html;
    }

    const Release *newest = NULL;
    for (int i = 0; i < cache_count && !newest; i++)
        if (cache_releases[i].installable) newest = &cache_releases[i];

    if (newest && version_cmp(newest->version, BOAT_RUDDER_VERSION) > 0) {
        const char *unavailable = apply_unavailable_reason();
        char *action;
        if (progress->active) {
            action = strdup("<p class=\"boat-rudder-dashboard__hint\">An update is running - see below.</p>");
        } else if (unavailable) {
            char *reason = html_escape(unavailable);
            action = reason ? render_template("<p class=\"boat-rudder-dashboard__hint\">Can't be installed from "
                                              "here: %s</p>", reason) : NULL;
            free(reason);
        } else {
            action = render_template(install_tpl, newest->version);
        }
        char *detail = action ? render_template(available_tpl, newest->version, action) : NULL;
        char title[96];
        snprintf(title, sizeof(title), "Version %s is available", newest->version);
        char *html = detail ? render_template(status_tpl, "available", title, detail) : NULL;
        free(action);
        free(detail);
        return html;
    }
    if (!newest)
        return render_template(status_tpl, "ok", "No signed releases yet",
                               "<p>Nothing to install: no release has a signed source package.</p>");
    char detail[128];
    snprintf(detail, sizeof(detail), "<p>%s is the newest signed release.</p>", newest->version);
    return render_template(status_tpl, "ok",
                           version_cmp(newest->version, BOAT_RUDDER_VERSION) == 0
                               ? "Up to date" : "Newer than every release", detail);
}

static const char *error_message(const char *code) {
    if (!code) return NULL;
    if (strcmp(code, "password") == 0)    return "Wrong password - nothing was started.";
    if (strcmp(code, "busy") == 0)        return "An update or rollback is already running.";
    if (strcmp(code, "version") == 0)     return "That version isn't a signed release newer than the installed one. Check again and retry.";
    if (strcmp(code, "rollback") == 0)    return "There is no update to roll back.";
    if (strcmp(code, "unavailable") == 0) return "This server can't update itself - see the reason above.";
    if (strcmp(code, "launch") == 0)      return "The updater couldn't be started - see the server log.";
    return NULL;
}

char *updates_admin_page(int epoch, const char *error_code) {
    pthread_mutex_lock(&cache_lock);
    bool stale = cache_checked_at == 0 || time(NULL) - cache_checked_at > CHECK_MAX_AGE_SECS;
    pthread_mutex_unlock(&cache_lock);
    if (stale) refresh_cache();

    char *page_tpl      = load_template("dashboard/settings/updates_epoch%d.html", epoch);
    char *status_tpl    = load_template("dashboard/settings/updates-status_epoch%d.html", epoch);
    char *available_tpl = load_template("dashboard/settings/updates-available_epoch%d.html", epoch);
    char *install_tpl   = load_template("dashboard/settings/updates-install_epoch%d.html", epoch);
    char *rollback_tpl  = load_template("dashboard/settings/updates-rollback_epoch%d.html", epoch);
    char *progress_tpl  = load_template("dashboard/settings/updates-progress_epoch%d.html", epoch);
    char *release_tpl   = load_template("dashboard/settings/updates-release_epoch%d.html", epoch);
    char *error_tpl     = load_template("dashboard/settings/settings-error_epoch%d.html", epoch);
    char *result = NULL, *status = NULL, *releases = NULL, *progress_html = NULL;
    char *rollback_html = NULL, *error_html = NULL;
    if (!page_tpl || !status_tpl || !available_tpl || !install_tpl || !rollback_tpl ||
        !progress_tpl || !release_tpl || !error_tpl) goto cleanup;

    Progress progress;
    read_progress(&progress);
    progress_html = render_progress(progress_tpl, &progress);

    char from[64], date[32];
    if (!progress.active && !apply_unavailable_reason() && rollback_target(from, sizeof(from), date, sizeof(date)))
        rollback_html = render_template(rollback_tpl, BOAT_RUDDER_VERSION, from, date);
    else
        rollback_html = strdup("");

    const char *message = error_message(error_code);
    error_html = message ? render_template(error_tpl, message) : strdup("");

    pthread_mutex_lock(&cache_lock);
    status   = render_status(status_tpl, available_tpl, install_tpl, &progress);
    releases = strdup("");
    for (int i = 0; i < cache_count && releases; i++) {
        char *item = render_release(release_tpl, &cache_releases[i]);
        releases = item ? str_append(releases, item) : (free(releases), NULL);
        free(item);
    }
    if (releases && cache_count == 0 && !cache_error[0])
        releases = str_append(releases, "<p class=\"boat-rudder-dashboard__hint\">No releases published yet.</p>");

    char checked[64];
    struct tm t;
    gmtime_r(&cache_checked_at, &t);
    strftime(checked, sizeof(checked), "Checked %Y-%m-%d %H:%M UTC", &t);
    pthread_mutex_unlock(&cache_lock);

    if (status && releases && progress_html && rollback_html && error_html)
        result = render_template(page_tpl, BOAT_RUDDER_VERSION, status, checked, update_repo, releases,
                                 error_html, progress_html, rollback_html);

cleanup:
    free(page_tpl);
    free(status_tpl);
    free(available_tpl);
    free(install_tpl);
    free(rollback_tpl);
    free(progress_tpl);
    free(release_tpl);
    free(error_tpl);
    free(status);
    free(releases);
    free(progress_html);
    free(rollback_html);
    free(error_html);
    return result;
}
