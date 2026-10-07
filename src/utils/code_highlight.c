#include "code_highlight.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---- Language table --------------------------------------------------------

typedef struct {
    const char *key;               // the editor <select> value
    const char *display;           // the editor <select> label
    const char *const *keywords;   // NULL-terminated, NULL = none
    const char *line_comment[3];   // NULL-terminated markers
    const char *block_open;        // NULL = no block comments
    const char *block_close;
    const char *quotes;            // string delimiters
    const char *pre_markers[4];    // NULL-terminated, emitted as "pre" (PHP's <?php / ?>)
    int nocase;                    // keywords match regardless of case (SQL, PHP)
    int c_preproc;                 // '#' opening a line starts a directive
    int dollar_vars;               // "$name", "${...}" are variables
    int shell_specials;            // "$1", "$#", "$?", ... are variables too (Bash)
    int dollar_ident;              // '$' is a plain identifier character (JS)
    int hash_word_start;           // '#' only comments at a word boundary (Bash: "$#", "a#b")
    int multiline_strings;         // every string may span lines (Bash)
    int triple_quotes;             // """ / ''' block strings (Python)
    int css;                       // property names, @rules, #hex colors, !important
    int html;                      // markup scanner instead of the code scanner
} LangDef;

static const char *const KW_JS[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue",
    "debugger", "default", "delete", "do", "else", "export", "extends", "false",
    "finally", "for", "from", "function", "get", "if", "import", "in",
    "instanceof", "let", "new", "null", "of", "return", "set", "static", "super",
    "switch", "this", "throw", "true", "try", "typeof", "undefined", "var",
    "void", "while", "with", "yield", NULL
};

static const char *const KW_C[] = {
    "auto", "bool", "break", "case", "catch", "char", "class", "const",
    "constexpr", "continue", "default", "delete", "do", "double", "else", "enum",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
    "long", "namespace", "new", "nullptr", "NULL", "operator", "private",
    "protected", "public", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "template", "this", "throw", "true",
    "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "while", NULL
};

static const char *const KW_PYTHON[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else", "except", "finally", "for",
    "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not",
    "or", "pass", "raise", "return", "self", "try", "while", "with", "yield", NULL
};

static const char *const KW_PHP[] = {
    "abstract", "and", "array", "as", "break", "callable", "case", "catch",
    "class", "clone", "const", "continue", "declare", "default", "do", "echo",
    "else", "elseif", "empty", "endfor", "endforeach", "endif", "endswitch",
    "endwhile", "extends", "false", "final", "finally", "fn", "for", "foreach",
    "function", "global", "goto", "if", "implements", "include", "include_once",
    "instanceof", "interface", "isset", "list", "match", "namespace", "new",
    "null", "or", "print", "private", "protected", "public", "readonly",
    "require", "require_once", "return", "static", "switch", "throw", "trait",
    "true", "try", "unset", "use", "var", "while", "xor", "yield", NULL
};

static const char *const KW_BASH[] = {
    "alias", "break", "case", "cd", "continue", "declare", "do", "done", "echo",
    "elif", "else", "esac", "eval", "exec", "exit", "export", "false", "fi",
    "for", "function", "if", "in", "local", "printf", "read", "readonly",
    "return", "select", "set", "shift", "source", "test", "then", "time",
    "trap", "true", "unset", "until", "while", NULL
};

static const char *const KW_SQL[] = {
    "ADD", "ALL", "ALTER", "AND", "AS", "ASC", "AVG", "BETWEEN", "BOOLEAN", "BY",
    "CASE", "CHAR", "COUNT", "CREATE", "DATABASE", "DATE", "DEFAULT", "DELETE",
    "DESC", "DISTINCT", "DROP", "ELSE", "END", "EXISTS", "FALSE", "FOREIGN",
    "FROM", "FULL", "GRANT", "GROUP", "HAVING", "IN", "INDEX", "INNER", "INSERT",
    "INT", "INTEGER", "INTO", "IS", "JOIN", "KEY", "LEFT", "LIKE", "LIMIT", "MAX",
    "MIN", "NOT", "NULL", "OFFSET", "ON", "OR", "ORDER", "OUTER", "PRIMARY",
    "REFERENCES", "RIGHT", "SELECT", "SET", "SUM", "TABLE", "TEXT", "THEN",
    "TRUE", "UNION", "UNIQUE", "UPDATE", "USE", "VALUES", "VARCHAR", "VIEW",
    "WHEN", "WHERE", NULL
};

static const LangDef LANGS[] = {
    { .key = "javascript", .display = "JavaScript", .keywords = KW_JS,
      .line_comment = { "//" }, .block_open = "/*", .block_close = "*/",
      .quotes = "\"'`", .dollar_ident = 1 },
    { .key = "html", .display = "HTML", .html = 1 },
    { .key = "c", .display = "C / C++", .keywords = KW_C,
      .line_comment = { "//" }, .block_open = "/*", .block_close = "*/",
      .quotes = "\"'", .c_preproc = 1 },
    { .key = "python", .display = "Python", .keywords = KW_PYTHON,
      .line_comment = { "#" }, .quotes = "\"'", .triple_quotes = 1 },
    { .key = "css", .display = "CSS", .block_open = "/*", .block_close = "*/",
      .quotes = "\"'", .css = 1 },
    { .key = "php", .display = "PHP", .keywords = KW_PHP,
      .line_comment = { "//", "#" }, .block_open = "/*", .block_close = "*/",
      .quotes = "\"'", .pre_markers = { "<?php", "<?=", "?>" },
      .nocase = 1, .dollar_vars = 1 },
    { .key = "bash", .display = "Bash", .keywords = KW_BASH,
      .line_comment = { "#" }, .quotes = "\"'`",
      .dollar_vars = 1, .shell_specials = 1, .hash_word_start = 1, .multiline_strings = 1 },
    { .key = "sql", .display = "SQL", .keywords = KW_SQL,
      .line_comment = { "--" }, .block_open = "/*", .block_close = "*/",
      .quotes = "\"'`", .nocase = 1 },
};

static const LangDef *find_lang(const char *key) {
    if (!key || !key[0]) return NULL;
    for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++)
        if (strcmp(LANGS[i].key, key) == 0) return &LANGS[i];
    return NULL;
}

const char *code_highlight_display_name(const char *lang) {
    const LangDef *L = find_lang(lang);
    return L ? L->display : "Plain text";
}

// ---- Output ----------------------------------------------------------------

typedef struct {
    char  *buf;
    size_t len, cap;
    int    ok;
} Sb;

static void sb_putn(Sb *sb, const char *s, size_t n) {
    if (!sb->ok) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 1024;
        while (sb->len + n + 1 > cap) cap *= 2;
        char *nb = realloc(sb->buf, cap);
        if (!nb) { sb->ok = 0; return; }
        sb->buf = nb;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(Sb *sb, const char *s) { sb_putn(sb, s, strlen(s)); }

#define LINE_OPEN  "<span class=\"boat-rudder-code-line\">"
#define LINE_CLOSE "</span>"

// Output target: CSS classes and per-line spans (pal == NULL, epoch 3), or
// <font color> tags and server-written line numbers (pal set, epoch 2).
typedef struct {
    Sb sb;
    const CodeHighlightPalette *pal;
    int line_no;
    int digits; // width of the largest line number, for right alignment
} Out;

static const char *palette_color(const CodeHighlightPalette *pal, const char *cls) {
    switch (cls[0]) {
        case 'k': case 'f': return pal->keyword;               // kw, fn
        case 's':           return pal->string;                // str
        case 'c':           return pal->comment;               // com
        case 'n':           return pal->number;                // num
        case 'v':           return pal->variable;              // var
        default:            return pal->tag;                   // pre, tag, attr
    }
}

static void line_start(Out *o) {
    o->line_no++;
    if (!o->pal) {
        sb_puts(&o->sb, LINE_OPEN);
        return;
    }
    if (!o->pal->numbers) return;
    char num[48];
    snprintf(num, sizeof(num), "%*d | ", o->digits, o->line_no);
    if (o->pal->no_color) {
        sb_puts(&o->sb, num);
        return;
    }
    sb_puts(&o->sb, "<font color=\"");
    sb_puts(&o->sb, o->pal->line_number);
    sb_puts(&o->sb, "\">");
    sb_puts(&o->sb, num);
    sb_puts(&o->sb, "</font>");
}

static void line_end(Out *o) {
    if (!o->pal) sb_puts(&o->sb, LINE_CLOSE);
}

static void token_open(Out *o, const char *cls) {
    if (!o->pal) {
        sb_puts(&o->sb, "<span class=\"boat-rudder-code-token--");
        sb_puts(&o->sb, cls);
        sb_puts(&o->sb, "\">");
        return;
    }
    // Keywords bold and comments italic in every epoch, color or not - epoch
    // 0's readers have no color, but bold and italics still read there (Lynx
    // highlights them, w3m/ELinks/Cello render them).
    if (!o->pal->no_color) {
        sb_puts(&o->sb, "<font color=\"");
        sb_puts(&o->sb, palette_color(o->pal, cls));
        sb_puts(&o->sb, "\">");
    }
    if (strcmp(cls, "kw") == 0) sb_puts(&o->sb, "<b>");
    else if (strcmp(cls, "com") == 0) sb_puts(&o->sb, "<i>");
}

static void token_close(Out *o, const char *cls) {
    if (!o->pal) {
        sb_puts(&o->sb, "</span>");
        return;
    }
    if (strcmp(cls, "kw") == 0) sb_puts(&o->sb, "</b>");
    else if (strcmp(cls, "com") == 0) sb_puts(&o->sb, "</i>");
    if (!o->pal->no_color) sb_puts(&o->sb, "</font>");
}

// Escapes and appends s[0..n), inside a token's markup when `cls` is set. A
// newline ends the current line and starts the next; a token crossing it
// (block comment, multi-line string) is closed before and reopened after, so
// its markup never straddles lines.
static void emit(Out *o, const char *s, size_t n, const char *cls) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        if (j > i) {
            if (cls) token_open(o, cls);
            size_t run = i;
            for (size_t k = i; k < j; k++) {
                const char *ent = NULL;
                switch (s[k]) {
                    case '&': ent = "&amp;";  break;
                    case '<': ent = "&lt;";   break;
                    case '>': ent = "&gt;";   break;
                    case '"': ent = "&quot;"; break;
                }
                if (ent) {
                    sb_putn(&o->sb, s + run, k - run);
                    sb_puts(&o->sb, ent);
                    run = k + 1;
                }
            }
            sb_putn(&o->sb, s + run, j - run);
            if (cls) token_close(o, cls);
        }
        if (j < n) {
            line_end(o);
            sb_puts(&o->sb, (o->pal && o->pal->br_newlines) ? "<br>" : "\n");
            line_start(o);
            j++;
        }
        i = j;
    }
}

// ---- Scanners --------------------------------------------------------------

// Every scanner walks `s` with `i`, leaving untokenized text pending from
// `plain` until the next token (or the end) flushes it as a plain run.
#define FLUSH()          do { if (i > plain) emit(sb, s + plain, i - plain, NULL); } while (0)
#define TOKEN(len, cls)  do { FLUSH(); emit(sb, s + i, (len), (cls)); i += (len); plain = i; } while (0)

static int starts_with(const char *s, size_t avail, const char *m) {
    size_t ml = strlen(m);
    return ml <= avail && strncmp(s, m, ml) == 0;
}

// Length from s[from] to the end of the first `m` at or after it, or to the
// end of the input if `m` never shows up (an unterminated comment/string
// simply runs to the end, same as in an editor).
static size_t span_to(const char *s, size_t from, size_t n, const char *m) {
    size_t ml = strlen(m);
    for (size_t j = from; j + ml <= n; j++)
        if (strncmp(s + j, m, ml) == 0) return j + ml;
    return n;
}

static int is_ident(const LangDef *L, char c) {
    return isalnum((unsigned char)c) || c == '_' ||
           (L->css && c == '-') || (L->dollar_ident && c == '$');
}

static int is_ident_start(const LangDef *L, const char *s, size_t i, size_t n) {
    char c = s[i];
    if (isalpha((unsigned char)c) || c == '_' || (L->dollar_ident && c == '$')) return 1;
    return L->css && c == '-' && i + 1 < n &&
           (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '-');
}

static int is_keyword(const LangDef *L, const char *w, size_t len) {
    if (!L->keywords) return 0;
    for (const char *const *k = L->keywords; *k; k++) {
        if (strlen(*k) != len) continue;
        if ((L->nocase ? strncasecmp(*k, w, len) : strncmp(*k, w, len)) == 0) return 1;
    }
    return 0;
}

static int line_comment_at(const LangDef *L, const char *s, size_t i, size_t n) {
    for (int k = 0; k < 3 && L->line_comment[k]; k++) {
        const char *m = L->line_comment[k];
        if (!starts_with(s + i, n - i, m)) continue;
        if (L->hash_word_start && m[0] == '#' && i > 0 && !isspace((unsigned char)s[i - 1]))
            continue;
        return 1;
    }
    return 0;
}

static int at_line_start(const char *s, size_t i) {
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    return i == 0 || s[i - 1] == '\n';
}

static size_t string_len(const LangDef *L, const char *s, size_t i, size_t n) {
    char q = s[i];
    if (L->triple_quotes && i + 2 < n && s[i + 1] == q && s[i + 2] == q) {
        const char triple[4] = { q, q, q, '\0' };
        return span_to(s, i + 3, n, triple) - i;
    }
    int multiline = L->multiline_strings || q == '`';
    for (size_t j = i + 1; j < n; j++) {
        if (s[j] == '\\' && j + 1 < n) { j++; continue; }
        if (s[j] == q) return j + 1 - i;
        // An unterminated quote (an apostrophe in prose, a typo) stops at
        // the end of its line instead of coloring the rest of the block.
        if (s[j] == '\n' && !multiline) return j - i;
    }
    return n - i;
}

static void scan_code(Out *sb, const char *s, size_t n, const LangDef *L) {
    size_t i = 0, plain = 0;
    int depth = 0; // CSS brace depth: property names only exist inside a rule

    while (i < n) {
        char c = s[i];

        if (L->block_open && starts_with(s + i, n - i, L->block_open)) {
            TOKEN(span_to(s, i + strlen(L->block_open), n, L->block_close) - i, "com");
            continue;
        }

        if (line_comment_at(L, s, i, n)) {
            size_t j = i;
            while (j < n && s[j] != '\n') j++;
            TOKEN(j - i, "com");
            continue;
        }

        if (L->c_preproc && c == '#' && at_line_start(s, i)) {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            size_t word = j;
            while (j < n && isalpha((unsigned char)s[j])) j++;
            int is_include = (j - word == 7 && strncmp(s + word, "include", 7) == 0);
            TOKEN(j - i, "pre");
            if (is_include) {
                while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
                if (i < n && s[i] == '<') {
                    size_t k = i;
                    while (k < n && s[k] != '>' && s[k] != '\n') k++;
                    if (k < n && s[k] == '>') TOKEN(k + 1 - i, "str");
                }
            }
            continue;
        }

        int marker = 0;
        for (int k = 0; k < 4 && L->pre_markers[k]; k++) {
            if (starts_with(s + i, n - i, L->pre_markers[k])) {
                TOKEN(strlen(L->pre_markers[k]), "pre");
                marker = 1;
                break;
            }
        }
        if (marker) continue;

        if (L->quotes && c && strchr(L->quotes, c)) {
            TOKEN(string_len(L, s, i, n), "str");
            continue;
        }

        if (L->dollar_vars && c == '$' && i + 1 < n) {
            char d = s[i + 1];
            if (d == '{') {
                size_t j = i + 2;
                while (j < n && s[j] != '}' && s[j] != '\n') j++;
                if (j < n && s[j] == '}') { TOKEN(j + 1 - i, "var"); continue; }
            } else if (isalpha((unsigned char)d) || d == '_') {
                size_t j = i + 1;
                while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
                TOKEN(j - i, "var");
                continue;
            } else if (L->shell_specials && (isdigit((unsigned char)d) || strchr("#?@*!$-", d))) {
                TOKEN(2, "var");
                continue;
            }
        }

        if (isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]) &&
             (i == 0 || !is_ident(L, s[i - 1])))) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_' ||
                             (L->css && s[j] == '%')))
                j++;
            TOKEN(j - i, "num");
            continue;
        }

        if (L->css) {
            if (c == '@' && i + 1 < n && isalpha((unsigned char)s[i + 1])) {
                size_t j = i + 1;
                while (j < n && is_ident(L, s[j])) j++;
                TOKEN(j - i, "kw");
                continue;
            }
            if (c == '#' && depth > 0 && i + 1 < n && isxdigit((unsigned char)s[i + 1])) {
                size_t j = i + 1;
                while (j < n && isxdigit((unsigned char)s[j])) j++;
                TOKEN(j - i, "num");
                continue;
            }
            if (c == '!' && starts_with(s + i, n - i, "!important")) {
                TOKEN(10, "kw");
                continue;
            }
        }

        if (is_ident_start(L, s, i, n)) {
            size_t j = i + 1;
            while (j < n && is_ident(L, s[j])) j++;
            size_t len = j - i;

            const char *cls = NULL;
            if (L->css && depth > 0) {
                size_t k = j;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == ':') cls = "attr";
            }
            if (!cls && is_keyword(L, s + i, len)) cls = "kw";
            if (!cls && j < n && s[j] == '(') cls = "fn";

            if (cls) TOKEN(len, cls);
            else i = j;
            continue;
        }

        if (L->css) {
            if (c == '{') depth++;
            else if (c == '}' && depth > 0) depth--;
        }
        i++;
    }
    FLUSH();
}

static void scan_html(Out *sb, const char *s, size_t n) {
    size_t i = 0, plain = 0;

    while (i < n) {
        if (starts_with(s + i, n - i, "<!--")) {
            TOKEN(span_to(s, i + 4, n, "-->") - i, "com");
            continue;
        }

        if (s[i] == '<' && i + 1 < n &&
            (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
            size_t j = i + 1;
            if (s[j] == '/' || s[j] == '!') j++;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == ':')) j++;
            TOKEN(j - i, "tag");

            while (i < n && s[i] != '>') {
                char c = s[i];
                if (c == '"' || c == '\'') {
                    size_t k = i + 1;
                    while (k < n && s[k] != c) k++;
                    TOKEN((k < n ? k + 1 : n) - i, "str");
                    continue;
                }
                if (isalpha((unsigned char)c) || c == '_' || c == ':' || c == '@') {
                    size_t k = i + 1;
                    while (k < n && (isalnum((unsigned char)s[k]) || strchr("-_:.@", s[k]))) k++;
                    TOKEN(k - i, "attr");
                    continue;
                }
                if (c == '/' && i + 1 < n && s[i + 1] == '>') break;
                i++;
            }
            if (i < n) TOKEN(s[i] == '/' ? 2 : 1, "tag");
            continue;
        }
        i++;
    }
    FLUSH();
}

#undef FLUSH
#undef TOKEN

// ---- Entry point -----------------------------------------------------------

static char *highlight(const char *code, const char *lang, const CodeHighlightPalette *pal) {
    if (!code) code = "";

    // CRLF from a Windows-pasted snippet would otherwise leave a stray '\r'
    // at the end of every line; trailing blank lines would get numbers of
    // their own under nothing.
    size_t n = strlen(code);
    char *src = malloc(n + 1);
    if (!src) return NULL;
    size_t len = 0;
    for (size_t k = 0; k < n; k++)
        if (code[k] != '\r') src[len++] = code[k];
    while (len > 0 && src[len - 1] == '\n') len--;
    src[len] = '\0';

    Out o = { .sb = { .ok = 1 }, .pal = pal };
    int lines = 1;
    for (size_t k = 0; k < len; k++) if (src[k] == '\n') lines++;
    for (int v = lines; v > 0; v /= 10) o.digits++;

    line_start(&o);
    const LangDef *L = find_lang(lang);
    if (!L)           emit(&o, src, len, NULL);
    else if (L->html) scan_html(&o, src, len);
    else              scan_code(&o, src, len, L);
    line_end(&o);
    free(src);

    if (!o.sb.ok) {
        free(o.sb.buf);
        return NULL;
    }
    return o.sb.buf;
}

char *code_highlight_html(const char *code, const char *lang) {
    return highlight(code, lang, NULL);
}

char *code_highlight_html_fonts(const char *code, const char *lang,
                                const CodeHighlightPalette *palette) {
    return highlight(code, lang, palette);
}
