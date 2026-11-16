#include "terminal_session.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static size_t skip_spaces(const char *line, size_t i, size_t len) {
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    return i;
}

// "$ ls", "# apt update", "user@host:~$ make": the first word ends in the
// prompt sign and a space (or the line's end) follows it. A bare "$" or "#"
// line is an empty prompt; "$HOME", "#include" or the output "50% done"
// (a digit before the sign) aren't one.
static size_t unix_prompt(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && line[i] != ' ' && line[i] != '\t') i++;
    if (i == 0) return 0;
    char sign = line[i - 1];
    if (sign != '$' && sign != '#' && sign != '%') return 0;
    if (i > 1 && isdigit((unsigned char)line[i - 2])) return 0;
    return skip_spaces(line, i, len);
}

// "C:\>", "C:\GAMES>", "C:\Program Files>": a drive, ":\", a path up to
// the first '>'.
static size_t dos_prompt(const char *line, size_t len) {
    if (len < 4 || !isalpha((unsigned char)line[0]) || line[1] != ':' || line[2] != '\\') return 0;
    for (size_t i = 3; i < len; i++)
        if (line[i] == '>') return skip_spaces(line, i + 1, len);
    return 0;
}

// "PS>", "PS C:\Users\me>".
static size_t powershell_prompt(const char *line, size_t len) {
    if (len < 3 || strncmp(line, "PS", 2) != 0) return 0;
    if (line[2] == '>') return skip_spaces(line, 3, len);
    if (line[2] != ' ') return 0;
    for (size_t i = 3; i < len; i++)
        if (line[i] == '>') return skip_spaces(line, i + 1, len);
    return 0;
}

size_t terminal_prompt_len(const char *line, size_t len, const char *shell) {
    if (shell && strcmp(shell, "dos") == 0) return dos_prompt(line, len);
    if (shell && strcmp(shell, "powershell") == 0) return powershell_prompt(line, len);
    return unix_prompt(line, len);
}

int terminal_has_prompt(const char *text, const char *shell) {
    for (const char *line = text ? text : ""; *line; ) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (terminal_prompt_len(line, len, shell)) return 1;
        if (!end) break;
        line = end + 1;
    }
    return 0;
}

const char *terminal_shell_prompt(const char *shell) {
    if (shell && strcmp(shell, "dos") == 0) return "C:\\>";
    if (shell && strcmp(shell, "powershell") == 0) return "PS> ";
    return "$ ";
}

const char *terminal_shell_title(const char *shell) {
    if (shell && strcmp(shell, "dos") == 0) return "MS-DOS";
    if (shell && strcmp(shell, "powershell") == 0) return "PowerShell";
    return "Terminal";
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

static void sb_escape(Sb *sb, const char *s, size_t n) {
    size_t run = 0;
    for (size_t k = 0; k < n; k++) {
        const char *ent = NULL;
        switch (s[k]) {
            case '&': ent = "&amp;";  break;
            case '<': ent = "&lt;";   break;
            case '>': ent = "&gt;";   break;
            case '"': ent = "&quot;"; break;
        }
        if (ent) {
            sb_putn(sb, s + run, k - run);
            sb_puts(sb, ent);
            run = k + 1;
        }
    }
    sb_putn(sb, s + run, n - run);
}

// One piece of a line: a classed span (pal NULL), a <font color> (bold for
// the command), or the bare text (no_color).
static void piece(Sb *sb, const TerminalPalette *pal, const char *cls, const char *color,
                  int bold, const char *s, size_t n) {
    if (n == 0) return;
    if (!pal) {
        sb_puts(sb, "<span class=\"boat-rudder-terminal__");
        sb_puts(sb, cls);
        sb_puts(sb, "\">");
    } else {
        if (!pal->no_color) {
            sb_puts(sb, "<font color=\"");
            sb_puts(sb, color);
            sb_puts(sb, "\">");
        }
        if (bold) sb_puts(sb, "<b>");
    }
    sb_escape(sb, s, n);
    if (!pal) {
        sb_puts(sb, "</span>");
    } else {
        if (bold) sb_puts(sb, "</b>");
        if (!pal->no_color) sb_puts(sb, "</font>");
    }
}

// ---- A command's parts -----------------------------------------------------

static int is_space(char c) { return c == ' ' || c == '\t'; }

// A word's text, with its variables in their own color: $NAME, ${...}, $1,
// $? (Unix, PowerShell) and %NAME%, %1, %%i (DOS). `cls` NULL is the
// command's own color.
static void word_text(Sb *sb, const TerminalPalette *pal, int dos, const char *cls,
                      const char *color, const char *s, size_t n) {
    size_t run = 0, k = 0;
    while (k < n) {
        size_t end = 0;
        if (!dos && s[k] == '$' && k + 1 < n) {
            char d = s[k + 1];
            if (d == '{') {
                const char *close = memchr(s + k, '}', n - k);
                if (close) end = (size_t)(close - s) + 1;
            } else if (isalpha((unsigned char)d) || d == '_') {
                end = k + 1;
                while (end < n && (isalnum((unsigned char)s[end]) || s[end] == '_')) end++;
            } else if (isdigit((unsigned char)d) || strchr("?#@*!$", d)) {
                end = k + 2;
            }
        } else if (dos && s[k] == '%' && k + 1 < n) {
            if (s[k + 1] == '%' && k + 2 < n && isalpha((unsigned char)s[k + 2])) {
                end = k + 3;
            } else if (isdigit((unsigned char)s[k + 1]) || s[k + 1] == '*') {
                end = k + 2;
            } else {
                const char *close = memchr(s + k + 1, '%', n - k - 1);
                if (close && close > s + k + 1) end = (size_t)(close - s) + 1;
            }
        }
        if (!end) { k++; continue; }
        if (k > run) {
            if (cls) piece(sb, pal, cls, color, 0, s + run, k - run);
            else sb_escape(sb, s + run, k - run);
        }
        piece(sb, pal, "var", pal ? pal->variable : NULL, 0, s + k, end - k);
        run = k = end;
    }
    if (n > run) {
        if (cls) piece(sb, pal, cls, color, 0, s + run, n - run);
        else sb_escape(sb, s + run, n - run);
    }
}

// Length of the word at s[0..n): up to a space or a ; | & that ends it
// ("/dev/null;"), a quoted part ("a b") read whole.
static size_t word_len(const char *s, size_t n) {
    size_t k = 0;
    while (k < n && !is_space(s[k]) && (k == 0 || !strchr(";|&", s[k]))) {
        if (s[k] == '"' || s[k] == '\'') {
            const char *close = memchr(s + k + 1, s[k], n - k - 1);
            k = close ? (size_t)(close - s) + 1 : n;
        } else {
            k++;
        }
    }
    return k;
}

// The shell's operators: |, ||, &, &&, ;, >, >>, 2>, <. A "<NAME>" with no
// space in it is a placeholder a how-to asks to fill in, not a redirection.
static size_t operator_len(const char *s, size_t n) {
    if (s[0] == '<') {
        size_t k = 1;
        while (k < n && !is_space(s[k]) && s[k] != '>') k++;
        if (k > 1 && k < n && s[k] == '>') return 0;
        return 1;
    }
    if (s[0] == '2' && n > 1 && s[1] == '>') return n > 2 && s[2] == '>' ? 3 : 2;
    if (strchr("|&>", s[0])) return n > 1 && s[1] == s[0] ? 2 : 1;
    if (s[0] == ';') return 1;
    return 0;
}

// A command after its prompt: options (-x, --name, PowerShell's -Name, DOS's
// /w), the value an option takes (the next word, or what follows its '='),
// quoted strings, variables and operators each in their color; the rest -
// the program, its plain arguments - in the command's own. Bold throughout
// on epochs 0-2.
static void command(Sb *sb, const TerminalPalette *pal, const char *shell, const char *s, size_t n) {
    if (n == 0) return;
    int dos = shell && strcmp(shell, "dos") == 0;
    if (!pal) {
        sb_puts(sb, "<span class=\"boat-rudder-terminal__cmd\">");
    } else {
        if (!pal->no_color) {
            sb_puts(sb, "<font color=\"");
            sb_puts(sb, pal->text);
            sb_puts(sb, "\">");
        }
        sb_puts(sb, "<b>");
    }

    const char *opt = pal ? pal->option : NULL, *val = pal ? pal->value : NULL;
    int wants_value = 0; // the last word was an option without '='
    size_t i = 0;
    while (i < n) {
        if (is_space(s[i])) {
            size_t j = i;
            while (j < n && is_space(s[j])) j++;
            sb_escape(sb, s + i, j - i);
            i = j;
            continue;
        }
        size_t op = operator_len(s + i, n - i);
        if (op) {
            piece(sb, pal, "op", pal ? pal->prompt : NULL, 0, s + i, op);
            i += op;
            wants_value = 0;
            continue;
        }
        const char *w = s + i;
        size_t len = word_len(w, n - i);
        int is_option = (w[0] == '-' && len > 1 && w[1] != ' ') ||
                        (dos && w[0] == '/' && len > 1);
        if (is_option) {
            const char *eq = memchr(w, '=', len);
            size_t name = eq ? (size_t)(eq - w) + 1 : len;
            piece(sb, pal, "opt", opt, 0, w, name);
            if (eq) word_text(sb, pal, dos, "val", val, w + name, len - name);
            wants_value = !eq;
        } else if (wants_value || w[0] == '"' || w[0] == '\'') {
            word_text(sb, pal, dos, "val", val, w, len);
            wants_value = 0;
        } else {
            word_text(sb, pal, dos, NULL, NULL, w, len);
        }
        i += len;
    }

    if (!pal) {
        sb_puts(sb, "</span>");
    } else {
        sb_puts(sb, "</b>");
        if (!pal->no_color) sb_puts(sb, "</font>");
    }
}

static char *render(const char *text, const char *shell, const TerminalPalette *pal) {
    if (!text) text = "";
    Sb sb = { .ok = 1 };
    sb_puts(&sb, "");

    // A Windows-pasted session's '\r's and the blank lines after the last
    // one would only add empty rows at the bottom.
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;

    // A block with no prompt anywhere is a list of commands to type (what a
    // how-to shows), not their output: each line gets the shell's prompt.
    int listed = !terminal_has_prompt(text, shell);
    const char *own_prompt = terminal_shell_prompt(shell);

    size_t i = 0;
    int first = 1;
    while (i < n) {
        size_t j = i;
        while (j < n && text[j] != '\n') j++;
        size_t len = j - i;
        if (len > 0 && text[i + len - 1] == '\r') len--;

        if (!first) sb_puts(&sb, pal && pal->newline ? pal->newline : "\n");
        first = 0;

        const char *line = text + i;
        size_t prompt = terminal_prompt_len(line, len, shell);
        if (listed && len > 0) {
            piece(&sb, pal, "prompt", pal ? pal->prompt : NULL, 0, own_prompt, strlen(own_prompt));
            command(&sb, pal, shell, line, len);
        } else if (prompt) {
            piece(&sb, pal, "prompt", pal ? pal->prompt : NULL, 0, line, prompt);
            command(&sb, pal, shell, line + prompt, len - prompt);
        } else {
            piece(&sb, pal, "out", pal ? pal->output : NULL, 0, line, len);
        }
        i = j + 1;
    }

    if (!sb.ok) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

char *terminal_html(const char *text, const char *shell) {
    return render(text, shell, NULL);
}

char *terminal_html_fonts(const char *text, const char *shell, const TerminalPalette *palette) {
    return render(text, shell, palette);
}
