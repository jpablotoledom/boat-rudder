#include "wbxml.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---- Byte buffer -------------------------------------------------------------

typedef struct {
    unsigned char *d;
    size_t len, cap;
    int ok;
} Buf;

static void b_put(Buf *b, const void *p, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        unsigned char *nd = realloc(b->d, cap);
        if (!nd) { b->ok = 0; return; }
        b->d = nd;
        b->cap = cap;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    b->d[b->len] = 0;
}

static void b_byte(Buf *b, unsigned char c) { b_put(b, &c, 1); }

static void b_utf8(Buf *b, unsigned long cp) {
    unsigned char u[4];
    size_t n;
    if (cp < 0x80)         { u[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)   { u[0] = 0xC0 | (cp >> 6); u[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { u[0] = 0xE0 | (cp >> 12); u[1] = 0x80 | ((cp >> 6) & 0x3F);
                             u[2] = 0x80 | (cp & 0x3F); n = 3; }
    else                   { u[0] = 0xF0 | (cp >> 18); u[1] = 0x80 | ((cp >> 12) & 0x3F);
                             u[2] = 0x80 | ((cp >> 6) & 0x3F); u[3] = 0x80 | (cp & 0x3F); n = 4; }
    b_put(b, u, n);
}

// ---- WML tree ----------------------------------------------------------------

typedef struct Node {
    char *name;                 // NULL for a text node
    char *text;                 // text nodes only (UTF-8, whitespace collapsed)
    char **an, **av;
    int na;
    struct Node **kids;
    int nk, ck;
    int borrowed;               // kids belong to another node (a split wrapper)
} Node;

static Node *node_new(const char *name) {
    Node *n = calloc(1, sizeof(*n));
    if (n && name) n->name = strdup(name);
    return n;
}

static void node_add(Node *parent, Node *kid) {
    if (!parent || !kid) return;
    if (parent->nk == parent->ck) {
        int ck = parent->ck ? parent->ck * 2 : 4;
        Node **nk = realloc(parent->kids, (size_t)ck * sizeof(*nk));
        if (!nk) return;
        parent->kids = nk;
        parent->ck = ck;
    }
    parent->kids[parent->nk++] = kid;
}

static void node_set_attr(Node *n, const char *k, const char *v) {
    char **an = realloc(n->an, (size_t)(n->na + 1) * sizeof(*an));
    if (!an) return;
    n->an = an;
    char **av = realloc(n->av, (size_t)(n->na + 1) * sizeof(*av));
    if (!av) return;
    n->av = av;
    n->an[n->na] = strdup(k);
    n->av[n->na] = strdup(v);
    n->na++;
}

static const char *node_attr(const Node *n, const char *k) {
    for (int i = 0; i < n->na; i++)
        if (strcasecmp(n->an[i], k) == 0) return n->av[i];
    return NULL;
}

static void node_free(Node *n) {
    if (!n) return;
    if (!n->borrowed)
        for (int i = 0; i < n->nk; i++) node_free(n->kids[i]);
    for (int i = 0; i < n->na; i++) { free(n->an[i]); free(n->av[i]); }
    free(n->an);
    free(n->av);
    free(n->kids);
    free(n->name);
    free(n->text);
    free(n);
}

// Nodes made after parsing (split wrappers, nav links, text chunks) - freed
// together once the page is compiled.
typedef struct {
    Node **items;
    int n, cap;
} Pool;

static Node *pool_add(Pool *p, Node *n) {
    if (!n) return NULL;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        Node **ni = realloc(p->items, (size_t)cap * sizeof(*ni));
        if (!ni) { node_free(n); return NULL; }
        p->items = ni;
        p->cap = cap;
    }
    p->items[p->n++] = n;
    return n;
}

static void pool_free(Pool *p) {
    for (int i = 0; i < p->n; i++) node_free(p->items[i]);
    free(p->items);
}

static Node *text_node(const char *s) {
    Node *n = node_new(NULL);
    if (n) n->text = strdup(s);
    return n;
}

// ---- Parsing -----------------------------------------------------------------

// Decodes entities (&amp; &lt; &gt; &quot; &apos; &nbsp; &#N; &#xH;) from
// s[0..n) into `out`; an unknown entity is kept verbatim.
static void decode_entities(const char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { b_byte(out, (unsigned char)s[i]); continue; }
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) { b_byte(out, '&'); continue; }
        size_t elen = (size_t)(semi - (s + i)) + 1;
        const char *e = s + i + 1;
        size_t nlen = elen - 2;
        unsigned long cp = 0;
        int known = 1;
        if (nlen == 3 && !strncmp(e, "amp", 3)) cp = '&';
        else if (nlen == 2 && !strncmp(e, "lt", 2)) cp = '<';
        else if (nlen == 2 && !strncmp(e, "gt", 2)) cp = '>';
        else if (nlen == 4 && !strncmp(e, "quot", 4)) cp = '"';
        else if (nlen == 4 && !strncmp(e, "apos", 4)) cp = '\'';
        else if (nlen == 4 && !strncmp(e, "nbsp", 4)) cp = 0xA0;
        else if (nlen > 1 && e[0] == '#') {
            char num[12] = {0};
            memcpy(num, e + 1, nlen - 1 < sizeof(num) - 1 ? nlen - 1 : sizeof(num) - 1);
            char *end = NULL;
            cp = (num[0] == 'x' || num[0] == 'X') ? strtoul(num + 1, &end, 16) : strtoul(num, &end, 10);
            if (!end || *end || cp == 0 || cp > 0x10FFFF) known = 0;
        } else known = 0;
        if (!known) { b_byte(out, '&'); continue; }
        b_utf8(out, cp);
        i += elen - 1;
    }
}

// Text content: entities decoded, whitespace runs collapsed to one space
// (not trimmed - "a</b> |<a>" must keep its spaces). NULL if all blank.
static char *make_text(const char *s, size_t n) {
    Buf raw = { .ok = 1 };
    decode_entities(s, n, &raw);
    if (!raw.ok || !raw.d) { free(raw.d); return NULL; }
    Buf out = { .ok = 1 };
    int space = 0, any = 0;
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = raw.d[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = 1; continue; }
        if (space) b_byte(&out, ' ');
        space = 0;
        b_byte(&out, c);
        any = 1;
    }
    if (space && any) b_byte(&out, ' ');
    free(raw.d);
    if (!any || !out.ok) { free(out.d); return NULL; }
    return (char *)out.d;
}

static int is_name_char(char c) {
    return isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-' || c == '.';
}

static int is_void_tag(const char *name) {
    return !strcasecmp(name, "br") || !strcasecmp(name, "img") ||
           !strcasecmp(name, "input") || !strcasecmp(name, "meta");
}

// A tolerant XML reader for this server's own WML: prolog, DOCTYPE and
// comments skipped; a stray close tag pops back to its match (or is ignored).
static Node *parse_wml(const char *s, size_t n) {
    Node *root = node_new("#root");
    if (!root) return NULL;
    Node *stack[64];
    int sp = 0;
    stack[0] = root;

    size_t i = 0;
    while (i < n) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < n && s[j] != '<') j++;
            char *t = make_text(s + i, j - i);
            if (t) {
                Node *tn = node_new(NULL);
                if (tn) { tn->text = t; node_add(stack[sp], tn); } else free(t);
            }
            i = j;
            continue;
        }
        if (!strncmp(s + i, "<!--", 4)) {
            const char *e = strstr(s + i + 4, "-->");
            i = e ? (size_t)(e - s) + 3 : n;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '?' || s[i + 1] == '!')) {
            const char *e = memchr(s + i, '>', n - i);
            i = e ? (size_t)(e - s) + 1 : n;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {
            size_t j = i + 2, k;
            while (j < n && is_name_char(s[j])) j++;
            char name[32] = {0};
            k = j - (i + 2);
            memcpy(name, s + i + 2, k < sizeof(name) - 1 ? k : sizeof(name) - 1);
            const char *e = memchr(s + j, '>', n - j);
            i = e ? (size_t)(e - s) + 1 : n;
            for (int d = sp; d > 0; d--) {
                if (!strcasecmp(stack[d]->name, name)) { sp = d - 1; break; }
            }
            continue;
        }

        size_t j = i + 1;
        while (j < n && is_name_char(s[j])) j++;
        if (j == i + 1) { i++; continue; }
        char name[32] = {0};
        size_t k = j - (i + 1);
        memcpy(name, s + i + 1, k < sizeof(name) - 1 ? k : sizeof(name) - 1);
        Node *el = node_new(name);
        if (!el) break;

        int self_close = 0;
        while (j < n) {
            while (j < n && isspace((unsigned char)s[j])) j++;
            if (j >= n) break;
            if (s[j] == '>') { j++; break; }
            if (s[j] == '/') { self_close = 1; j++; continue; }
            size_t a0 = j;
            while (j < n && is_name_char(s[j])) j++;
            if (j == a0) { j++; continue; }
            char an[32] = {0};
            size_t al = j - a0;
            memcpy(an, s + a0, al < sizeof(an) - 1 ? al : sizeof(an) - 1);
            while (j < n && isspace((unsigned char)s[j])) j++;
            if (j < n && s[j] == '=') {
                j++;
                while (j < n && isspace((unsigned char)s[j])) j++;
                size_t v0, v1;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    v0 = j;
                    while (j < n && s[j] != q) j++;
                    v1 = j;
                    if (j < n) j++;
                } else {
                    v0 = j;
                    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>') j++;
                    v1 = j;
                }
                Buf v = { .ok = 1 };
                decode_entities(s + v0, v1 - v0, &v);
                node_set_attr(el, an, v.d ? (char *)v.d : "");
                free(v.d);
            } else {
                node_set_attr(el, an, an);
            }
        }
        i = j;
        node_add(stack[sp], el);
        if (!self_close && !is_void_tag(name) && sp < 63) stack[++sp] = el;
    }
    return root;
}

static char *latin1_to_utf8(const char *s, size_t n) {
    Buf b = { .ok = 1 };
    for (size_t i = 0; i < n; i++) b_utf8(&b, (unsigned char)s[i]);
    if (!b.ok) { free(b.d); return NULL; }
    if (!b.d) return strdup("");
    return (char *)b.d;
}

// ---- Compiler ----------------------------------------------------------------

#define TOKEN_END   0x01
#define TOKEN_STR_I 0x03
#define ATTR_HREF   0x4A
#define ATTR_SRC    0x32

static const struct { const char *name; int code; } ELEMENTS[] = {
    {"wml", 0x3F}, {"card", 0x27}, {"do", 0x28}, {"onevent", 0x33}, {"head", 0x2C},
    {"template", 0x3B}, {"access", 0x23}, {"meta", 0x30}, {"go", 0x2B}, {"prev", 0x32},
    {"refresh", 0x36}, {"noop", 0x31}, {"postfield", 0x21}, {"setvar", 0x3E},
    {"select", 0x37}, {"optgroup", 0x34}, {"option", 0x35}, {"input", 0x2F},
    {"fieldset", 0x2A}, {"timer", 0x3C}, {"img", 0x2E}, {"anchor", 0x22}, {"a", 0x1C},
    {"table", 0x1F}, {"tr", 0x1E}, {"td", 0x1D}, {"em", 0x29}, {"strong", 0x39},
    {"b", 0x24}, {"i", 0x2D}, {"u", 0x3D}, {"big", 0x25}, {"small", 0x38}, {"p", 0x20},
    {"br", 0x26},
};

// Attributes WML 1.1 has no plain start token for (method, mode, ...) are
// left out, i.e. dropped, as in the original compiler.
static const struct { const char *name; int code; } ATTRS[] = {
    {"accept-charset", 0x05}, {"accesskey", 0x5E}, {"align", 0x52}, {"alt", 0x0C},
    {"class", 0x54}, {"columns", 0x53}, {"content", 0x0D}, {"domain", 0x0F},
    {"enctype", 0x5F}, {"format", 0x12}, {"height", 0x13}, {"href", ATTR_HREF},
    {"hspace", 0x14}, {"http-equiv", 0x5A}, {"id", 0x55}, {"ivalue", 0x15},
    {"iname", 0x16}, {"label", 0x18}, {"localsrc", 0x19}, {"maxlength", 0x1A},
    {"name", 0x21}, {"onenterbackward", 0x25}, {"onenterforward", 0x26},
    {"onpick", 0x24}, {"ontimer", 0x27}, {"path", 0x2A}, {"scheme", 0x2E},
    {"size", 0x31}, {"src", ATTR_SRC}, {"tabindex", 0x35}, {"title", 0x36},
    {"type", 0x37}, {"value", 0x4D}, {"vspace", 0x4E}, {"width", 0x4F},
    {"xml:lang", 0x50},
};

static int element_code(const char *name) {
    for (size_t i = 0; i < sizeof(ELEMENTS) / sizeof(ELEMENTS[0]); i++)
        if (!strcasecmp(ELEMENTS[i].name, name)) return ELEMENTS[i].code;
    return -1;
}

static int attr_code(const char *name) {
    for (size_t i = 0; i < sizeof(ATTRS) / sizeof(ATTRS[0]); i++)
        if (!strcasecmp(ATTRS[i].name, name)) return ATTRS[i].code;
    return -1;
}

static void put_str(Buf *b, const char *s) {
    b_byte(b, TOKEN_STR_I);
    b_put(b, s, strlen(s));
    b_byte(b, 0);
}

static void compile_node(Buf *b, const Node *n, const char *base) {
    if (!n->name) { put_str(b, n->text); return; }

    int code = element_code(n->name);
    if (code < 0) {
        for (int i = 0; i < n->nk; i++) compile_node(b, n->kids[i], base);
        return;
    }

    Buf attrs = { .ok = 1 }, content = { .ok = 1 };
    for (int i = 0; i < n->na; i++) {
        int ac = attr_code(n->an[i]);
        if (ac < 0) continue;
        b_byte(&attrs, (unsigned char)ac);
        const char *v = n->av[i];
        if ((ac == ATTR_HREF || ac == ATTR_SRC) && base && v[0] == '/' && v[1] != '/') {
            Buf abs = { .ok = 1 };
            b_put(&abs, base, strlen(base));
            b_put(&abs, v, strlen(v));
            put_str(&attrs, abs.d ? (char *)abs.d : v);
            free(abs.d);
        } else {
            put_str(&attrs, v);
        }
    }
    for (int i = 0; i < n->nk; i++) compile_node(&content, n->kids[i], base);

    unsigned char flag = 0;
    if (attrs.len) flag |= 0x80;
    if (content.len) flag |= 0x40;
    b_byte(b, (unsigned char)code | flag);
    if (attrs.len)   { b_put(b, attrs.d, attrs.len);     b_byte(b, TOKEN_END); }
    if (content.len) { b_put(b, content.d, content.len); b_byte(b, TOKEN_END); }
    if (!attrs.ok || !content.ok) b->ok = 0;
    free(attrs.d);
    free(content.d);
}

// A whole one-card deck: header, <wml><card id="p" title=...>, the given
// blocks and nav nodes. Version 1.1, public id WML 1.1 (0x04), UTF-8 (0x6A).
static void compile_card(Buf *b, const char *title, Node **blocks, int nb,
                         Node **nav, int nn, const char *base) {
    static const unsigned char header[] = { 0x01, 0x04, 0x6A, 0x00 };
    b_put(b, header, sizeof(header));
    b_byte(b, 0x3F | 0x40);                  // <wml>, has content
    b_byte(b, 0x27 | 0x80 | 0x40);           // <card>, attrs + content
    b_byte(b, 0x55); put_str(b, "p");        // id="p"
    b_byte(b, 0x36); put_str(b, title);      // title="..."
    b_byte(b, TOKEN_END);
    size_t before = b->len;
    for (int i = 0; i < nb; i++) compile_node(b, blocks[i], base);
    for (int i = 0; i < nn; i++) compile_node(b, nav[i], base);
    if (b->len == before) put_str(b, " ");   // a card needs some content
    b_byte(b, TOKEN_END);                    // </card>
    b_byte(b, TOKEN_END);                    // </wml>
}

static size_t card_size(const char *title, Node **blocks, int nb, Node **nav, int nn,
                        const char *base) {
    Buf b = { .ok = 1 };
    compile_card(&b, title, blocks, nb, nav, nn, base);
    size_t len = b.ok ? b.len : (size_t)-1;
    free(b.d);
    return len;
}

// ---- Pagination ----------------------------------------------------------------

typedef struct {
    Node **items;
    int n, cap;
} NodeList;

static void list_add(NodeList *l, Node *n) {
    if (!n) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        Node **ni = realloc(l->items, (size_t)cap * sizeof(*ni));
        if (!ni) return;
        l->items = ni;
        l->cap = cap;
    }
    l->items[l->n++] = n;
}

static Node *find_card(Node *n) {
    if (n->name && !strcasecmp(n->name, "card")) return n;
    for (int i = 0; i < n->nk; i++) {
        Node *c = find_card(n->kids[i]);
        if (c) return c;
    }
    return NULL;
}

// An empty copy of `like` (same tag, same attributes) whose kids are
// borrowed - a wrapper for one slice of a split block.
static Node *wrapper_like(const Node *like, Pool *pool) {
    Node *w = pool_add(pool, node_new(like->name ? like->name : "p"));
    if (!w) return NULL;
    w->borrowed = 1;
    for (int i = 0; i < like->na; i++) node_set_attr(w, like->an[i], like->av[i]);
    return w;
}

// Size of a page holding `cur`'s blocks plus `extra` (when non-NULL).
static size_t page_size_with(const char *title, NodeList *cur, Node *extra, const char *base) {
    if (extra) list_add(cur, extra);
    size_t n = card_size(title, cur->items, cur->n, NULL, 0, base);
    if (extra) cur->n--;
    return n;
}

// A wrapper like `blk` holding blk->kids[0..m) and, if `words` > 0, the
// first `words` words of text kid m.
static Node *prefix_of(const Node *blk, int m, const char *text, size_t text_cut, Pool *pool) {
    Node *w = wrapper_like(blk, pool);
    if (!w) return NULL;
    for (int i = 0; i < m; i++) node_add(w, blk->kids[i]);
    if (text && text_cut) {
        char *t = strndup(text, text_cut);
        if (t) { node_add(w, pool_add(pool, text_node(t))); free(t); }
    }
    return w;
}

// Byte offsets in `text` just past each word (each cut keeps the word's own
// trailing space out), at most `max` of them; returns how many.
static int word_cuts(const char *text, size_t *cuts, int max) {
    int n = 0;
    size_t len = strlen(text);
    for (size_t i = 1; i <= len && n < max; i++)
        if (i == len || (text[i] == ' ' && text[i - 1] != ' ')) cuts[n++] = i;
    return n;
}

#define MAX_WORDS 4096

static int has_alnum(const char *w, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (isalnum((unsigned char)w[i]) || ((unsigned char)w[i] & 0x80)) return 1;
    return 0;
}

// Whether a word (no spaces) can end a page's last line: anything with a
// letter or digit, or closing punctuation ("],", ")."), but not a bare
// separator ("|", ",") nor anything that opens ("[", "(").
static int ends_line(const char *w, size_t n) {
    if (has_alnum(w, n)) return 1;
    for (size_t i = 0; i < n; i++)
        if (strchr("[({<", w[i])) return 0;
    for (size_t i = 0; i < n; i++)
        if (!strchr("|,;:-", w[i])) return 1;
    return 0;
}

// Length of `t` up to the end of its last word that can end a line
// (trailing spaces dropped); 0 if none can.
static size_t trim_dangling_tail(const char *t) {
    size_t end = strlen(t);
    for (;;) {
        while (end > 0 && t[end - 1] == ' ') end--;
        if (end == 0) return 0;
        size_t start = end;
        while (start > 0 && t[start - 1] != ' ') start--;
        if (ends_line(t + start, end - start)) return end;
        end = start;
    }
}

// `t` past its leading spaces and bare separator words ("|", ",").
static const char *skip_leading_separators(const char *t) {
    for (;;) {
        while (*t == ' ') t++;
        size_t n = strcspn(t, " ");
        if (n == 0 || has_alnum(t, n) || strspn(t, "|,;:-") != n) return t;
        t += n;
    }
}

// Splits `blk` so its head fills what's left of the page `cur` - whole
// children first, then the boundary text child word by word. On success
// *head goes on this page, *rest starts the next, and 1 is returned; 0 when
// not even a word fits (or blk has no children to split).
static int split_to_fit(NodeList *cur, Node *blk, const char *title, size_t limit,
                        const char *base, Pool *pool, Node **head, Node **rest) {
    if (!blk->name || blk->nk == 0) return 0;

    // Largest m such that kids[0..m) fit.
    int m = 0;
    while (m < blk->nk) {
        Node *probe = prefix_of(blk, m + 1, NULL, 0, pool);
        if (!probe || page_size_with(title, cur, probe, base) > limit) break;
        m++;
    }

    const Node *edge = m < blk->nk ? blk->kids[m] : NULL;
    size_t cut = 0;
    size_t *cuts = (edge && !edge->name) ? malloc(MAX_WORDS * sizeof(*cuts)) : NULL;
    if (cuts) {
        int nw = word_cuts(edge->text, cuts, MAX_WORDS);
        int lo = 0, hi = nw;   // binary search: most words that still fit
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            Node *probe = prefix_of(blk, m, edge->text, cuts[mid - 1], pool);
            if (probe && page_size_with(title, cur, probe, base) <= limit) lo = mid; else hi = mid - 1;
        }
        if (lo > 0) cut = cuts[lo - 1];
        free(cuts);
    }
    if (m == blk->nk) return 0;   // it all fits after all - caller adds it whole

    // Tidy the seam. A cut between children (no text cut) whose head ends in
    // a text child like " | " or "], [" would leave a dangling separator or
    // opening bracket on this page: back the cut up into that child instead,
    // to just after its last word that can end a line (see ends_line()).
    if (cut == 0 && m > 0 && !blk->kids[m - 1]->name) {
        const char *t = blk->kids[m - 1]->text;
        size_t keep = trim_dangling_tail(t);
        if (keep < strlen(t)) {
            m--;
            edge = blk->kids[m];
            cut = keep;   // 0 = the whole child moves to the next page
        }
    }
    if (m == 0 && cut == 0) return 0;

    *head = prefix_of(blk, m, edge && !edge->name ? edge->text : NULL, cut, pool);
    Node *r = wrapper_like(blk, pool);
    if (!*head || !r) return 0;
    int from = m;
    if (edge && !edge->name) {
        // The edge text's unplaced remainder starts the next page - minus
        // any bare separators ("|", ",") it would open with.
        const char *after = skip_leading_separators(edge->text + cut);
        if (*after) node_add(r, pool_add(pool, text_node(after)));
        from = m + 1;
    }
    for (int i = from; i < blk->nk; i++) node_add(r, blk->kids[i]);
    *rest = r->nk ? r : NULL;
    return 1;
}

// Text a block shows, roughly: a split that would leave this little on the
// current page (a dangling word or two) isn't worth it.
#define MIN_SPLIT_HEAD_BYTES 24

int wbxml_page_param(const char *uri) {
    const char *p = strstr(uri, "__page=");
    while (p && p > uri && p[-1] != '?' && p[-1] != '&') p = strstr(p + 1, "__page=");
    return p ? atoi(p + 7) : 0;
}

void wbxml_set_page_param(const char *uri, int page, char *out, size_t out_size) {
    char base[1024];
    snprintf(base, sizeof(base), "%s", uri);
    char *hash = strchr(base, '#');
    if (hash) *hash = '\0';

    // Drop any existing __page=N (and its separator).
    char *p = strstr(base, "__page=");
    while (p && p > base && p[-1] != '?' && p[-1] != '&') p = strstr(p + 1, "__page=");
    if (p) {
        char *end = p + 7;
        while (isdigit((unsigned char)*end)) end++;
        if (*end == '&') end++;
        else if (p[-1] == '&' || p[-1] == '?') p--;
        memmove(p, end, strlen(end) + 1);
        size_t bl = strlen(base);
        if (bl && (base[bl - 1] == '?' || base[bl - 1] == '&')) base[bl - 1] = '\0';
    }

    if (page <= 0) snprintf(out, out_size, "%s", base);
    else snprintf(out, out_size, "%s%s__page=%d", base, strchr(base, '?') ? "&" : "?", page);
}

static Node *nav_link(const char *label, const char *href, Pool *pool) {
    Node *p = pool_add(pool, node_new("p"));
    Node *a = pool_add(pool, node_new("a"));
    Node *t = pool_add(pool, text_node(label));
    if (!p || !a || !t) return NULL;
    p->borrowed = 1;
    a->borrowed = 1;
    node_set_attr(a, "href", href);
    node_add(a, t);
    node_add(p, a);
    return p;
}

// "scheme://host[:port]" of `uri`, or "" when it isn't absolute.
static void uri_base(const char *uri, char *out, size_t out_size) {
    out[0] = '\0';
    const char *sep = strstr(uri, "://");
    if (!sep) return;
    const char *path = strchr(sep + 3, '/');
    size_t n = path ? (size_t)(path - uri) : strlen(uri);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, uri, n);
    out[n] = '\0';
}

// Copies at most `max` bytes of UTF-8 `s`, never splitting a character.
static void utf8_truncate(const char *s, size_t max, char *out, size_t out_size) {
    size_t n = strlen(s);
    if (n > max) n = max;
    if (n >= out_size) n = out_size - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    memcpy(out, s, n);
    out[n] = '\0';
}

// ---- Shared pagination (WBXML for the gateway, WML text for HTTP) ------------------

typedef struct {
    Node *root;
    Pool pool;
    NodeList queue;
    NodeList *pagev;
    int pages;
    int page;               // the requested one, clamped
    char base_title[48];
    char base[512];         // scheme://host prefixed to site-relative links ("" = none)
    Node *nav[2];
    int nn;
    char page_title[96];
} Paged;

static void paged_free(Paged *pg) {
    for (int k = 0; k < pg->pages; k++) free(pg->pagev[k].items);
    free(pg->pagev);
    free(pg->queue.items);
    pool_free(&pg->pool);
    node_free(pg->root);
}

// Parses `src` (UTF-8 WML), packs its first card into pages of at most
// `page_bytes` compiled bytes and prepares page `page` (title and Prev/Next
// links built from `uri`). `absolute_links`: prefix site-relative links with
// uri's scheme://host (the gateway does; HTTP pages keep them relative).
static int paged_build(Paged *pg, const char *src, const char *uri, int absolute_links,
                       int page, size_t page_bytes) {
    memset(pg, 0, sizeof(*pg));
    pg->root = parse_wml(src, strlen(src));
    if (!pg->root) return -1;

    if (absolute_links) uri_base(uri, pg->base, sizeof(pg->base));

    // Pages are always measured as if every site-relative link carried a
    // scheme://host - a reference one, or the real one when that's longer -
    // so a deck splits at the same points whether it goes out with absolute
    // links (the gateway) or relative ones (HTTP); only the gateway's own
    // output actually uses pg->base.
    static const char reference_base[] = "http://000.000.000.000:00";
    const char *basep = strlen(pg->base) > sizeof(reference_base) - 1 ? pg->base : reference_base;

    Node *card = find_card(pg->root);
    if (!card) card = pg->root;
    utf8_truncate(node_attr(card, "title") ? node_attr(card, "title") : "", 30,
                  pg->base_title, sizeof(pg->base_title));

    // Pages are sized against the longest title they could carry (" (99/99)"
    // appended), so the final "(x/N)" never pushes one past the limit.
    char title[64];
    snprintf(title, sizeof(title), "%s (99/99)", pg->base_title);

    // Room for the worst-case Prev+Next links, so a full page plus its
    // navigation still fits.
    // Measured from the path alone (host dropped, then measured with basep
    // like any relative link), so both paths reserve the very same room.
    const char *uri_path = uri;
    const char *scheme = strstr(uri, "://");
    if (scheme) {
        uri_path = strchr(scheme + 3, '/');
        if (!uri_path) uri_path = "/";
    }
    char prev_href[1100], next_href[1100];
    wbxml_set_page_param(uri_path, 98, prev_href, sizeof(prev_href));
    wbxml_set_page_param(uri_path, 99, next_href, sizeof(next_href));
    Node *probe[2] = { nav_link("<< Prev", prev_href, &pg->pool), nav_link("Next >>", next_href, &pg->pool) };
    size_t nav_cost = card_size(title, NULL, 0, probe, 2, basep) - card_size(title, NULL, 0, NULL, 0, basep);
    // +3: the empty card it's measured against carries a 3-byte filler
    // string (compile_card()) that a card with nav links doesn't.
    nav_cost += 3;
    size_t limit = page_bytes > nav_cost + 128 ? page_bytes - nav_cost : 128;

    // Fill each page to the limit: a block that doesn't fit what's left is
    // split there (see split_to_fit()), its rest starting the next page.
    for (int i = 0; i < card->nk; i++)
        if (card->kids[i]->name) list_add(&pg->queue, card->kids[i]);

    int pcap = 0;
    NodeList cur = {0};
    for (int i = 0; i < pg->queue.n; ) {
        Node *blk = pg->queue.items[i];
        if (page_size_with(title, &cur, blk, basep) <= limit) {
            list_add(&cur, blk);
            i++;
            continue;
        }
        Node *head = NULL, *rest = NULL;
        size_t before = card_size(title, cur.items, cur.n, NULL, 0, basep);
        int split = split_to_fit(&cur, blk, title, limit, basep, &pg->pool, &head, &rest);
        if (split && cur.n && page_size_with(title, &cur, head, basep) - before < MIN_SPLIT_HEAD_BYTES)
            split = 0;
        if (split) {
            list_add(&cur, head);
            if (rest) pg->queue.items[i] = rest; else i++;
        } else if (cur.n == 0) {
            list_add(&cur, blk);   // unsplittable and oversized: send it as-is
            i++;
        }
        if (pg->pages == pcap) {
            pcap = pcap ? pcap * 2 : 8;
            NodeList *np = realloc(pg->pagev, (size_t)pcap * sizeof(*np));
            if (!np) { free(cur.items); return -1; }
            pg->pagev = np;
        }
        pg->pagev[pg->pages++] = cur;
        cur = (NodeList){0};
    }
    if (cur.n || pg->pages == 0) {
        if (pg->pages == pcap) {
            NodeList *np = realloc(pg->pagev, (size_t)(pcap + 1) * sizeof(*np));
            if (!np) { free(cur.items); return -1; }
            pg->pagev = np;
        }
        pg->pagev[pg->pages++] = cur;
    }

    pg->page = page < 0 ? 0 : (page >= pg->pages ? pg->pages - 1 : page);
    if (pg->page > 0) {
        wbxml_set_page_param(uri, pg->page - 1, prev_href, sizeof(prev_href));
        pg->nav[pg->nn++] = nav_link("<< Prev", prev_href, &pg->pool);
    }
    if (pg->page < pg->pages - 1) {
        wbxml_set_page_param(uri, pg->page + 1, next_href, sizeof(next_href));
        pg->nav[pg->nn++] = nav_link("Next >>", next_href, &pg->pool);
    }
    if (pg->pages > 1)
        snprintf(pg->page_title, sizeof(pg->page_title), "%s (%d/%d)", pg->base_title, pg->page + 1, pg->pages);
    else
        snprintf(pg->page_title, sizeof(pg->page_title), "%s", pg->base_title);
    return 0;
}

int wbxml_compile_page(const char *wml, size_t wml_len, int latin1,
                       const char *uri, int page, size_t page_bytes, WbxmlBuf *out) {
    out->data = NULL;
    out->len = 0;

    char *src = latin1 ? latin1_to_utf8(wml, wml_len) : strndup(wml, wml_len);
    if (!src) return -1;
    Paged pg;
    int built = paged_build(&pg, src, uri, 1, page, page_bytes);
    free(src);

    int rc = -1;
    if (built == 0) {
        Buf b = { .ok = 1 };
        compile_card(&b, pg.page_title, pg.pagev[pg.page].items, pg.pagev[pg.page].n,
                     pg.nav, pg.nn, pg.base[0] ? pg.base : NULL);
        if (b.ok) { out->data = b.d; out->len = b.len; rc = 0; } else free(b.d);
    }
    paged_free(&pg);
    return rc;
}

// ---- WML text output ------------------------------------------------------------------

static void ser_escape(Buf *b, const char *s, int attr) {
    for (; *s; s++) {
        switch (*s) {
            case '&': b_put(b, "&amp;", 5); break;
            case '<': b_put(b, "&lt;", 4); break;
            case '>': b_put(b, "&gt;", 4); break;
            case '"': if (attr) { b_put(b, "&quot;", 6); break; } /* fall through */
            default:  b_byte(b, (unsigned char)*s);
        }
    }
}

static void ser_attr(Buf *b, const char *name, const char *value) {
    b_byte(b, ' ');
    b_put(b, name, strlen(name));
    b_put(b, "=\"", 2);
    ser_escape(b, value, 1);
    b_byte(b, '"');
}

static void ser_node(Buf *b, const Node *n) {
    if (!n->name) { ser_escape(b, n->text, 0); return; }
    b_byte(b, '<');
    b_put(b, n->name, strlen(n->name));
    for (int i = 0; i < n->na; i++) ser_attr(b, n->an[i], n->av[i]);
    if (n->nk == 0) { b_put(b, "/>", 2); return; }
    b_byte(b, '>');
    for (int i = 0; i < n->nk; i++) ser_node(b, n->kids[i]);
    b_put(b, "</", 2);
    b_put(b, n->name, strlen(n->name));
    b_byte(b, '>');
}

int wml_paginate(const char *wml, size_t wml_len, int latin1, const char *uri, int page,
                 size_t page_bytes, const char *xml_encoding, char **out) {
    *out = NULL;
    char *src = latin1 ? latin1_to_utf8(wml, wml_len) : strndup(wml, wml_len);
    if (!src) return -1;
    Paged pg;
    int built = paged_build(&pg, src, uri, 0, page, page_bytes);
    free(src);
    if (built != 0) { paged_free(&pg); return -1; }
    if (pg.pages <= 1) { paged_free(&pg); return 0; }

    Buf b = { .ok = 1 };
    b_put(&b, "<?xml version=\"1.0\" encoding=\"", 30);
    b_put(&b, xml_encoding, strlen(xml_encoding));
    static const char head[] =
        "\"?>\n"
        "<!DOCTYPE wml PUBLIC \"-//WAPFORUM//DTD WML 1.1//EN\" \"http://www.wapforum.org/DTD/wml_1.1.xml\">\n"
        "<wml>\n<card id=\"p\"";
    b_put(&b, head, sizeof(head) - 1);
    ser_attr(&b, "title", pg.page_title);
    b_put(&b, ">\n", 2);
    for (int i = 0; i < pg.pagev[pg.page].n; i++) { ser_node(&b, pg.pagev[pg.page].items[i]); b_byte(&b, '\n'); }
    for (int i = 0; i < pg.nn; i++) { ser_node(&b, pg.nav[i]); b_byte(&b, '\n'); }
    b_put(&b, "</card>\n</wml>\n", 15);

    int rc = -1;
    if (b.ok) { *out = (char *)b.d; rc = 0; } else free(b.d);
    paged_free(&pg);
    return rc;
}

int wbxml_message_card(const char *title, const char *text, WbxmlBuf *out) {
    Pool pool = {0};
    Node *p = pool_add(&pool, node_new("p"));
    Node *t = pool_add(&pool, text_node(text));
    int rc = -1;
    out->data = NULL;
    out->len = 0;
    if (p && t) {
        p->borrowed = 1;
        node_add(p, t);
        Buf b = { .ok = 1 };
        compile_card(&b, title, &p, 1, NULL, 0, NULL);
        if (b.ok) { out->data = b.d; out->len = b.len; rc = 0; } else free(b.d);
    }
    pool_free(&pool);
    return rc;
}
