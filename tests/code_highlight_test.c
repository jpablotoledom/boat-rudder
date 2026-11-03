// The code-text block's highlighter (src/utils/code_highlight.c): each
// language colors what it should - and only that - and every key the
// editor's <select> offers has a name of its own.
//
// Run with: ctest --test-dir build/debug (or the binary directly).

#include "../src/utils/code_highlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

// `html` contains `want` (a token as "cls:text", or raw markup when it
// holds no ':'), or lacks it with `absent`.
static void check(const char *lang, const char *html, const char *want, int absent) {
    char needle[256];
    const char *colon = strchr(want, ':');
    if (colon)
        snprintf(needle, sizeof(needle), "<span class=\"boat-rudder-code-token--%.*s\">%s</span>",
                 (int)(colon - want), want, colon + 1);
    else
        snprintf(needle, sizeof(needle), "%s", want);
    checks++;
    if ((strstr(html, needle) != NULL) == absent) {
        printf("FAIL %s: %s %s\n  in: %s\n", lang, absent ? "unexpected" : "missing", needle, html);
        failures++;
    }
}

static void has(const char *lang, const char *code, const char *const *wants) {
    char *html = code_highlight_html(code, lang);
    if (!html) { printf("FAIL %s: NULL\n", lang); failures++; return; }
    for (; *wants; wants++) {
        int absent = (*wants)[0] == '!';
        check(lang, html, *wants + absent, absent);
    }
    free(html);
}

#define HAS(lang, code, ...) has(lang, code, (const char *const[]){ __VA_ARGS__, NULL })

int main(void) {
    // The languages that came first, unchanged.
    HAS("javascript", "const x = f('a'); // hi", "kw:const", "fn:f", "str:'a'", "com:// hi");
    HAS("c", "#include <stdio.h>\nint main(void) { return 0; }", "pre:#include", "str:&lt;stdio.h&gt;", "kw:int", "num:0");
    HAS("python", "@dataclass\ndef f(): return \"\"\"x\"\"\"", "pre:@dataclass", "kw:def", "str:&quot;&quot;&quot;x&quot;&quot;&quot;");
    HAS("python", "a = b @ c", "!pre:@");
    HAS("php", "<?php $x = 1; ?>", "pre:&lt;?php", "var:$x", "pre:?&gt;");
    HAS("bash", "echo $HOME $1 # c", "kw:echo", "var:$HOME", "var:$1", "com:# c");
    HAS("sql", "select * from t -- c", "kw:select", "kw:from", "com:-- c");
    HAS("css", "a { color: #fff !important; }", "attr:color", "num:#fff", "kw:!important");
    HAS("html", "<a href=\"x\">", "tag:&lt;a", "attr:href", "str:&quot;x&quot;");

    // Web and general-purpose.
    HAS("typescript", "@Component\ninterface A { x: number }", "pre:@Component", "kw:interface", "kw:number");
    HAS("typescript", "mail@host", "!pre:@host");
    HAS("java", "@Override\npublic void run() {}", "pre:@Override", "kw:public", "kw:void", "fn:run");
    HAS("csharp", "#region X\nnamespace A { string s = \"a\"; }", "pre:#region", "kw:namespace", "kw:string", "str:&quot;a&quot;");
    HAS("kotlin", "fun main() { val s = \"\"\"raw\"\"\" }", "kw:fun", "kw:val", "str:&quot;&quot;&quot;raw&quot;&quot;&quot;");
    HAS("go", "func main() { s := `raw\nline` }", "kw:func", "str:`raw");
    HAS("swift", "#if DEBUG\n@objc func f() -> Int { guard let x else { return nil } }", "pre:#if", "pre:@objc", "kw:guard", "kw:nil");
    HAS("json", "{\"name\": \"x\", \"n\": 1, \"ok\": true}", "attr:&quot;name&quot;", "str:&quot;x&quot;", "num:1", "kw:true");

    // Markup and data.
    HAS("xml", "<?xml version=\"1.0\"?>\n<a><![CDATA[<b>]]></a>", "pre:&lt;?xml version=&quot;1.0&quot;?&gt;", "str:&lt;![CDATA[&lt;b&gt;]]&gt;", "tag:&lt;a");
    HAS("ini", "[server]\nport = 8080 ; c\n# c2\ncolor=#fff", "tag:[server]", "attr:port", "num:8080", "com:; c", "com:# c2", "!com:#fff");
    HAS("diff", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n same", "kw:--- a/x", "kw:+++ b/x", "pre:@@ -1 +1 @@", "var:-old", "str:+new", "!str: same");

    // Retro.
    HAS("asm", "start:  mov ax, $FF ; c\n        lda #$10\n.org $0800\n  db 0FFh, %1010\n  movl %eax, %ebx",
        "attr:start", "kw:mov", "var:ax", "num:$FF", "com:; c", "kw:lda", "num:$10", "pre:.org",
        "kw:db", "num:0FFh", "num:%1010", "kw:movl", "var:%eax");
    HAS("asm", "%define X 1", "pre:%define");
    HAS("basic", "10 PRINT \"HI\": A$ = LEFT$(B$, 2) ' c\n20 REM a \"comment\"\n30 GOTO 10",
        "num:10", "kw:PRINT", "str:&quot;HI&quot;", "kw:LEFT$", "com:' c", "com:REM a &quot;comment&quot;", "kw:GOTO");
    HAS("basic", "remember = 1", "!com:remember = 1");
    HAS("pascal", "{$I+}\nprogram Hi; { c } (* c2 *)\nbegin WriteLn('C:\\'); x := $FF end.",
        "pre:{$I+}", "kw:program", "com:{ c }", "com:(* c2 *)", "kw:begin", "str:'C:\\'", "num:$FF", "kw:end");
    HAS("batch", "@echo off\n:: c\nrem c2\n:loop\nset X=%1\necho %PATH% !V! %%i %~dp0\ngoto loop\ncopy \"C:\\DOS\\\" a",
        "kw:echo", "kw:off", "com::: c", "com:rem c2", "attr::loop", "kw:set", "var:%1", "var:%PATH%",
        "var:!V!", "var:%%i", "var:%~dp0", "kw:goto", "str:&quot;C:\\DOS\\&quot;");

    // Every key in the editor's <select> has its own name.
    const char *keys[] = { "javascript", "html", "c", "python", "css", "php", "bash", "sql",
                           "typescript", "java", "csharp", "kotlin", "go", "swift", "json", "xml",
                           "ini", "asm", "basic", "pascal", "batch", "diff", NULL };
    for (const char *const *k = keys; *k; k++) {
        checks++;
        if (strcmp(code_highlight_display_name(*k), "Plain text") == 0) {
            printf("FAIL no name for %s\n", *k);
            failures++;
        }
    }
    checks++;
    if (strcmp(code_highlight_display_name("cobol"), "Plain text") != 0) failures++;

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
