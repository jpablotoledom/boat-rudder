# Boat Rudder - Code Highlighting

The `code-text` content block is syntax-highlighted **on the server**, once per request, with no
JavaScript and no external library: `src/utils/code_highlight.c` tokenizes the code and emits
markup suited to each epoch, colored from the active theme's *Code colors*.

---

## Languages

The block's `extra_data` is the language key, exactly the value of the entry editor's
`<select>` (`html/templates/dashboard/entries/editor/blocks/code-text_epoch3.html`). There is no
alias table.

| Key | Display name | Notable rules |
|---|---|---|
| `""` | Plain text | Escaping and line wrapping only, no tokens |
| `javascript` | JavaScript | `//`, `/* */`, `"` `'` `` ` `` strings, `$` is an identifier char |
| `html` | HTML | Dedicated markup scanner: tags, attributes, attribute values, comments |
| `c` | C / C++ | `#` at line start is a preprocessor directive |
| `python` | Python | `#` comments, `"""`/`'''` block strings |
| `css` | CSS | Property names, `@rules`, `#hex` colors, `!important` |
| `php` | PHP | `<?php`, `<?=`, `?>` markers, `$vars`, case-insensitive keywords, `//` and `#` comments |
| `bash` | Bash | `$var`, `${...}`, `$1 $# $?`, `#` only at a word boundary, multi-line strings |
| `sql` | SQL | `--` comments, case-insensitive keywords |

An unknown key behaves like plain text and is labeled "Plain text".

### Adding a language

1. Add a `KW_<LANG>[]` keyword list (if any) and a `LangDef` entry to `LANGS[]` in
   `code_highlight.c` - its `key` is the stored value, `display` the label; set the flags that
   apply (comments, quotes, `nocase`, `c_preproc`, `dollar_vars`, …).
2. Add the matching `<option value="<key>"><display></option>` to the editor's
   `code-text_epoch3.html`.
3. Nothing else: the CSS classes and theme colors are shared by every language.

---

## Token classes and colors

| Token | Epoch 3 class | Theme color (`themes.colors`) |
|---|---|---|
| keyword | `boat-rudder__code-token--kw` (bold) | `code-keyword` |
| function call | `--fn` | `code-keyword` |
| string | `--str` | `code-string` |
| comment | `--com` (italic) | `code-comment` |
| number, hex color | `--num` | `code-number` |
| variable (`$x`) | `--var` | `code-variable` |
| directive / PHP marker | `--pre` | `code-tag` |
| markup tag | `--tag` | `code-tag` |
| attribute | `--attr` | `code-tag` |
| untokenized text | - | `code-text` |
| box background | - | `code-background` |
| line-number gutter | `boat-rudder__code-line::before` | `code-line-number` |

Nine colors rather than one per token type keeps the theme panel short; defaults are the
CGA/Turbo C palette on navy. See [themes.md](themes.md#41-the-palette).

---

## Behavior per epoch

| Epoch | Function | Output |
|---|---|---|
| 3 | `code_highlight_html(code, lang)` | Each line in `<span class="boat-rudder__code-line">`, tokens in classed `<span>`s; colors from `--br-color-code-*` CSS variables; line numbers drawn by a CSS counter |
| 2 | `code_highlight_html_fonts(code, lang, &palette)` | Tokens as `<font color="#…">` from the theme palette (Netscape 4/IE 5 can't be trusted with class rules); each line prefixed with its right-aligned number; joined by `\n` inside `<pre>` |
| 1 | same, `br_newlines = 1` | As epoch 2 but lines end in `<br>` (some Mosaic builds collapse `<pre>` newlines); NCSA Mosaic ignores `<font color>` and simply shows plain text |
| 0 | same, `no_color = 1` | Plain escaped text with line numbers in `<pre>` - no markup at all |
| −1 (WML) | `expand_newlines()` | No highlighting; newlines become `<br/>`. WML has no `<pre>` |

All code is HTML-escaped in every path. Rendering lives in `render_code_text()`
(`src/modules/entry_page/entry_page.c`) with the per-epoch templates
`html/templates/elements/code-text/code-text_epoch<N>.html`.

### Line numbers toggle (`?code_lines=off`)

Epochs 0-2 write line numbers into the text itself, and their JavaScript (if any) can't rewrite a
rendered page. So each block gets a **Hide/Show line numbers** button that reloads the same page
with or without `?code_lines=off`, jumping back to the block's anchor `#code-<n>` (blocks are
numbered per page). The parameter is read once per request into `request_code_lines`
(`src/utils/request_code_lines.c`). Epoch 3 doesn't need it.

### Editor preview

The entry editor shows an unselected `code-text` block through
`POST /dashboard/api/block-preview`, which calls `entry_page_render_block()` - i.e. exactly the
epoch 3 output a reader gets ([entry-editor.md](entry-editor.md#block-preview)).

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
