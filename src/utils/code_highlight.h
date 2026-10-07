#ifndef CODE_HIGHLIGHT_H
#define CODE_HIGHLIGHT_H

// Server-side syntax highlighting for the code-text block. `lang` is the
// block's extra_data - exactly the value the entry editor's language
// <select> stores ("javascript", "html", "c", "python", "css", "php",
// "bash", "sql", or "" for plain text), so there is no alias table: adding a
// language to the editor means adding a LangDef in code_highlight.c too.
//
// Returns malloc'd HTML (caller frees), NULL only on allocation failure:
// every source line wrapped in <span class="boat-rudder-code-line">,
// joined by '\n', and every recognized token in
// <span class="boat-rudder-code-token--{kw,str,com,num,fn,var,pre,tag,attr}">.
// All code text is HTML-escaped. An unknown or empty `lang` still gets the
// line wrapping and escaping, just no token spans.
char *code_highlight_html(const char *code, const char *lang);

// The theme's "Code colors" (cms_themes.h code_*), as plain "#rrggbb".
typedef struct {
    const char *keyword;     // keywords (bold) and function calls
    const char *string;
    const char *comment;
    const char *number;
    const char *variable;
    const char *tag;         // directives, markup tags, attributes
    const char *line_number;
    int numbers;             // write each line's number in front of it
    int no_color;            // no markup at all: plain text (epoch 0's readers)
    int br_newlines;         // end lines with <br> instead of '\n' (epoch 1: some
                             // Mosaic builds collapse a <pre>'s own newlines)
} CodeHighlightPalette;

// Same tokenizer, for browsers without dependable CSS (epoch 1-2: Netscape
// and IE of that era apply class rules erratically, if at all): every token
// is a <font color="..."> straight from `palette` - or no markup at all with
// palette->no_color (epoch 0) - lines are joined by plain '\n' for a <pre>,
// and with palette->numbers each starts with its own right-aligned number
// (in palette->line_number's color unless no_color) - there is no CSS
// counter to draw the gutter.
char *code_highlight_html_fonts(const char *code, const char *lang,
                                const CodeHighlightPalette *palette);

// Human-readable name for `lang` as the editor's <select> labels it
// ("C / C++", "JavaScript", ...), "Plain text" for "" / unknown. Never NULL.
const char *code_highlight_display_name(const char *lang);

#endif
