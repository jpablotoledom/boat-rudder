#ifndef TERMINAL_SESSION_H
#define TERMINAL_SESSION_H

#include <stddef.h>

// The terminal block: a pasted shell session. `shell` is the block's
// extra_data - the entry editor's <select> value: "" (Linux / Unix),
// "dos" (MS-DOS / Windows) or "powershell"; an unknown one reads as "".
// A line that starts with that shell's prompt is a command, every other
// line is what the commands printed - unless no line has a prompt, see
// terminal_has_prompt():
//
//   ""            "$ ", "# ", "% " and "user@host:~$ " (a word ending in $ # %)
//   "dos"         "C:\>", "C:\GAMES>", "A:\>"
//   "powershell"  "PS>", "PS C:\Users\me> "
//
// Returns the prompt's length in line[0..len) - spaces after it included,
// so the command starts right after - or 0 when the line isn't a command.
size_t terminal_prompt_len(const char *line, size_t len, const char *shell);

// Whether any line of `text` starts with the shell's prompt. A block where
// none does is a list of commands (a how-to's "type this"): every non-empty
// line is drawn as a command after terminal_shell_prompt().
int terminal_has_prompt(const char *text, const char *shell);

// The prompt drawn before a listed command: "$ ", "C:\>", "PS> ".
const char *terminal_shell_prompt(const char *shell);

// The title shown on the block's window: "Terminal", "MS-DOS",
// "PowerShell". Never NULL.
const char *terminal_shell_title(const char *shell);

// Epoch 3: malloc'd HTML (caller frees), NULL only on allocation failure.
// Lines joined by '\n', each command as
//   <span class="boat-rudder-terminal__prompt">$ </span><span class="boat-rudder-terminal__cmd">ls ...</span>
// with its parts in __opt (-la, --name), __val (an option's value, a quoted
// string), __var ($HOME, %PATH%) and __op (|, &&, >) spans inside __cmd,
// and each output line as
//   <span class="boat-rudder-terminal__out">...</span>
// colored by the theme's --br-color-terminal-* properties. Everything is
// HTML-escaped.
char *terminal_html(const char *text, const char *shell);

// The theme's "Terminal colors" (cms_themes.h terminal_*), as "#rrggbb".
typedef struct {
    const char *text;    // commands
    const char *prompt;
    const char *output;
    const char *option;   // -x, --name, /w
    const char *value;    // an option's value, quoted strings
    const char *variable; // $NAME, %NAME%
    int no_color;        // no <font> at all (epoch 0's readers, WML)
    const char *newline; // between lines: NULL for '\n' (a <pre>), "<br>" for epoch 1's
                         // Mosaic, "<br/>" for WML
} TerminalPalette;

// Epochs -1 to 2: the same lines as <font color="..."> (or bare, no_color),
// for browsers without dependable CSS; commands bold, so they still stand
// out on a reader without color.
char *terminal_html_fonts(const char *text, const char *shell, const TerminalPalette *palette);

#endif
