#include "analytics_view.h"
#include "analytics_charts.h"
#include "country_continent.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VISITS_DAILY_COLLECTION       "page_visits_daily"
#define ENTRY_VISITS_DAILY_COLLECTION "entry_visits_daily"

// One tallied key ("Chrome/124", "Windows/10", "US", ...) and its running
// count, for the browser/OS/country/route breakdowns - each day-bucket
// document in the queried range gets merged into one of these lists.
typedef struct {
    char key[128];
    int count;
} KV;

// Heap-grown list of KV - unbounded, since an "All Time" query easily sees
// several hundred distinct browser/OS keys (every version and every bot UA
// is its own key), and a fixed cap silently dropped whichever came last.
typedef struct {
    KV *items;
    int count, cap;
} KVList;

typedef struct {
    int total, epochwml, epoch0, epoch1, epoch2, epoch3;
    KVList browsers, oses, countries, routes;
} AnalyticsData;

static void kv_increment(KVList *list, const char *key, int delta) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].key, key) == 0) { list->items[i].count += delta; return; }
    }
    if (list->count == list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 32;
        KV *grown = realloc(list->items, (size_t)new_cap * sizeof(KV));
        if (!grown) return;
        list->items = grown;
        list->cap = new_cap;
    }
    snprintf(list->items[list->count].key, sizeof(list->items[list->count].key), "%s", key);
    list->items[list->count].count = delta;
    list->count++;
}

static void kv_free(KVList *list) {
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

static int kv_cmp_count_desc(const void *a, const void *b) {
    const KV *x = a, *y = b;
    if (x->count != y->count) return y->count > x->count ? 1 : -1;
    return strcmp(x->key, y->key);
}

static void kv_sort(KVList *list) {
    if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof(KV), kv_cmp_count_desc);
}

// Merges every field of one page_visits_daily document into `d` - `total`,
// `by_epoch`'s five named counters, and the three open-ended by_browser/
// by_os/by_country subdocuments (walked generically, since their field
// names are the dynamic keys analytics.c wrote) plus by_route's five.
static void merge_daily_doc(const bson_t *doc, AnalyticsData *d) {
    bson_iter_t iter, sub;

    if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
        d->total += bson_iter_int32(&iter);

    if (bson_iter_init_find(&iter, doc, "by_epoch") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub)) {
            if (!BSON_ITER_HOLDS_INT32(&sub)) continue;
            const char *key = bson_iter_key(&sub);
            int v = bson_iter_int32(&sub);
            if (strcmp(key, "epochwml") == 0) d->epochwml += v;
            else if (strcmp(key, "epoch0") == 0) d->epoch0 += v;
            else if (strcmp(key, "epoch1") == 0) d->epoch1 += v;
            else if (strcmp(key, "epoch2") == 0) d->epoch2 += v;
            else if (strcmp(key, "epoch3") == 0) d->epoch3 += v;
        }
    }

    if (bson_iter_init_find(&iter, doc, "by_browser") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(&d->browsers, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_os") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(&d->oses, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_country") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(&d->countries, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_route") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(&d->routes, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
}

// Builds the {filter} shared by query_page_visits()/query_top_articles():
// day -> exact key match; week/month -> {year, <week|month>}; year -> just
// {year}; range -> a "gte/lte" bound; all -> {} (everything). `id_field` is
// "_id" for page_visits_daily (whose _id IS the "YYYY-MM-DD" key, so range
// queries can compare it directly - lexicographic order matches
// chronological order for that format) and "date" for entry_visits_daily
// (whose _id is "<type>__<slug>__<date>", not orderable the same way).
static bson_t *build_period_filter(const char *id_field, const char *period,
                                    int year, int month, int week,
                                    const char *date, const char *from, const char *to) {
    if (strcmp(period, "day") == 0)
        return BCON_NEW(id_field, BCON_UTF8(date));
    if (strcmp(period, "week") == 0)
        return BCON_NEW("year", BCON_INT32(year), "week", BCON_INT32(week));
    if (strcmp(period, "year") == 0)
        return BCON_NEW("year", BCON_INT32(year));
    if (strcmp(period, "all") == 0)
        return bson_new();
    if (strcmp(period, "range") == 0)
        return BCON_NEW(id_field, "{", "$gte", BCON_UTF8(from), "$lte", BCON_UTF8(to), "}");
    // "month" - also the fallback for any unrecognized value.
    return BCON_NEW("year", BCON_INT32(year), "month", BCON_INT32(month));
}

static void query_page_visits(AnalyticsData *d, const char *period, int year, int month,
                               int week, const char *date, const char *from, const char *to) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(VISITS_DAILY_COLLECTION);
    if (!collection) return;

    bson_t *filter = build_period_filter("_id", period, year, month, week, date, from, to);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, NULL, NULL);

    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) merge_daily_doc(doc, d);

    bson_destroy(filter);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
}

// Top 20 articles/pages/categories by total visits in the period - a
// separate query (not derivable from page_visits_daily) since that
// collection has no per-slug breakdown at all.
static char *build_top_articles_table(const char *period, int year, int month, int week,
                                       const char *date, const char *from, const char *to) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRY_VISITS_DAILY_COLLECTION);
    if (!collection) return strdup("<p>No data</p>");

    bson_t *filter = build_period_filter("date", period, year, month, week, date, from, to);
    bson_t *opts = BCON_NEW(
        "sort", "{", "total", BCON_INT32(-1), "}",
        "limit", BCON_INT64((int64_t)20)
    );

    // Merge per-slug across the queried range the same way page_visits_daily
    // gets merged - a slug can have one bucket document per day.
    KVList articles = {0};

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, NULL, NULL);
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        char label[160] = "";
        const char *entry_type = "", *slug = "";
        if (bson_iter_init_find(&iter, doc, "entry_type") && BSON_ITER_HOLDS_UTF8(&iter))
            entry_type = bson_iter_utf8(&iter, NULL);
        if (bson_iter_init_find(&iter, doc, "slug") && BSON_ITER_HOLDS_UTF8(&iter))
            slug = bson_iter_utf8(&iter, NULL);
        snprintf(label, sizeof(label), "[%s] %s", entry_type, slug);

        int total = 0;
        if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
            total = bson_iter_int32(&iter);

        kv_increment(&articles, label, total);
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    bson_destroy(filter);
    bson_destroy(opts);

    kv_sort(&articles);
    int article_count = articles.count > 20 ? 20 : articles.count;

    if (article_count == 0) { kv_free(&articles); return strdup("<p>No data</p>"); }

    char *rows = strdup("");
    for (int i = 0; rows && i < article_count; i++) {
        char *label_enc = malloc(strlen(articles.items[i].key) * 6 + 1);
        if (!label_enc) { free(rows); rows = NULL; break; }
        html_encode(label_enc, articles.items[i].key, strlen(articles.items[i].key) * 6 + 1);

        char *row = render_template(
            "<tr><td>%s</td><td>%d</td></tr>", label_enc, articles.items[i].count);
        free(label_enc);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    kv_free(&articles);
    if (!rows) return strdup("<p>No data</p>");

    char *table = render_template(
        "<table class=\"boat-rudder-dashboard__table\">"
        "<thead><tr><th>Article</th><th>Visits</th></tr></thead><tbody>%s</tbody></table>",
        rows);
    free(rows);
    return table ? table : strdup("<p>No data</p>");
}

// One breakdown table (Browsers/OSes/Countries/Routes): a name column and a
// bar built from a CSS width percentage next to the count, widest entry
// first (`list` is already sorted by kv_sort() before this is called).
static char *build_kv_table(const KVList *list, int total) {
    const KV *arr = list->items;
    int count = list->count;
    if (count == 0) return strdup("<p class=\"boat-rudder-empty\">No data</p>");

    int max_count = arr[0].count > 0 ? arr[0].count : 1;

    char *rows = strdup("");
    for (int i = 0; rows && i < count; i++) {
        int bar_w = max_count > 0 ? arr[i].count * 100 / max_count : 0;
        int pct = total > 0 ? arr[i].count * 100 / total : 0;

        char *key_enc = malloc(strlen(arr[i].key) * 6 + 1);
        if (!key_enc) { free(rows); rows = NULL; break; }
        html_encode(key_enc, arr[i].key, strlen(arr[i].key) * 6 + 1);

        char *row = render_template(
            "<tr><td>%s<span class=\"boat-rudder-analytics__bar\" style=\"width:%dpx\"></span></td>"
            "<td>%d <span class=\"boat-rudder-analytics__pct\">(%d%%)</span></td></tr>",
            key_enc, bar_w, arr[i].count, pct);
        free(key_enc);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) return strdup("<p class=\"boat-rudder-empty\">No data</p>");

    char *table = render_template("<table class=\"boat-rudder-analytics__kv-table\">%s</table>", rows);
    free(rows);
    return table ? table : strdup("<p class=\"boat-rudder-empty\">No data</p>");
}

// "Chrome/124" -> family "Chrome" (length up to the first '/'); a key with
// no '/' ("Unknown", "Cello", most bot UAs) is its own family.
static size_t family_len(const char *key) {
    return strcspn(key, "/");
}

// Orders keys by family name first, then by count (desc) within the family,
// so each family's versions end up as one contiguous, already-ranked run.
static int kv_cmp_family(const void *a, const void *b) {
    const KV *x = a, *y = b;
    size_t lx = family_len(x->key), ly = family_len(y->key);
    size_t n = lx < ly ? lx : ly;
    int c = strncmp(x->key, y->key, n);
    if (c != 0) return c;
    if (lx != ly) return lx < ly ? -1 : 1;
    return kv_cmp_count_desc(a, b);
}

typedef struct {
    int start, len, count;
} KVGroup;

static int kv_group_cmp(const void *a, const void *b) {
    const KVGroup *x = a, *y = b;
    return y->count > x->count ? 1 : (y->count < x->count ? -1 : 0);
}

static char *html_encode_dup(const char *src, size_t len) {
    char raw[128];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, src, len);
    raw[len] = '\0';
    char *enc = malloc(len * 6 + 1);
    if (enc) html_encode(enc, raw, len * 6 + 1);
    return enc;
}

// Browsers/OSes breakdown as a two-level tree: one <details> per family
// ("Chrome", "Windows", "Europe", ...) with the family's summed count and bar, which
// expands to that family's versions. A family with a single, version-less
// key ("Unknown") renders as a plain, non-expandable row. `child_unit`
// labels the child count next to the family name ("ver.", "countries"). Native
// <details>/<summary>, so no JavaScript is needed to expand/collapse.
static char *build_grouped_kv_table(const KVList *list, int total, const char *child_unit) {
    const char *empty = "<p class=\"boat-rudder-empty\">No data</p>";
    if (list->count == 0) return strdup(empty);

    KV *items = malloc((size_t)list->count * sizeof(KV));
    KVGroup *groups = malloc((size_t)list->count * sizeof(KVGroup));
    if (!items || !groups) { free(items); free(groups); return strdup(empty); }
    memcpy(items, list->items, (size_t)list->count * sizeof(KV));
    qsort(items, (size_t)list->count, sizeof(KV), kv_cmp_family);

    int group_count = 0;
    for (int i = 0; i < list->count; i++) {
        KVGroup *g = group_count > 0 ? &groups[group_count - 1] : NULL;
        size_t fl = family_len(items[i].key);
        if (g && family_len(items[g->start].key) == fl &&
            strncmp(items[g->start].key, items[i].key, fl) == 0) {
            g->len++;
            g->count += items[i].count;
        } else {
            groups[group_count++] = (KVGroup){ i, 1, items[i].count };
        }
    }
    qsort(groups, (size_t)group_count, sizeof(KVGroup), kv_group_cmp);

    int max_group = groups[0].count > 0 ? groups[0].count : 1;

    char *html = strdup("<div class=\"boat-rudder-analytics__tree\">");
    for (int gi = 0; html && gi < group_count; gi++) {
        const KVGroup *g = &groups[gi];
        const KV *first = &items[g->start];
        size_t fl = family_len(first->key);
        int bar_w = g->count * 100 / max_group;
        int pct = total > 0 ? g->count * 100 / total : 0;

        char *name_enc = html_encode_dup(first->key, fl);
        if (!name_enc) { free(html); html = NULL; break; }

        char *block;
        if (g->len == 1 && first->key[fl] == '\0') {
            block = render_template(
                "<div class=\"boat-rudder-analytics__tree-row boat-rudder-analytics__tree-row--leaf\">"
                "<span class=\"boat-rudder-analytics__tree-name\">%s<span class=\"boat-rudder-analytics__bar\" style=\"width:%dpx\"></span></span>"
                "<span class=\"boat-rudder-analytics__tree-count\">%d <span class=\"boat-rudder-analytics__pct\">(%d%%)</span></span>"
                "</div>",
                name_enc, bar_w, g->count, pct);
        } else {
            int max_ver = first->count > 0 ? first->count : 1;
            char *rows = strdup("");
            for (int i = g->start; rows && i < g->start + g->len; i++) {
                const char *ver = items[i].key + fl;
                if (*ver == '/') ver++;
                char *ver_enc = html_encode_dup(*ver ? ver : "(no version)", strlen(*ver ? ver : "(no version)"));
                if (!ver_enc) { free(rows); rows = NULL; break; }
                char *row = render_template(
                    "<tr><td>%s<span class=\"boat-rudder-analytics__bar\" style=\"width:%dpx\"></span></td>"
                    "<td>%d <span class=\"boat-rudder-analytics__pct\">(%d%%)</span></td></tr>",
                    ver_enc, items[i].count * 100 / max_ver, items[i].count,
                    total > 0 ? items[i].count * 100 / total : 0);
                free(ver_enc);
                rows = row ? str_append(rows, row) : NULL;
                free(row);
            }
            block = rows ? render_template(
                "<details class=\"boat-rudder-analytics__tree-node\">"
                "<summary class=\"boat-rudder-analytics__tree-row\">"
                "<span class=\"boat-rudder-analytics__tree-name\">%s <span class=\"boat-rudder-analytics__tree-versions\">%d %s</span><span class=\"boat-rudder-analytics__bar\" style=\"width:%dpx\"></span></span>"
                "<span class=\"boat-rudder-analytics__tree-count\">%d <span class=\"boat-rudder-analytics__pct\">(%d%%)</span></span>"
                "</summary>"
                "<table class=\"boat-rudder-analytics__kv-table boat-rudder-analytics__tree-children\">%s</table>"
                "</details>",
                name_enc, g->len, child_unit, bar_w, g->count, pct, rows) : NULL;
            free(rows);
        }
        free(name_enc);
        html = block ? str_append(html, block) : NULL;
        free(block);
    }
    if (html) html = str_append(html, "</div>");

    free(items);
    free(groups);
    return html ? html : strdup(empty);
}

// Countries breakdown grouped by continent: rewrites each "Chile" key as
// "South America/Chile" so build_grouped_kv_table() can group on the part
// before the '/' like it does for browsers/OSes. "Unknown" (and any name
// country_continent() doesn't know) stays a plain, non-expandable row.
static char *build_continent_kv_table(const KVList *countries, int total) {
    KVList by_continent = {0};
    for (int i = 0; i < countries->count; i++) {
        const char *continent = country_continent(countries->items[i].key);
        // Room for the longest continent name ("South America") plus the '/'.
        char key[sizeof(countries->items[i].key) + 32];
        if (continent) snprintf(key, sizeof(key), "%s/%s", continent, countries->items[i].key);
        else snprintf(key, sizeof(key), "%s", countries->items[i].key);
        kv_increment(&by_continent, key, countries->items[i].count);
    }
    char *html = build_grouped_kv_table(&by_continent, total, "countries");
    kv_free(&by_continent);
    return html;
}

static const char *MONTH_NAMES[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void make_period_label(char *out, size_t out_size, const char *period,
                               int year, int month, int week,
                               const char *date, const char *from, const char *to) {
    if (strcmp(period, "day") == 0) {
        snprintf(out, out_size, "%s", date);
    } else if (strcmp(period, "week") == 0) {
        snprintf(out, out_size, "Week %d / %d", week, year);
    } else if (strcmp(period, "year") == 0) {
        snprintf(out, out_size, "%d", year);
    } else if (strcmp(period, "all") == 0) {
        snprintf(out, out_size, "All Time");
    } else if (strcmp(period, "range") == 0) {
        snprintf(out, out_size, "%s \xe2\x80\x94 %s", from, to);
    } else {
        const char *name = (month >= 1 && month <= 12) ? MONTH_NAMES[month - 1] : "?";
        snprintf(out, out_size, "%s %d", name, year);
    }
}

char *analytics_view(int epoch, const char *period, int year, int month, int week,
                      const char *date, const char *from, const char *to) {
    if (!period || !period[0]) period = "month";

    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    int cur_year = t.tm_year + 1900;
    int cur_month = t.tm_mon + 1;
    char cur_week_buf[4] = {0};
    strftime(cur_week_buf, sizeof(cur_week_buf), "%V", &t);
    int cur_week = atoi(cur_week_buf);
    int prev_year = cur_year - 1;

    char today_str[40];
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d", cur_year, cur_month, t.tm_mday);

    if (!date || !date[0]) date = today_str;
    if (year <= 0) year = cur_year;
    if (month <= 0) month = cur_month;
    if (week <= 0) week = cur_week;
    if (!from || !from[0]) from = today_str;
    if (!to || !to[0]) to = today_str;

    AnalyticsData d = {0};
    query_page_visits(&d, period, year, month, week, date, from, to);

    kv_sort(&d.browsers);
    kv_sort(&d.oses);
    kv_sort(&d.countries);
    kv_sort(&d.routes);

    char period_label[96];
    make_period_label(period_label, sizeof(period_label), period, year, month, week, date, from, to);

    #define ACTIVE_CLASS "boat-rudder-btn--active"
    const char *act_day      = strcmp(period, "day") == 0 ? ACTIVE_CLASS : "";
    const char *act_week     = strcmp(period, "week") == 0 ? ACTIVE_CLASS : "";
    const char *act_month    = strcmp(period, "month") == 0 ? ACTIVE_CLASS : "";
    const char *act_year     = (strcmp(period, "year") == 0 && year == cur_year) ? ACTIVE_CLASS : "";
    const char *act_lastyear = (strcmp(period, "year") == 0 && year == prev_year) ? ACTIVE_CLASS : "";
    const char *act_all      = strcmp(period, "all") == 0 ? ACTIVE_CLASS : "";
    const char *act_range    = strcmp(period, "range") == 0 ? "boat-rudder-analytics__range-form--active" : "";

    char *tpl_path = generate_url_theme("dashboard/analytics/analytics_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        kv_free(&d.browsers);
        kv_free(&d.oses);
        kv_free(&d.countries);
        kv_free(&d.routes);
        return NULL;
    }

    char *tbl_routes    = build_kv_table(&d.routes, d.total);
    char *tbl_browsers  = build_grouped_kv_table(&d.browsers, d.total, "ver.");
    char *tbl_oses      = build_grouped_kv_table(&d.oses, d.total, "ver.");
    char *tbl_countries = build_continent_kv_table(&d.countries, d.total);
    char *tbl_articles  = build_top_articles_table(period, year, month, week, date, from, to);

    char *result = NULL;
    if (tbl_routes && tbl_browsers && tbl_oses && tbl_countries && tbl_articles) {
        result = render_template(tpl,
            today_str, act_day,
            year, week, act_week,
            year, month, act_month,
            cur_year, act_year,
            prev_year, act_lastyear,
            act_all,
            act_range, from, to,
            period_label,
            d.total, d.epochwml, d.epoch0, d.epoch1, d.epoch2, d.epoch3,
            tbl_routes, tbl_browsers, tbl_oses, tbl_countries, tbl_articles);
    }

    free(tpl);
    free(tbl_routes);
    free(tbl_browsers);
    free(tbl_oses);
    free(tbl_countries);
    free(tbl_articles);
    kv_free(&d.browsers);
    kv_free(&d.oses);
    kv_free(&d.countries);
    kv_free(&d.routes);
    return result;
}


// ---------------------------------------------------------------------------
// Dashboard home summary (analytics_summary())
// ---------------------------------------------------------------------------

#define SUMMARY_TOP_N 5
#define SUMMARY_DAYS  7

static int kv_get(const KVList *list, const char *key) {
    for (int i = 0; i < list->count; i++)
        if (strcmp(list->items[i].key, key) == 0) return list->items[i].count;
    return 0;
}

// Sums a browser list's versions per family ("Chrome/124" + "Chrome/125" ->
// "Chrome"), the level a 5-row summary is readable at.
static void kv_by_family(const KVList *src, KVList *out) {
    for (int i = 0; i < src->count; i++) {
        char family[sizeof(src->items[i].key)];
        snprintf(family, sizeof(family), "%.*s",
                 (int)family_len(src->items[i].key), src->items[i].key);
        kv_increment(out, family, src->items[i].count);
    }
}

static void free_analytics_data(AnalyticsData *d) {
    kv_free(&d->browsers);
    kv_free(&d->oses);
    kv_free(&d->countries);
    kv_free(&d->routes);
}

// A "<name> | Today | 7 days" table of `week`'s first `limit` rows (already
// sorted by kv_sort()), today's count looked up in `today` by the same key.
// `href_fmt`, if non-NULL, links each name: a format with one %s for the
// URL-encoded key (top articles -> "/blog/%s").
static char *build_compare_table(const char *name_header, const KVList *week,
                                  const KVList *today, int limit, const char *href_fmt) {
    if (week->count == 0) return strdup("<p class=\"boat-rudder-empty\">No data</p>");

    char *rows = strdup("");
    for (int i = 0; rows && i < week->count && i < limit; i++) {
        const char *key = week->items[i].key;
        char *key_enc = html_encode_dup(key, strlen(key));
        if (!key_enc) { free(rows); rows = NULL; break; }

        char *name;
        if (href_fmt) {
            char key_url[sizeof(week->items[i].key) * 3];
            url_encode(key_url, key, sizeof(key_url));
            char *href = render_template(href_fmt, key_url);
            name = href ? render_template("<a href=\"%s\">%s</a>", href, key_enc) : NULL;
            free(href);
        } else {
            name = strdup(key_enc);
        }
        free(key_enc);

        char *row = name ? render_template("<tr><td>%s</td><td>%d</td><td>%d</td></tr>",
                                            name, kv_get(today, key), week->items[i].count)
                         : NULL;
        free(name);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) return strdup("<p class=\"boat-rudder-empty\">No data</p>");

    char *table = render_template(
        "<table class=\"boat-rudder-analytics__kv-table boat-rudder-analytics__kv-table--compare\">"
        "<thead><tr><th>%s</th><th>Today</th><th>7 days</th></tr></thead><tbody>%s</tbody></table>",
        name_header, rows);
    free(rows);
    return table ? table : strdup("<p class=\"boat-rudder-empty\">No data</p>");
}

// The five per-epoch counters as a compare table, in epoch order (not
// ranked - every row is always shown, zero or not).
static char *build_epoch_compare_table(const AnalyticsData *today, const AnalyticsData *week) {
    const struct { const char *label; int today, week; } rows[] = {
        { "WML (WAP)",        today->epochwml, week->epochwml },
        { "0 (Pre-standard)", today->epoch0,   week->epoch0 },
        { "1 (Early)",        today->epoch1,   week->epoch1 },
        { "2 (Middle)",       today->epoch2,   week->epoch2 },
        { "3 (Modern)",       today->epoch3,   week->epoch3 },
    };

    char *body = strdup("");
    for (size_t i = 0; body && i < sizeof(rows) / sizeof(rows[0]); i++) {
        char *row = render_template("<tr><td>%s</td><td>%d</td><td>%d</td></tr>",
                                     rows[i].label, rows[i].today, rows[i].week);
        body = row ? str_append(body, row) : NULL;
        free(row);
    }
    if (!body) return NULL;

    char *table = render_template(
        "<table class=\"boat-rudder-analytics__kv-table boat-rudder-analytics__kv-table--compare\">"
        "<thead><tr><th>Epoch</th><th>Today</th><th>7 days</th></tr></thead><tbody>%s</tbody></table>",
        body);
    free(body);
    return table;
}

// Blog visits per slug from entry_visits_daily for `from`..`to`, merged
// across days into `week`, and today's buckets alone into `today`.
static void query_blog_visits(const char *from, const char *to, KVList *week, KVList *today) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRY_VISITS_DAILY_COLLECTION);
    if (!collection) return;

    bson_t *filter = BCON_NEW("entry_type", BCON_UTF8("blog"),
                              "date", "{", "$gte", BCON_UTF8(from), "$lte", BCON_UTF8(to), "}");
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, NULL, NULL);

    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        const char *slug = "", *date = "";
        int total = 0;
        if (bson_iter_init_find(&iter, doc, "slug") && BSON_ITER_HOLDS_UTF8(&iter))
            slug = bson_iter_utf8(&iter, NULL);
        if (bson_iter_init_find(&iter, doc, "date") && BSON_ITER_HOLDS_UTF8(&iter))
            date = bson_iter_utf8(&iter, NULL);
        if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
            total = bson_iter_int32(&iter);
        if (!slug[0]) continue;

        kv_increment(week, slug, total);
        if (strcmp(date, to) == 0) kv_increment(today, slug, total);
    }

    bson_destroy(filter);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
}

// The five per-epoch counters as a donut, always in epoch order with a fixed
// color per epoch (series 1 = WML ... 5 = epoch 3), so a color means the same
// epoch on every chart.
static char *build_epoch_donut(const AnalyticsData *week) {
    const ChartSlice slices[] = {
        { "WML (WAP)",        week->epochwml, 1 },
        { "0 (Pre-standard)", week->epoch0,   2 },
        { "1 (Early)",        week->epoch1,   3 },
        { "2 (Middle)",       week->epoch2,   4 },
        { "3 (Modern)",       week->epoch3,   5 },
    };
    return analytics_chart_donut(slices, sizeof(slices) / sizeof(slices[0]),
                                 "7 days", "Visits by epoch, last 7 days");
}

// `week`'s first `limit` rows (already sorted) as horizontal bars, with
// today's count from `today` in each tooltip. `href_fmt` as in
// build_compare_table().
static char *build_summary_hbars(const KVList *week, const KVList *today, int limit,
                                  const char *href_fmt, const char *aria_label) {
    int n = week->count < limit ? week->count : limit;
    if (n == 0) return analytics_chart_hbars(NULL, 0, aria_label);

    ChartBar bars[SUMMARY_TOP_N];
    char details[SUMMARY_TOP_N][32];
    char *hrefs[SUMMARY_TOP_N] = {0};
    for (int i = 0; i < n; i++) {
        const char *key = week->items[i].key;
        snprintf(details[i], sizeof(details[i]), "%d today", kv_get(today, key));
        if (href_fmt) {
            char key_url[sizeof(week->items[i].key) * 3];
            url_encode(key_url, key, sizeof(key_url));
            hrefs[i] = render_template(href_fmt, key_url);
        }
        bars[i] = (ChartBar){ key, week->items[i].count, hrefs[i], details[i] };
    }

    char *chart = analytics_chart_hbars(bars, (size_t)n, aria_label);
    for (int i = 0; i < n; i++) free(hrefs[i]);
    return chart;
}

// Visits per day for the SUMMARY_DAYS days ending today, as columns - today
// highlighted, the rest muted. `dates[i]` is day i's "YYYY-MM-DD", oldest
// first; `weekdays[i]` its "Mon 05" axis label.
static char *build_daily_columns(char dates[][16], char weekdays[][16]) {
    int totals[SUMMARY_DAYS] = {0};

    mongoc_collection_t *collection = mongodb_manager_get_collection(VISITS_DAILY_COLLECTION);
    if (collection) {
        bson_t *filter = BCON_NEW("_id", "{", "$gte", BCON_UTF8(dates[0]),
                                  "$lte", BCON_UTF8(dates[SUMMARY_DAYS - 1]), "}");
        bson_t *opts = BCON_NEW("projection", "{", "total", BCON_INT32(1), "}");
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, opts, NULL);

        const bson_t *doc;
        while (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            if (!bson_iter_init_find(&iter, doc, "_id") || !BSON_ITER_HOLDS_UTF8(&iter)) continue;
            const char *id = bson_iter_utf8(&iter, NULL);
            int total = 0;
            if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
                total = bson_iter_int32(&iter);
            for (int i = 0; i < SUMMARY_DAYS; i++)
                if (strcmp(id, dates[i]) == 0) { totals[i] += total; break; }
        }

        mongoc_cursor_destroy(cursor);
        bson_destroy(opts);
        bson_destroy(filter);
        mongoc_collection_destroy(collection);
    }

    ChartColumn columns[SUMMARY_DAYS];
    char tooltips[SUMMARY_DAYS][48];
    for (int i = 0; i < SUMMARY_DAYS; i++) {
        int is_today = i == SUMMARY_DAYS - 1;
        snprintf(tooltips[i], sizeof(tooltips[i]), "%s%s: %d visits",
                 dates[i], is_today ? " (today)" : "", totals[i]);
        columns[i] = (ChartColumn){ is_today ? "Today" : weekdays[i], tooltips[i],
                                    totals[i], is_today };
    }
    return analytics_chart_columns(columns, SUMMARY_DAYS, "Visits per day, last 7 days");
}

char *analytics_summary(int epoch) {
    time_t now = time(NULL);
    time_t week_start = now - 6 * 24 * 60 * 60;
    struct tm t_now, t_from;
    gmtime_r(&now, &t_now);
    gmtime_r(&week_start, &t_from);

    char today_str[16], from_str[16];
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", &t_now);
    strftime(from_str, sizeof(from_str), "%Y-%m-%d", &t_from);

    char dates[SUMMARY_DAYS][16], weekdays[SUMMARY_DAYS][16];
    for (int i = 0; i < SUMMARY_DAYS; i++) {
        time_t day = now - (time_t)(SUMMARY_DAYS - 1 - i) * 24 * 60 * 60;
        struct tm t_day;
        gmtime_r(&day, &t_day);
        strftime(dates[i], sizeof(dates[i]), "%Y-%m-%d", &t_day);
        strftime(weekdays[i], sizeof(weekdays[i]), "%a %d", &t_day);
    }

    // Same period filters as the full report: "range" over the 7 day-buckets
    // for the week column, "day" (today's bucket alone) for the today column.
    AnalyticsData week = {0}, today = {0};
    query_page_visits(&week, "range", 0, 0, 0, NULL, from_str, today_str);
    query_page_visits(&today, "day", 0, 0, 0, today_str, NULL, NULL);

    KVList browsers_week = {0}, browsers_today = {0};
    kv_by_family(&week.browsers, &browsers_week);
    kv_by_family(&today.browsers, &browsers_today);
    kv_sort(&browsers_week);
    kv_sort(&week.countries);

    KVList blog_week = {0}, blog_today = {0};
    query_blog_visits(from_str, today_str, &blog_week, &blog_today);
    kv_sort(&blog_week);

    char *tpl_path = generate_url_theme("dashboard/analytics/summary_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);

    char *tbl_epochs    = build_epoch_compare_table(&today, &week);
    char *tbl_countries = build_compare_table("Country", &week.countries, &today.countries,
                                               SUMMARY_TOP_N, NULL);
    char *tbl_browsers  = build_compare_table("Browser", &browsers_week, &browsers_today,
                                               SUMMARY_TOP_N, NULL);
    char *tbl_articles  = build_compare_table("Article", &blog_week, &blog_today,
                                               SUMMARY_TOP_N, "/blog/%s");

    char *chart_days      = build_daily_columns(dates, weekdays);
    char *chart_epochs    = build_epoch_donut(&week);
    char *chart_countries = build_summary_hbars(&week.countries, &today.countries, SUMMARY_TOP_N,
                                                NULL, "Top 5 countries, last 7 days");
    char *chart_browsers  = build_summary_hbars(&browsers_week, &browsers_today, SUMMARY_TOP_N,
                                                NULL, "Top 5 browsers, last 7 days");
    char *chart_articles  = build_summary_hbars(&blog_week, &blog_today, SUMMARY_TOP_N,
                                                "/blog/%s", "Top 5 blog articles, last 7 days");

    char *result = NULL;
    if (tpl && tbl_epochs && tbl_countries && tbl_browsers && tbl_articles &&
        chart_days && chart_epochs && chart_countries && chart_browsers && chart_articles) {
        result = render_template(tpl, from_str, today_str,
                                  today.total, week.total, chart_days,
                                  chart_articles, tbl_articles,
                                  chart_epochs, tbl_epochs,
                                  chart_browsers, tbl_browsers,
                                  chart_countries, tbl_countries);
    }

    free(chart_days);
    free(chart_epochs);
    free(chart_countries);
    free(chart_browsers);
    free(chart_articles);
    free(tpl);
    free(tbl_epochs);
    free(tbl_countries);
    free(tbl_browsers);
    free(tbl_articles);
    free_analytics_data(&week);
    free_analytics_data(&today);
    kv_free(&browsers_week);
    kv_free(&browsers_today);
    kv_free(&blog_week);
    kv_free(&blog_today);
    return result;
}
