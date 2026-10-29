#include "cms_search.h"
#include "cms_entries.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <ctype.h>
#include <mongoc/mongoc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNIPPET_BEFORE  70    // bytes of context before the match
#define SNIPPET_AFTER   110   // and after it

// The block types whose text a reader sees as prose ("tittle" is how older
// entries spell "title").
static const char *const SEARCHED_BLOCKS[] = { "title", "tittle", "paragraph", "list" };

// ---- Text helpers ---------------------------------------------------------------

// The query as a literal regex: every ASCII character that isn't a letter,
// digit or space is escaped, so "C++" or "a.b" mean exactly that. UTF-8
// bytes pass through.
static char *regex_literal(const char *query) {
    char *out = malloc(strlen(query) * 2 + 1);
    if (!out) return NULL;
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        if (*p < 0x80 && !isalnum(*p) && *p != ' ') *w++ = '\\';
        *w++ = (char)*p;
    }
    *w = '\0';
    return out;
}

// Lower-cases ASCII and the Latin-1 capitals (U+00C0-U+00DE, two bytes
// C3 80-C3 9E, but not U+00D7 "×") in place. Byte length never changes, so
// an offset found in the folded copy is valid in the original.
static void fold_case(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (unsigned char)(*p + 32);
        } else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            p[1] = (unsigned char)(p[1] + 0x20);
            p++;
        }
    }
}

// Rich text to what a reader sees: tags dropped (a tag stands for a space,
// so "a</p><p>b" doesn't read "ab"), the common entities decoded, runs of
// whitespace collapsed. Returns a malloc'd string.
static char *plain_text(const char *html) {
    size_t n = strlen(html);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    char *w = out;
    int space = 0;
    for (const char *p = html; *p; ) {
        char c = 0;
        if (*p == '<') {
            const char *end = strchr(p, '>');
            p = end ? end + 1 : p + strlen(p);
            space = 1;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *name; char c; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
                { "&#39;", '\'' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }, { "&#160;", ' ' },
            };
            size_t i;
            for (i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
                size_t l = strlen(ents[i].name);
                if (strncmp(p, ents[i].name, l) == 0) { c = ents[i].c; p += l; break; }
            }
            if (i == sizeof(ents) / sizeof(ents[0])) c = *p++;
        } else {
            c = *p++;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = 1;
            continue;
        }
        if (space && w > out) *w++ = ' ';
        space = 0;
        *w++ = c;
    }
    *w = '\0';
    return out;
}

// Moves `i` back (dir -1) or forward (dir 1) off a UTF-8 continuation byte.
static size_t utf8_boundary(const char *s, size_t len, size_t i, int dir) {
    while (i > 0 && i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i += (size_t)dir;
    return i;
}

// If `text` contains the folded `needle`, fills r->before/match/after with
// an excerpt around the first occurrence and returns 1.
static int excerpt(const char *text, const char *needle, size_t needle_len, CmsSearchResult *r) {
    char *folded = strdup(text);
    if (!folded) return 0;
    fold_case(folded);
    const char *hit = strstr(folded, needle);
    size_t at = hit ? (size_t)(hit - folded) : 0;
    free(folded);
    if (!hit) return 0;

    size_t len = strlen(text);
    size_t start = at > SNIPPET_BEFORE ? at - SNIPPET_BEFORE : 0;
    size_t end = at + needle_len + SNIPPET_AFTER < len ? at + needle_len + SNIPPET_AFTER : len;
    start = utf8_boundary(text, len, start, 1);
    end = utf8_boundary(text, len, end, -1);
    // Whole words at the cut edges.
    if (start > 0) {
        const char *sp = memchr(text + start, ' ', at - start);
        if (sp) start = (size_t)(sp - text) + 1;
    }
    if (end < len) {
        size_t e = end;
        while (e > at + needle_len && text[e] != ' ') e--;
        if (e > at + needle_len) end = e;
    }

    r->before = malloc(at - start + 4);
    r->match = strndup(text + at, needle_len);
    r->after = malloc(end - (at + needle_len) + 4);
    if (!r->before || !r->match || !r->after) return 0;
    snprintf(r->before, at - start + 4, "%s%.*s", start > 0 ? "..." : "", (int)(at - start), text + start);
    snprintf(r->after, end - (at + needle_len) + 4, "%.*s%s",
             (int)(end - (at + needle_len)), text + at + needle_len, end < len ? "..." : "");
    return 1;
}

// ---- Query --------------------------------------------------------------------

static int valid_lang(const char *lang) {
    size_t n = strlen(lang);
    if (n < 2 || n > 3) return 0;
    for (; *lang; lang++) if (!islower((unsigned char)*lang)) return 0;
    return 1;
}

// {"<field>.<lang>": /re/i} for the header fields, and a content element of
// a searched type with "text.<lang>" matching.
static void append_lang_clauses(bson_t *or_array, int *idx, const char *lang, const char *re) {
    char key[16], field[64];
    const char *header_fields[] = { "header.title", "header.summary" };
    for (size_t i = 0; i < 2; i++) {
        bson_t clause;
        snprintf(key, sizeof(key), "%d", (*idx)++);
        snprintf(field, sizeof(field), "%s.%s", header_fields[i], lang);
        bson_append_document_begin(or_array, key, -1, &clause);
        bson_append_regex(&clause, field, -1, re, "i");
        bson_append_document_end(or_array, &clause);
    }

    bson_t clause, content, elem, type, in;
    snprintf(key, sizeof(key), "%d", (*idx)++);
    snprintf(field, sizeof(field), "text.%s", lang);
    bson_append_document_begin(or_array, key, -1, &clause);
    bson_append_document_begin(&clause, "content", -1, &content);
    bson_append_document_begin(&content, "$elemMatch", -1, &elem);
    bson_append_document_begin(&elem, "type", -1, &type);
    bson_append_array_begin(&type, "$in", -1, &in);
    for (size_t i = 0; i < sizeof(SEARCHED_BLOCKS) / sizeof(SEARCHED_BLOCKS[0]); i++) {
        char k[8];
        snprintf(k, sizeof(k), "%zu", i);
        bson_append_utf8(&in, k, -1, SEARCHED_BLOCKS[i], -1);
    }
    bson_append_array_end(&type, &in);
    bson_append_document_end(&elem, &type);
    bson_append_regex(&elem, field, -1, re, "i");
    bson_append_document_end(&content, &elem);
    bson_append_document_end(&clause, &content);
    bson_append_document_end(or_array, &clause);
}

static int is_searched_block(const char *type) {
    for (size_t i = 0; i < sizeof(SEARCHED_BLOCKS) / sizeof(SEARCHED_BLOCKS[0]); i++)
        if (type && strcmp(type, SEARCHED_BLOCKS[i]) == 0) return 1;
    return 0;
}

// Checks one candidate the way its reader sees it and, on a match, fills `r`.
static int check_entry(const char *link, const char *lang, const char *needle, size_t needle_len,
                       CmsSearchResult *r) {
    CmsEntry e;
    int found = cms_get_entry_by_link(link, lang, 0, &e);
    int hit = 0;
    if (found && e.type && (strcmp(e.type, "blog") == 0 || strcmp(e.type, "page") == 0)) {
        const char *title = e.header_title && e.header_title[0] ? e.header_title : NULL;
        for (size_t i = 0; !title && i < e.content_count; i++)
            if (e.content[i].type && (strcmp(e.content[i].type, "title") == 0 ||
                                      strcmp(e.content[i].type, "tittle") == 0) &&
                e.content[i].text && e.content[i].text[0])
                title = e.content[i].text;

        // Title, summary, then the blocks in reading order: the excerpt
        // comes from the first place the reader would meet the word.
        const char *texts[2] = { e.header_title, e.header_summary };
        for (size_t i = 0; !hit && i < 2 + e.content_count; i++) {
            const char *raw = i < 2 ? texts[i]
                            : is_searched_block(e.content[i - 2].type) ? e.content[i - 2].text : NULL;
            if (!raw || !raw[0]) continue;
            char *plain = plain_text(raw);
            if (plain) hit = excerpt(plain, needle, needle_len, r);
            free(plain);
        }
        if (hit) {
            r->link = strdup(link);
            r->type = strdup(e.type);
            char *plain_title = plain_text(title ? title : link);
            r->title = plain_title ? plain_title : strdup(link);
            snprintf(r->date, sizeof(r->date), "%s", strcmp(e.type, "blog") == 0 ? e.header_date : "");
            hit = r->link && r->type && r->title;
        }
    }
    cms_entry_free(&e);
    return hit;
}

int cms_search_entries(const char *query, const char *lang, CmsSearchResult **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!valid_lang(lang)) lang = "en";

    mongoc_collection_t *col = mongodb_manager_get_collection(ENTRIES_COLLECTION);
    if (!col) return -1;

    char *re = regex_literal(query);
    char *needle = strdup(query);
    if (!re || !needle) {
        free(re);
        free(needle);
        mongoc_collection_destroy(col);
        return -1;
    }
    fold_case(needle);
    size_t needle_len = strlen(needle);

    // Pre-selection in MongoDB: the reader's language and, since entries
    // fall back to it, English. The exact check below then reads each
    // candidate in the reader's language, tags stripped.
    bson_t filter, type, in, or_array;
    bson_init(&filter);
    bson_append_bool(&filter, "enabled", -1, true);
    bson_append_document_begin(&filter, "type", -1, &type);
    bson_append_array_begin(&type, "$in", -1, &in);
    bson_append_utf8(&in, "0", -1, "blog", -1);
    bson_append_utf8(&in, "1", -1, "page", -1);
    bson_append_array_end(&type, &in);
    bson_append_document_end(&filter, &type);
    bson_append_array_begin(&filter, "$or", -1, &or_array);
    int idx = 0;
    append_lang_clauses(&or_array, &idx, lang, re);
    if (strcmp(lang, "en") != 0) append_lang_clauses(&or_array, &idx, "en", re);
    bson_append_array_end(&filter, &or_array);

    bson_t *opts = BCON_NEW("projection", "{", "link", BCON_INT32(1), "}",
                            "sort", "{", "header.date", BCON_INT32(-1), "}",
                            "limit", BCON_INT64(SEARCH_CANDIDATES_MAX));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, &filter, opts, NULL);

    // Links first: check_entry() runs its own queries, which must not
    // interleave with this cursor on the thread's one client.
    char **links = calloc(SEARCH_CANDIDATES_MAX, sizeof(char *));
    size_t nlinks = 0;
    const bson_t *doc;
    while (links && nlinks < SEARCH_CANDIDATES_MAX && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "link") && BSON_ITER_HOLDS_UTF8(&it))
            links[nlinks++] = strdup(bson_iter_utf8(&it, NULL));
    }
    bson_error_t error;
    int rc = links ? 0 : -1;
    if (mongoc_cursor_error(cursor, &error)) {
        LOG_ERROR("cms_search_entries: %s", error.message);
        rc = -1;
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(&filter);
    mongoc_collection_destroy(col);

    CmsSearchResult *results = rc == 0 ? calloc(SEARCH_RESULTS_LIMIT, sizeof(CmsSearchResult)) : NULL;
    if (rc == 0 && !results) rc = -1;
    size_t n = 0;
    for (size_t i = 0; rc == 0 && i < nlinks && n < SEARCH_RESULTS_LIMIT; i++) {
        if (!links[i]) continue;
        if (check_entry(links[i], lang, needle, needle_len, &results[n])) {
            n++;
        } else {
            // A miss may leave a partly filled excerpt behind.
            free(results[n].before);
            free(results[n].match);
            free(results[n].after);
            free(results[n].link);
            free(results[n].type);
            free(results[n].title);
            memset(&results[n], 0, sizeof(results[n]));
        }
    }

    for (size_t i = 0; i < nlinks; i++) free(links[i]);
    free(links);
    free(re);
    free(needle);

    if (rc != 0) {
        cms_search_free(results, n);
        return -1;
    }
    *out = results;
    *count = n;
    return 0;
}

void cms_search_free(CmsSearchResult *results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].link);
        free(results[i].type);
        free(results[i].title);
        free(results[i].before);
        free(results[i].match);
        free(results[i].after);
    }
    free(results);
}
