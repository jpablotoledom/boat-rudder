#include "analytics_view.h"
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
        "<table class=\"boat-rudder__dashboard__table\">"
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
    if (count == 0) return strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");

    int max_count = arr[0].count > 0 ? arr[0].count : 1;

    char *rows = strdup("");
    for (int i = 0; rows && i < count; i++) {
        int bar_w = max_count > 0 ? arr[i].count * 100 / max_count : 0;
        int pct = total > 0 ? arr[i].count * 100 / total : 0;

        char *key_enc = malloc(strlen(arr[i].key) * 6 + 1);
        if (!key_enc) { free(rows); rows = NULL; break; }
        html_encode(key_enc, arr[i].key, strlen(arr[i].key) * 6 + 1);

        char *row = render_template(
            "<tr><td>%s<span class=\"boat-rudder__analytics__bar\" style=\"width:%dpx\"></span></td>"
            "<td>%d <span class=\"boat-rudder__analytics__pct\">(%d%%)</span></td></tr>",
            key_enc, bar_w, arr[i].count, pct);
        free(key_enc);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) return strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");

    char *table = render_template("<table class=\"boat-rudder__analytics__kv-table\">%s</table>", rows);
    free(rows);
    return table ? table : strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");
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
    const char *empty = "<p class=\"boat-rudder__dashboard__empty\">No data</p>";
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

    char *html = strdup("<div class=\"boat-rudder__analytics__tree\">");
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
                "<div class=\"boat-rudder__analytics__tree-row boat-rudder__analytics__tree-row--leaf\">"
                "<span class=\"boat-rudder__analytics__tree-name\">%s<span class=\"boat-rudder__analytics__bar\" style=\"width:%dpx\"></span></span>"
                "<span class=\"boat-rudder__analytics__tree-count\">%d <span class=\"boat-rudder__analytics__pct\">(%d%%)</span></span>"
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
                    "<tr><td>%s<span class=\"boat-rudder__analytics__bar\" style=\"width:%dpx\"></span></td>"
                    "<td>%d <span class=\"boat-rudder__analytics__pct\">(%d%%)</span></td></tr>",
                    ver_enc, items[i].count * 100 / max_ver, items[i].count,
                    total > 0 ? items[i].count * 100 / total : 0);
                free(ver_enc);
                rows = row ? str_append(rows, row) : NULL;
                free(row);
            }
            block = rows ? render_template(
                "<details class=\"boat-rudder__analytics__tree-node\">"
                "<summary class=\"boat-rudder__analytics__tree-row\">"
                "<span class=\"boat-rudder__analytics__tree-name\">%s <span class=\"boat-rudder__analytics__tree-versions\">%d %s</span><span class=\"boat-rudder__analytics__bar\" style=\"width:%dpx\"></span></span>"
                "<span class=\"boat-rudder__analytics__tree-count\">%d <span class=\"boat-rudder__analytics__pct\">(%d%%)</span></span>"
                "</summary>"
                "<table class=\"boat-rudder__analytics__kv-table boat-rudder__analytics__tree-children\">%s</table>"
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
        char key[sizeof(countries->items[i].key)];
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

    char period_label[64];
    make_period_label(period_label, sizeof(period_label), period, year, month, week, date, from, to);

    #define ACTIVE_CLASS "boat-rudder__analytics__period-btn--active"
    const char *act_day      = strcmp(period, "day") == 0 ? ACTIVE_CLASS : "";
    const char *act_week     = strcmp(period, "week") == 0 ? ACTIVE_CLASS : "";
    const char *act_month    = strcmp(period, "month") == 0 ? ACTIVE_CLASS : "";
    const char *act_year     = (strcmp(period, "year") == 0 && year == cur_year) ? ACTIVE_CLASS : "";
    const char *act_lastyear = (strcmp(period, "year") == 0 && year == prev_year) ? ACTIVE_CLASS : "";
    const char *act_all      = strcmp(period, "all") == 0 ? ACTIVE_CLASS : "";
    const char *act_range    = strcmp(period, "range") == 0 ? ACTIVE_CLASS : "";

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
