#include "mailer.h"
#include "config_loader.h"
#include "log.h"
#include "../db/cms_site_settings.h"
#include "../db/mongodb_manager.h"
#include <bson/bson.h>
#include <curl/curl.h>
#include <mongoc/mongoc.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAIL_QUEUE_COLLECTION "mail_queue"
#define CONNECT_TIMEOUT_SECS  15
#define SEND_TIMEOUT_SECS     30

// Minutes to wait after the 1st, 2nd... failed attempt; after the last one
// the message is marked failed.
static const int RETRY_MINUTES[] = { 1, 5, 15, 60 };
#define MAX_ATTEMPTS ((int)(sizeof(RETRY_MINUTES) / sizeof(RETRY_MINUTES[0])) + 1)

static pthread_t       g_thread;
static atomic_int      g_running;
static pthread_mutex_t g_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;

int mailer_configured(void) {
    return mail_smtp_url[0] && mail_from[0];
}

static int has_control(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1;
    return 0;
}

int mailer_valid_address(const char *a) {
    size_t n = a ? strlen(a) : 0;
    if (n < 3 || n > 254 || has_control(a)) return 0;
    const char *at = strchr(a, '@');
    if (!at || at == a || strchr(at + 1, '@') || !at[1]) return 0;
    const char *dot = strrchr(at + 1, '.');
    if (!dot || dot == at + 1 || !dot[1]) return 0;
    for (const char *p = a; *p; p++)
        if (*p == ' ' || *p == '"' || *p == '<' || *p == '>' || *p == '(' || *p == ')' ||
            *p == ',' || *p == ';' || *p == ':' || *p == '\\' || *p == '[' || *p == ']')
            return 0;
    return 1;
}

int mailer_enqueue(const char *to, const char *subject, const char *text) {
    if (!mailer_configured() || !mailer_valid_address(to) || !subject || has_control(subject) ||
        strlen(subject) > 200 || !text)
        return -1;
    mongoc_collection_t *col = mongodb_manager_get_collection(MAIL_QUEUE_COLLECTION);
    if (!col) return -1;

    int64_t now = (int64_t)time(NULL) * 1000;
    bson_t *doc = BCON_NEW("to", BCON_UTF8(to), "subject", BCON_UTF8(subject), "text", BCON_UTF8(text),
                           "status", BCON_UTF8("queued"), "attempts", BCON_INT32(0),
                           "created_at", BCON_DATE_TIME(now), "next_attempt_at", BCON_DATE_TIME(now));
    bson_error_t error;
    int rc = mongoc_collection_insert_one(col, doc, NULL, NULL, &error) ? 0 : -1;
    if (rc != 0) LOG_ERROR("mailer_enqueue: %s", error.message);
    bson_destroy(doc);
    mongoc_collection_destroy(col);

    if (rc == 0) {
        pthread_mutex_lock(&g_wake_lock);
        pthread_cond_signal(&g_wake);
        pthread_mutex_unlock(&g_wake_lock);
    }
    return rc;
}

// ---- Message ------------------------------------------------------------------

// RFC 2047 "encoded word" when `s` isn't plain ASCII, else a copy.
static char *header_text(const char *s) {
    int ascii = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p >= 0x80) ascii = 0;
    if (ascii) return strdup(s);
    size_t blen = sodium_base64_encoded_len(strlen(s), sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(blen);
    char *out = b64 ? malloc(blen + 16) : NULL;
    if (out) {
        sodium_bin2base64(b64, blen, (const unsigned char *)s, strlen(s), sodium_base64_VARIANT_ORIGINAL);
        snprintf(out, blen + 16, "=?UTF-8?B?%s?=", b64);
    }
    free(b64);
    return out;
}

// The whole RFC 5322 message, CRLF line ends. Returns a malloc'd string.
static char *build_message(const char *to, const char *subject, const char *text) {
    char *site = cms_get_site_name();
    char *from_name = header_text(mail_from_name[0] ? mail_from_name : (site ? site : "Boat Rudder"));
    char *subj = header_text(subject);
    free(site);

    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);

    unsigned char rnd[12];
    char id[25];
    randombytes_buf(rnd, sizeof(rnd));
    sodium_bin2hex(id, sizeof(id), rnd, sizeof(rnd));
    const char *domain = strchr(mail_from, '@');

    // Body: every line break as CRLF.
    size_t n = strlen(text);
    char *body = malloc(n * 2 + 3);
    char *w = body;
    for (const char *p = text; body && *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') *w++ = '\r';
        *w++ = *p;
    }
    if (body) { *w++ = '\r'; *w++ = '\n'; *w = '\0'; }

    char *msg = NULL;
    if (from_name && subj && body) {
        size_t cap = strlen(from_name) + strlen(subj) + strlen(body) + strlen(to) + 512 + strlen(mail_from) * 2;
        msg = malloc(cap);
        if (msg)
            snprintf(msg, cap,
                     "Date: %s\r\n"
                     "From: %s <%s>\r\n"
                     "To: <%s>\r\n"
                     "Subject: %s\r\n"
                     "Message-ID: <%s%s>\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "Content-Transfer-Encoding: 8bit\r\n"
                     "\r\n"
                     "%s",
                     date, from_name, mail_from, to, subj, id, domain ? domain : "@localhost", body);
    }
    free(from_name);
    free(subj);
    free(body);
    return msg;
}

typedef struct {
    const char *data;
    size_t      left;
} Payload;

static size_t read_payload(char *buf, size_t size, size_t nmemb, void *userp) {
    Payload *p = userp;
    size_t n = size * nmemb < p->left ? size * nmemb : p->left;
    memcpy(buf, p->data, n);
    p->data += n;
    p->left -= n;
    return n;
}

// TLS is required, except to a relay on this machine (smtp://localhost).
static int loopback_url(const char *url) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    return strncasecmp(h, "localhost", 9) == 0 || strncmp(h, "127.", 4) == 0 || strncmp(h, "[::1]", 5) == 0;
}

// Sends one message. Returns 0, or -1 with `error` filled.
static int smtp_send(const char *to, const char *subject, const char *text, char *error, size_t error_size) {
    char *msg = build_message(to, subject, text);
    if (!msg) { snprintf(error, error_size, "out of memory"); return -1; }

    CURL *curl = curl_easy_init();
    if (!curl) { free(msg); snprintf(error, error_size, "curl_easy_init failed"); return -1; }

    char from[300], rcpt[300];
    snprintf(from, sizeof(from), "<%s>", mail_from);
    snprintf(rcpt, sizeof(rcpt), "<%s>", to);
    struct curl_slist *rcpts = curl_slist_append(NULL, rcpt);
    Payload payload = { msg, strlen(msg) };
    char curl_error[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(curl, CURLOPT_URL, mail_smtp_url);
    if (mail_smtp_user[0]) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, mail_smtp_user);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, mail_smtp_password);
    }
    curl_easy_setopt(curl, CURLOPT_USE_SSL, loopback_url(mail_smtp_url) ? (long)CURLUSESSL_TRY : (long)CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpts);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_payload);
    curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)SEND_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        snprintf(error, error_size, "%s", curl_error[0] ? curl_error : curl_easy_strerror(res));

    curl_slist_free_all(rcpts);
    curl_easy_cleanup(curl);
    free(msg);
    return res == CURLE_OK ? 0 : -1;
}

// ---- Sender thread --------------------------------------------------------------

// Claims the next due message (status queued -> sending) and sends it.
// Returns 1 if one was handled, 0 if none was due.
static int send_next(void) {
    mongoc_collection_t *col = mongodb_manager_get_collection(MAIL_QUEUE_COLLECTION);
    if (!col) return 0;

    int64_t now = (int64_t)time(NULL) * 1000;
    bson_t *query = BCON_NEW("status", BCON_UTF8("queued"), "next_attempt_at", "{", "$lte", BCON_DATE_TIME(now), "}");
    bson_t *sort = BCON_NEW("next_attempt_at", BCON_INT32(1));
    bson_t *update = BCON_NEW("$set", "{", "status", BCON_UTF8("sending"), "}", "$inc", "{", "attempts", BCON_INT32(1), "}");
    bson_t reply;
    bson_error_t error;
    int handled = 0;

    if (mongoc_collection_find_and_modify(col, query, sort, update, NULL, false, false, true, &reply, &error)) {
        bson_iter_t it, v;
        if (bson_iter_init_find(&it, &reply, "value") && BSON_ITER_HOLDS_DOCUMENT(&it)) {
            uint32_t len;
            const uint8_t *data;
            bson_iter_document(&it, &len, &data);
            bson_t doc;
            bson_init_static(&doc, data, len);

            const char *to = "", *subject = "", *text = "";
            int attempts = 1;
            bson_oid_t oid;
            if (bson_iter_init_find(&v, &doc, "_id")) bson_oid_copy(bson_iter_oid(&v), &oid);
            if (bson_iter_init_find(&v, &doc, "to") && BSON_ITER_HOLDS_UTF8(&v)) to = bson_iter_utf8(&v, NULL);
            if (bson_iter_init_find(&v, &doc, "subject") && BSON_ITER_HOLDS_UTF8(&v)) subject = bson_iter_utf8(&v, NULL);
            if (bson_iter_init_find(&v, &doc, "text") && BSON_ITER_HOLDS_UTF8(&v)) text = bson_iter_utf8(&v, NULL);
            if (bson_iter_init_find(&v, &doc, "attempts") && BSON_ITER_HOLDS_INT32(&v)) attempts = bson_iter_int32(&v);

            char send_error[CURL_ERROR_SIZE + 64] = "";
            int ok = smtp_send(to, subject, text, send_error, sizeof(send_error)) == 0;
            int64_t later = (int64_t)time(NULL) * 1000;
            bson_t *done = BCON_NEW("_id", BCON_OID(&oid));
            bson_t *set;
            if (ok) {
                set = BCON_NEW("$set", "{", "status", BCON_UTF8("sent"), "sent_at", BCON_DATE_TIME(later), "}");
                LOG_INFO("Mail sent to %s: %s", to, subject);
            } else if (attempts >= MAX_ATTEMPTS) {
                set = BCON_NEW("$set", "{", "status", BCON_UTF8("failed"), "last_error", BCON_UTF8(send_error), "}");
                LOG_ERROR("Mail to %s failed for good: %s", to, send_error);
            } else {
                int64_t next = later + (int64_t)RETRY_MINUTES[attempts - 1] * 60 * 1000;
                set = BCON_NEW("$set", "{", "status", BCON_UTF8("queued"), "next_attempt_at", BCON_DATE_TIME(next),
                               "last_error", BCON_UTF8(send_error), "}");
                LOG_WARN("Mail to %s not sent (attempt %d): %s", to, attempts, send_error);
            }
            if (!mongoc_collection_update_one(col, done, set, NULL, NULL, &error))
                LOG_ERROR("mailer: %s", error.message);
            bson_destroy(done);
            bson_destroy(set);
            handled = 1;
        }
        bson_destroy(&reply);
    } else {
        LOG_ERROR("mailer: %s", error.message);
    }

    bson_destroy(query);
    bson_destroy(sort);
    bson_destroy(update);
    mongoc_collection_destroy(col);
    return handled;
}

// A message left "sending" by a server that stopped mid-send goes back to
// the queue (it may be sent twice; better than never).
static void requeue_interrupted(void) {
    mongoc_collection_t *col = mongodb_manager_get_collection(MAIL_QUEUE_COLLECTION);
    if (!col) return;
    bson_t *query = BCON_NEW("status", BCON_UTF8("sending"));
    bson_t *update = BCON_NEW("$set", "{", "status", BCON_UTF8("queued"), "}");
    bson_t *index = BCON_NEW("status", BCON_INT32(1), "next_attempt_at", BCON_INT32(1));
    mongoc_index_model_t *model = mongoc_index_model_new(index, NULL);
    mongoc_collection_update_many(col, query, update, NULL, NULL, NULL);
    mongoc_collection_create_indexes_with_opts(col, &model, 1, NULL, NULL, NULL);
    mongoc_index_model_destroy(model);
    bson_destroy(index);
    bson_destroy(query);
    bson_destroy(update);
    mongoc_collection_destroy(col);
}

static void *sender(void *arg) {
    (void)arg;
    requeue_interrupted();
    while (atomic_load(&g_running)) {
        while (atomic_load(&g_running) && send_next()) {}

        // Sleep until woken by mailer_enqueue() or 30 s pass (a retry may
        // come due), whichever is first.
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 30;
        pthread_mutex_lock(&g_wake_lock);
        if (atomic_load(&g_running)) pthread_cond_timedwait(&g_wake, &g_wake_lock, &until);
        pthread_mutex_unlock(&g_wake_lock);
    }
    return NULL;
}

int mailer_start(void) {
    if (!mailer_configured() || !mongodb_manager_is_ready()) return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        LOG_ERROR("mailer: curl_global_init failed");
        return -1;
    }
    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, sender, NULL) != 0) {
        atomic_store(&g_running, 0);
        LOG_ERROR("mailer: sender thread could not start");
        return -1;
    }
    LOG_INFO("Mail           : %s, from %s", mail_smtp_url, mail_from);
    return 0;
}

void mailer_stop(void) {
    if (!atomic_exchange(&g_running, 0)) return;
    pthread_mutex_lock(&g_wake_lock);
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_wake_lock);
    pthread_join(g_thread, NULL);
    curl_global_cleanup();
}
