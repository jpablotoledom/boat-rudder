#include "search_page.h"
#include "../../db/cms_search.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_lang.h"
#include "../../utils/request_site.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// HTML-escaped copy of `src`; on WML also with '$' doubled, since a lone one
// starts a variable reference there.
static char *escape_alloc(const char *src, int epoch) {
    size_t cap = strlen(src) * 6 + 1;
    char *html = malloc(cap);
    if (!html) return NULL;
    html_encode(html, src, cap);
    if (epoch != EPOCH_WML || !strchr(html, '$')) return html;

    char *out = malloc(strlen(html) * 2 + 1);
    char *w = out;
    for (const char *p = html; out && *p; p++) {
        if (*p == '$') *w++ = '$';
        *w++ = *p;
    }
    if (out) *w = '\0';
    free(html);
    return out;
}

static int valid_utf8(const unsigned char *s) {
    while (*s) {
        int n = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2
              : (*s & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return 0;
        for (s++; n > 0; n--, s++) if ((*s & 0xC0) != 0x80) return 0;
    }
    return 1;
}

// The query as UTF-8, trimmed, capped at SEARCH_QUERY_MAX bytes on a
// character boundary. Pages for epoch 0/1 and WML are sent in ISO-8859-1,
// so their forms come back in it ("música" as 0xFA): anything that isn't
// valid UTF-8 is read as Latin-1.
static void normalize_query(const char *raw, char *out, size_t size) {
    char utf8[SEARCH_QUERY_MAX * 2 + 8];
    const unsigned char *s = (const unsigned char *)(raw ? raw : "");
    size_t k = 0;
    if (valid_utf8(s)) {
        snprintf(utf8, sizeof(utf8), "%s", (const char *)s);
    } else {
        for (; *s && k + 3 < sizeof(utf8); s++) {
            if (*s < 0x80) {
                utf8[k++] = (char)*s;
            } else {
                utf8[k++] = (char)(0xC0 | (*s >> 6));
                utf8[k++] = (char)(0x80 | (*s & 0x3F));
            }
        }
        utf8[k] = '\0';
    }

    const char *p = utf8;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) n--;
    if (n > SEARCH_QUERY_MAX) {
        n = SEARCH_QUERY_MAX;
        while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80) n--;
    }
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

// What a form needs to carry along that links get from the response layer
// (build_epoch_response.c tags href="..." but not a form's action): the
// language on epochs 0/1, the theme on epochs 1/2, a Host-less request's
// site everywhere. WML's <go href> is tagged like any link, so it needs
// nothing here.
static char *hidden_fields(int epoch) {
    char *out = strdup("");
    if (!out || epoch == EPOCH_WML) return out;
    if (epoch <= EPOCH_EARLY) {
        char *f = render_template("\n<input type=\"hidden\" name=\"lang\" value=\"%s\">", request_lang());
        out = f ? str_append(out, f) : (free(out), NULL);
        free(f);
    }
    if (out && (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE)) {
        char *f = render_template("\n<input type=\"hidden\" name=\"theme\" value=\"%s\">", request_theme());
        out = f ? str_append(out, f) : (free(out), NULL);
        free(f);
    }
    const char *site = request_site();
    if (out && site && site[0]) {
        char *f = render_template("\n<input type=\"hidden\" name=\"site\" value=\"%s\">", site);
        out = f ? str_append(out, f) : (free(out), NULL);
        free(f);
    }
    return out;
}

static char *result_item(int epoch, const char *item_tpl, const char *mark_tpl, const CmsSearchResult *r) {
    // The home page's own entry is a page whose link is "/" itself.
    char url[600];
    if (r->link[0] == '/')
        snprintf(url, sizeof(url), "%s", r->link);
    else
        snprintf(url, sizeof(url), "/%s/%s", strcmp(r->type, "blog") == 0 ? "blog" : "page", r->link);
    char meta[64];
    if (strcmp(r->type, "blog") != 0)
        snprintf(meta, sizeof(meta), "Page");
    else if (r->date[0])
        snprintf(meta, sizeof(meta), "Blog - %s", r->date);
    else
        snprintf(meta, sizeof(meta), "Blog");

    char *url_enc = escape_alloc(url, epoch);
    char *title   = escape_alloc(r->title, epoch);
    char *before  = escape_alloc(r->before, epoch);
    char *match   = escape_alloc(r->match, epoch);
    char *after   = escape_alloc(r->after, epoch);
    char *marked  = match ? render_template(mark_tpl, match) : NULL;
    char *snippet = (before && marked && after) ? render_template("%s%s%s", before, marked, after) : NULL;
    char *item    = (url_enc && title && snippet) ? render_template(item_tpl, url_enc, title, meta, snippet) : NULL;

    free(url_enc); free(title); free(before); free(match); free(after); free(marked); free(snippet);
    return item;
}

char *search_page(int epoch, const char *raw_query) {
    char *page_tpl = load_template("search/search_epoch%d.html", epoch);
    char *item_tpl = load_template("search/search-item_epoch%d.html", epoch);
    char *mark_tpl = load_template("search/search-mark_epoch%d.html", epoch);
    char *query_html = NULL, *summary = NULL, *rows = NULL, *hidden = NULL, *result = NULL;
    CmsSearchResult *results = NULL;
    size_t count = 0;

    if (!page_tpl || !item_tpl || !mark_tpl) goto cleanup;

    char query[SEARCH_QUERY_MAX + 1];
    normalize_query(raw_query, query, sizeof(query));
    query_html = escape_alloc(query, epoch);
    hidden = hidden_fields(epoch);
    if (!query_html || !hidden) goto cleanup;

    rows = strdup("");
    if (!query[0]) {
        summary = strdup("Search the blog and the pages.");
    } else if (strlen(query) < SEARCH_QUERY_MIN) {
        summary = render_template("Type at least %d characters.", SEARCH_QUERY_MIN);
    } else if (!mongodb_manager_is_ready() ||
               cms_search_entries(query, request_lang(), &results, &count) != 0) {
        summary = strdup("Search is not available right now. Please try again later.");
    } else if (count == 0) {
        summary = render_template("No results for \"%s\".", query_html);
    } else {
        summary = render_template("%zu result%s for \"%s\"", count, count == 1 ? "" : "s", query_html);
        for (size_t i = 0; rows && i < count; i++) {
            char *item = result_item(epoch, item_tpl, mark_tpl, &results[i]);
            rows = item ? str_append(rows, item) : (free(rows), NULL);
            free(item);
        }
    }
    if (!summary || !rows) goto cleanup;

    result = render_template(page_tpl, query_html, summary, rows, hidden);

cleanup:
    cms_search_free(results, count);
    free(page_tpl);
    free(item_tpl);
    free(mark_tpl);
    free(query_html);
    free(summary);
    free(rows);
    free(hidden);
    return result;
}
