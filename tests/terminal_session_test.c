// The terminal block (src/utils/terminal_session.c): each shell's prompt is
// told from output - and only it - and the lines come out escaped, as
// classed spans (epoch 3) or <font> (epochs 1-2) or bare (epoch 0, WML).
//
// Run with: ctest --test-dir build/debug (or the binary directly).

#include "../src/utils/terminal_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect_prompt(const char *shell, const char *line, size_t expected) {
    checks++;
    size_t got = terminal_prompt_len(line, strlen(line), shell);
    if (got != expected) {
        printf("FAIL prompt [%s] \"%s\": expected %zu, got %zu\n", shell, line, expected, got);
        failures++;
    }
}

static void expect_html(const char *what, const char *html, const char *want) {
    checks++;
    if (!html || !strstr(html, want)) {
        printf("FAIL %s: missing %s\n  in: %s\n", what, want, html ? html : "(null)");
        failures++;
    }
}

static void expect_no(const char *what, const char *html, const char *unwanted) {
    checks++;
    if (!html || strstr(html, unwanted)) {
        printf("FAIL %s: unexpected %s\n  in: %s\n", what, unwanted, html ? html : "(null)");
        failures++;
    }
}

int main(void) {
    // Linux / Unix
    expect_prompt("", "$ ls -la", 2);
    expect_prompt("", "# apt update", 2);
    expect_prompt("", "% make", 2);
    expect_prompt("", "pablo@box:~$ make", 13);
    expect_prompt("", "root@box:/etc#   cat x", 17);
    expect_prompt("", "$", 1);
    expect_prompt("", "$HOME is set", 0);
    expect_prompt("", "#include <stdio.h>", 0);
    expect_prompt("", "50% done", 0);
    expect_prompt("", "total 8", 0);
    expect_prompt("", "", 0);
    expect_prompt("unknown", "$ ls", 2);

    // MS-DOS / Windows
    expect_prompt("dos", "C:\\>dir /w", 4);
    expect_prompt("dos", "C:\\> dir", 5);
    expect_prompt("dos", "C:\\GAMES\\DOOM>doom", 14);
    expect_prompt("dos", "C:\\Program Files>cd ..", 17);
    expect_prompt("dos", "a:\\>", 4);
    expect_prompt("dos", " Volume in drive C is DOS", 0);
    expect_prompt("dos", "C:\\DOS\\EDIT.COM", 0);
    expect_prompt("dos", "$ ls", 0);

    // PowerShell
    expect_prompt("powershell", "PS C:\\Users\\me> Get-Item .", 16);
    expect_prompt("powershell", "PS> ls", 4);
    expect_prompt("powershell", "PSReadLine loaded", 0);
    expect_prompt("powershell", "Mode  LastWriteTime", 0);

    // Titles
    checks += 3;
    if (strcmp(terminal_shell_title(""), "Terminal") != 0) { printf("FAIL title unix\n"); failures++; }
    if (strcmp(terminal_shell_title("dos"), "MS-DOS") != 0) { printf("FAIL title dos\n"); failures++; }
    if (strcmp(terminal_shell_title("powershell"), "PowerShell") != 0) { printf("FAIL title ps\n"); failures++; }

    // Epoch 3: spans, escaped, CRLF and trailing blank lines dropped
    char *html = terminal_html("C:\\>type A.TXT\r\n<hello & bye>\r\n\r\n", "dos");
    expect_html("epoch 3", html, "<span class=\"boat-rudder-terminal__prompt\">C:\\&gt;</span>"
                                 "<span class=\"boat-rudder-terminal__cmd\">type A.TXT</span>\n"
                                 "<span class=\"boat-rudder-terminal__out\">&lt;hello &amp; bye&gt;</span>");
    expect_no("epoch 3 trailing", html, "bye&gt;</span>\n");
    expect_no("epoch 3 CR", html, "\r");
    free(html);

    html = terminal_html("$ echo \"hi\"\nhi\n\n$ ", "");
    expect_html("blank output line kept", html, "</span>\n\n<span class=\"boat-rudder-terminal__prompt\">$ </span>");
    expect_html("quotes escaped", html, "echo <span class=\"boat-rudder-terminal__val\">&quot;hi&quot;</span>");
    free(html);

    html = terminal_html("", "");
    expect_html("empty", html, "");
    free(html);

    // No prompt anywhere: a list of commands, each after the shell's prompt;
    // a blank line stays blank
    html = terminal_html("cd ~/Documents\n\nmkdir vpn <x>\n", "");
    expect_html("listed", html, "<span class=\"boat-rudder-terminal__prompt\">$ </span>"
                                "<span class=\"boat-rudder-terminal__cmd\">cd ~/Documents</span>\n\n"
                                "<span class=\"boat-rudder-terminal__prompt\">$ </span>"
                                "<span class=\"boat-rudder-terminal__cmd\">mkdir vpn &lt;x&gt;</span>");
    expect_no("listed has no output", html, "__out");
    free(html);
    html = terminal_html("dir /w", "dos");
    expect_html("listed dos", html, "<span class=\"boat-rudder-terminal__prompt\">C:\\&gt;</span>"
                                    "<span class=\"boat-rudder-terminal__cmd\">dir <span class=\"boat-rudder-terminal__opt\">/w</span></span>");
    free(html);
    html = terminal_html("Get-Date", "powershell");
    expect_html("listed powershell", html, "__prompt\">PS&gt; </span>");
    free(html);
    // One prompt is enough to read the rest as output
    html = terminal_html("ls\n$ pwd\n/home", "");
    expect_html("mixed: no prompt is output", html, "<span class=\"boat-rudder-terminal__out\">ls</span>");
    free(html);

    // A command's parts
#define OPT(x) "<span class=\"boat-rudder-terminal__opt\">" x "</span>"
#define VAL(x) "<span class=\"boat-rudder-terminal__val\">" x "</span>"
#define VAR(x) "<span class=\"boat-rudder-terminal__var\">" x "</span>"
#define OP(x)  "<span class=\"boat-rudder-terminal__op\">" x "</span>"
    html = terminal_html("sudo openconnect --csd-wrapper=./hipreport.sh --protocol=gp "
                         "--user=<usuario> --servercert <HASH> vpn.<tunombre>.com", "");
    expect_html("openconnect", html, "sudo openconnect " OPT("--csd-wrapper=") VAL("./hipreport.sh") " "
                OPT("--protocol=") VAL("gp") " " OPT("--user=") VAL("&lt;usuario&gt;") " "
                OPT("--servercert") " " VAL("&lt;HASH&gt;") " vpn.&lt;tunombre&gt;.com");
    free(html);
    html = terminal_html("ls -la $HOME/x | grep \"a b\" > out.txt && echo ${USER} $1 2>/dev/null; cat - < in", "");
    expect_html("pipeline", html, "ls " OPT("-la") " " VAR("$HOME") VAL("/x"));
    free(html);
    html = terminal_html("ls -la $HOME/x | grep \"a b\" > out.txt && echo ${USER} $1 2>/dev/null; cat - < in", "");
    expect_html("pipeline: ops", html, " " OP("|") " grep " VAL("&quot;a b&quot;") " " OP("&gt;") " out.txt " OP("&amp;&amp;")
                " echo " VAR("${USER}") " " VAR("$1") " " OP("2&gt;") "/dev/null" OP(";") " cat - " OP("&lt;") " in");
    free(html);
    html = terminal_html("C:\\>copy %SRC% A:\\ /y\nC:\\>for %%i in (*) do echo %%i %1", "dos");
    expect_html("dos parts", html, "copy " VAR("%SRC%") " A:\\ " OPT("/y"));
    expect_html("dos for", html, "for " VAR("%%i") " in (*) do echo " VAR("%%i") " " VAR("%1"));
    free(html);
    html = terminal_html("PS> Get-ChildItem -Path $env:TEMP -Recurse", "powershell");
    expect_html("powershell parts", html, "Get-ChildItem " OPT("-Path") " " VAR("$env") VAL(":TEMP") " " OPT("-Recurse"));
    free(html);

    // Epochs 1-2: <font>, commands bold, epoch 1's <br>
    TerminalPalette pal = { .text = "#ffffff", .prompt = "#55ff55", .output = "#aaaaaa", .option = "#55ffff",
                            .value = "#ffff55", .variable = "#ff55ff", .newline = "<br>" };
    html = terminal_html_fonts("$ ls\nfile", "", &pal);
    expect_html("fonts", html, "<font color=\"#55ff55\">$ </font><font color=\"#ffffff\"><b>ls</b></font>"
                               "<br><font color=\"#aaaaaa\">file</font>");
    free(html);
    html = terminal_html_fonts("$ ls -l $D", "", &pal);
    expect_html("fonts: parts", html, "<font color=\"#ffffff\"><b>ls <font color=\"#55ffff\">-l</font> "
                "<font color=\"#ff55ff\">$D</font></b></font>");
    free(html);
    html = terminal_html("a|b;c&&d", "");
    expect_html("operators split words", html, "a" OP("|") "b" OP(";") "c" OP("&amp;&amp;") "d");
    free(html);

    // Epoch 0 / WML: no color, still bold, escaped
    TerminalPalette plain = { .no_color = 1, .newline = "<br/>" };
    html = terminal_html_fonts("C:\\>dir\nOK", "dos", &plain);
    expect_html("plain", html, "C:\\&gt;<b>dir</b><br/>OK");
    expect_no("plain has no font", html, "<font");
    free(html);

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
