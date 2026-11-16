#include "cms_category_ranking.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENTRIES_COLLECTION       "entries"
#define ENTRY_VISITS_COLLECTION  "entry_visits_daily"

typedef struct {
    char    id[25];  // hex ObjectId
    int64_t visits;
} Ranked;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Ranked *g_ranked = NULL;   // most visited first; guarded by g_lock
static size_t  g_ranked_count = 0;

static pthread_t       g_thread;
static atomic_int      g_running = 0;
static pthread_mutex_t g_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;

// Visits per blog post (its link) since `since` ("YYYY-MM-DD"), as
// {slug: total}. NULL if mongodb isn't ready or the query failed.
static bson_t *post_visits(const char *since) {
    bson_t *pipeline = BCON_NEW(
        "0", "{", "$match", "{", "entry_type", BCON_UTF8("blog"), "date", "{", "$gte", BCON_UTF8(since), "}", "}", "}",
        "1", "{", "$group", "{", "_id", BCON_UTF8("$slug"), "total", "{", "$sum", BCON_UTF8("$total"), "}", "}", "}");
    mongoc_cursor_t *cursor = mongodb_manager_aggregate(ENTRY_VISITS_COLLECTION, pipeline);
    if (!cursor) { bson_destroy(pipeline); return NULL; }

    bson_t *visits = bson_new();
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t id, total;
        if (bson_iter_init_find(&id, doc, "_id") && BSON_ITER_HOLDS_UTF8(&id) &&
            bson_iter_init_find(&total, doc, "total") && BSON_ITER_HOLDS_NUMBER(&total))
            bson_append_int64(visits, bson_iter_utf8(&id, NULL), -1, bson_iter_as_int64(&total));
    }
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("category ranking: visits query failed: %s", error.message);
        bson_destroy(visits);
        visits = NULL;
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(pipeline);
    return visits;
}

static int by_visits(const void *a, const void *b) {
    const Ranked *x = a, *y = b;
    return (x->visits < y->visits) - (x->visits > y->visits);
}

// Adds `visits` to category `id` in ranked[0..*count), appending it if new.
static Ranked *add_visits(Ranked *ranked, size_t *count, size_t *cap, const char *id, int64_t visits) {
    for (size_t i = 0; i < *count; i++)
        if (strcmp(ranked[i].id, id) == 0) { ranked[i].visits += visits; return ranked; }
    if (*count == *cap) {
        size_t grown_cap = *cap ? *cap * 2 : 32;
        Ranked *grown = realloc(ranked, grown_cap * sizeof(Ranked));
        if (!grown) return ranked;
        ranked = grown;
        *cap = grown_cap;
    }
    snprintf(ranked[*count].id, sizeof(ranked[*count].id), "%s", id);
    ranked[*count].visits = visits;
    (*count)++;
    return ranked;
}

void category_ranking_refresh(void) {
    time_t since_t = time(NULL) - (time_t)CATEGORY_RANKING_DAYS * 24 * 60 * 60;
    struct tm tm;
    gmtime_r(&since_t, &tm);
    char since[16];
    strftime(since, sizeof(since), "%Y-%m-%d", &tm); // analytics' day keys are UTC too

    bson_t *visits = post_visits(since);
    if (!visits) return;

    mongoc_collection_t *col = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!col) { bson_destroy(visits); return; }
    bson_t *query = BCON_NEW("type", BCON_UTF8("blog"), "enabled", BCON_BOOL(true));
    bson_t *opts = BCON_NEW("projection", "{", "link", BCON_INT32(1), "categories", BCON_INT32(1), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, query, opts, NULL);

    Ranked *ranked = NULL;
    size_t count = 0, cap = 0;
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t link, post, cats, cat;
        if (!bson_iter_init_find(&link, doc, "link") || !BSON_ITER_HOLDS_UTF8(&link)) continue;
        if (!bson_iter_init_find(&post, visits, bson_iter_utf8(&link, NULL))) continue;
        int64_t n = bson_iter_as_int64(&post);
        if (n <= 0 || !bson_iter_init_find(&cats, doc, "categories") || !BSON_ITER_HOLDS_ARRAY(&cats) ||
            !bson_iter_recurse(&cats, &cat))
            continue;
        while (bson_iter_next(&cat)) {
            if (!BSON_ITER_HOLDS_OID(&cat)) continue;
            char id[25];
            bson_oid_to_string(bson_iter_oid(&cat), id);
            ranked = add_visits(ranked, &count, &cap, id, n);
        }
    }
    bson_error_t error;
    int failed = mongoc_cursor_error(cursor, &error);
    if (failed) LOG_ERROR("category ranking: entries query failed: %s", error.message);
    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    bson_destroy(opts);
    mongoc_collection_destroy(col);
    bson_destroy(visits);
    if (failed) { free(ranked); return; } // keep the last good ranking

    if (count) qsort(ranked, count, sizeof(Ranked), by_visits);
    pthread_mutex_lock(&g_lock);
    free(g_ranked);
    g_ranked = ranked;
    g_ranked_count = count;
    pthread_mutex_unlock(&g_lock);
    LOG_INFO("category ranking: %zu categories with visits in the last %d days", count, CATEGORY_RANKING_DAYS);
}

typedef struct {
    CmsCategoryItem item;
    int64_t visits;
    size_t  position; // the order it came in, for ties
} Sortable;

static int by_rank(const void *a, const void *b) {
    const Sortable *x = a, *y = b;
    if (x->visits != y->visits) return x->visits > y->visits ? -1 : 1;
    return (x->position > y->position) - (x->position < y->position);
}

void category_ranking_sort(CmsCategoryItem *cats, size_t count) {
    if (count < 2) return;
    Sortable *s = malloc(count * sizeof(Sortable));
    if (!s) return;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < count; i++) {
        s[i] = (Sortable){ .item = cats[i], .position = i };
        for (size_t r = 0; r < g_ranked_count; r++)
            if (cats[i].id && strcmp(g_ranked[r].id, cats[i].id) == 0) { s[i].visits = g_ranked[r].visits; break; }
    }
    pthread_mutex_unlock(&g_lock);
    qsort(s, count, sizeof(Sortable), by_rank);
    for (size_t i = 0; i < count; i++) cats[i] = s[i].item;
    free(s);
}

static void *refresher(void *arg) {
    (void)arg;
    while (atomic_load(&g_running)) {
        category_ranking_refresh();
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += CATEGORY_RANKING_INTERVAL_SECS;
        pthread_mutex_lock(&g_wake_lock);
        if (atomic_load(&g_running)) pthread_cond_timedwait(&g_wake, &g_wake_lock, &until);
        pthread_mutex_unlock(&g_wake_lock);
    }
    return NULL;
}

int category_ranking_start(void) {
    if (!mongodb_manager_is_ready()) return 0;
    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, refresher, NULL) != 0) {
        atomic_store(&g_running, 0);
        LOG_ERROR("category ranking: thread could not start");
        return -1;
    }
    return 0;
}

void category_ranking_stop(void) {
    if (!atomic_exchange(&g_running, 0)) return;
    pthread_mutex_lock(&g_wake_lock);
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_wake_lock);
    pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_lock);
    free(g_ranked);
    g_ranked = NULL;
    g_ranked_count = 0;
    pthread_mutex_unlock(&g_lock);
}
