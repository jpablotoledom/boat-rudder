// Code editor for the dashboard's raw-code fields (a theme's CSS, the banner
// and footer markup). Enhances every <textarea data-code-editor="css|html|javascript">
// with line numbers, syntax colors, a current-line band and editing keys.
//
// The textarea stays the form field: it is only made transparent and laid
// over a colored copy of its own text, so the form posts exactly as before,
// the browser's undo/redo, selection and find keep working, and without this
// script - or without the theme's .boat-rudder-code-editor rules, see
// enhance() - the page is the plain textarea it always was.
//
// Token classes are the server's (src/utils/code_highlight.c), so the colors
// are the theme's "Code colors"; the tokenizer below mirrors that one's
// rules, line by line, plus <style>/<script> contents inside HTML.
//
//   Tab / Shift+Tab   indent / outdent (the selected lines, if any)
//   Esc, then Tab     move focus out of the editor instead
//   Enter             keeps the indentation, one more level after { ( [ or an opening tag
//   Ctrl+S            save (submits the textarea's form; a page saving it by
//                     fetch fires 'code-editor:saved' on the textarea after)
//   Ctrl+/            comment / uncomment the current or selected lines
//   Ctrl+G            go to line
//
// A readonly textarea (e.g. a theme's original stylesheet, shown to copy
// rules from) gets the same view but only Ctrl+G: Tab moves focus as usual.
(function () {
  'use strict';

  var TOKEN_PREFIX = 'boat-rudder-code-token--';
  var INDENT_UNIT = 2;              // spaces per level; overridable with data-indent
  var MAX_HIGHLIGHT_LINES = 20000;  // past this, plain text (still editable)
  var LANG_NAMES = { css: 'CSS', html: 'HTML', javascript: 'JavaScript' };

  var KW_JS = {};
  ('async await break case catch class const continue debugger default delete do else export ' +
   'extends false finally for from function get if import in instanceof let new null of return ' +
   'set static super switch this throw true try typeof undefined var void while with yield')
    .split(' ').forEach(function (k) { KW_JS[k] = 1; });

  var VOID_TAGS = { area: 1, base: 1, br: 1, col: 1, embed: 1, hr: 1, img: 1, input: 1,
                    link: 1, meta: 1, source: 1, track: 1, wbr: 1 };

  // ---- Tokenizer ------------------------------------------------------------
  //
  // One line at a time, carrying a small state across lines so the editor can
  // re-tokenize only from the edited line until the state settles again:
  //   sub      '' (the base language), 'css' or 'js' (inside <style>/<script>)
  //   com      inside a block comment (/* */ or <!-- -->)
  //   str      the quote of a string still open (a JS template, an HTML attribute)
  //   depth    CSS brace depth: property names only exist inside a rule
  //   tag      inside an HTML tag, between "<name" and ">"
  //   tagName  that tag's name, to know what a closing ">" opens

  function initialState() {
    return { sub: '', com: 0, str: '', depth: 0, tag: 0, tagName: '' };
  }

  function copyState(s) {
    return { sub: s.sub, com: s.com, str: s.str, depth: s.depth, tag: s.tag, tagName: s.tagName };
  }

  function stateKey(s) {
    return s.sub + '|' + s.com + '|' + s.str + '|' + s.depth + '|' + s.tag + '|' + s.tagName;
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function Out() { this.html = ''; this.pending = ''; }
  Out.prototype.text = function (s) { this.pending += s; };
  Out.prototype.flush = function () {
    if (this.pending) { this.html += escapeHtml(this.pending); this.pending = ''; }
  };
  Out.prototype.tok = function (cls, s) {
    if (!s) return;
    this.flush();
    this.html += '<span class="' + TOKEN_PREFIX + cls + '">' + escapeHtml(s) + '</span>';
  };

  function isAlpha(c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
  function isDigit(c) { return c >= '0' && c <= '9'; }
  function isAlnum(c) { return isAlpha(c) || isDigit(c); }
  function isHex(c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
  function isCssIdent(c) { return isAlnum(c) || c === '_' || c === '-'; }
  function isJsIdent(c) { return isAlnum(c) || c === '_' || c === '$'; }

  function startsWithCI(s, i, m) { return s.substr(i, m.length).toLowerCase() === m; }

  // Index just past the closing `q` (backslash escapes skipped), -1 if the
  // line ends first.
  function closeQuote(s, from, q) {
    for (var j = from; j < s.length; j++) {
      if (s[j] === '\\') { j++; continue; }
      if (s[j] === q) return j + 1;
    }
    return -1;
  }

  // Index of "</tag" in `s` from `i`, case-insensitive, or s.length.
  function closingTagAt(s, i, tag) {
    var k = s.toLowerCase().indexOf('</' + tag, i);
    return k < 0 ? s.length : k;
  }

  // Returns where it stopped: s.length, or (embedded) at "</style".
  function scanCss(s, i, st, o, embedded) {
    var n = s.length, j, e;
    while (i < n) {
      if (st.com) {
        e = s.indexOf('*/', i);
        j = e < 0 ? n : e + 2;
        o.tok('com', s.slice(i, j));
        if (e >= 0) st.com = 0;
        i = j;
        continue;
      }
      var c = s[i], d = s[i + 1] || '';
      if (embedded && c === '<' && startsWithCI(s, i, '</style')) return i;

      if (c === '/' && d === '*') {
        e = s.indexOf('*/', i + 2);
        j = e < 0 ? n : e + 2;
        st.com = e < 0 ? 1 : 0;
        o.tok('com', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '"' || c === '\'') {
        j = closeQuote(s, i + 1, c);
        if (j < 0) j = n;
        o.tok('str', s.slice(i, j));
        i = j;
        continue;
      }
      if (isDigit(c) || (c === '.' && isDigit(d) && !(i > 0 && isCssIdent(s[i - 1])))) {
        j = i + 1;
        while (j < n && (isAlnum(s[j]) || s[j] === '.' || s[j] === '_' || s[j] === '%')) j++;
        o.tok('num', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '@' && isAlpha(d)) {
        j = i + 1;
        while (j < n && isCssIdent(s[j])) j++;
        o.tok('kw', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '#' && st.depth > 0 && isHex(d)) {
        j = i + 1;
        while (j < n && isHex(s[j])) j++;
        o.tok('num', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '!' && s.substr(i, 10) === '!important') {
        o.tok('kw', '!important');
        i += 10;
        continue;
      }
      if (isAlpha(c) || c === '_' || (c === '-' && (isAlpha(d) || d === '-'))) {
        j = i + 1;
        while (j < n && isCssIdent(s[j])) j++;
        var k = j;
        while (k < n && (s[k] === ' ' || s[k] === '\t')) k++;
        var cls = st.depth > 0 && s[k] === ':' ? 'attr' : (s[j] === '(' ? 'fn' : '');
        if (cls) o.tok(cls, s.slice(i, j));
        else o.text(s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '{') st.depth++;
      else if (c === '}' && st.depth > 0) st.depth--;
      o.text(c);
      i++;
    }
    return i;
  }

  // Returns where it stopped: s.length, or (embedded) at "</script".
  function scanJs(s, i, st, o, embedded) {
    var n = s.length, j, e;
    var stop = embedded ? closingTagAt(s, i, 'script') : n;
    while (i < stop) {
      if (st.com) {
        e = s.indexOf('*/', i);
        j = e < 0 || e + 2 > stop ? stop : e + 2;
        o.tok('com', s.slice(i, j));
        if (e >= 0 && e + 2 <= stop) st.com = 0;
        i = j;
        continue;
      }
      if (st.str) {
        j = closeQuote(s, i, st.str);
        if (j < 0 || j > stop) j = stop;
        else st.str = '';
        o.tok('str', s.slice(i, j));
        i = j;
        continue;
      }
      var c = s[i], d = s[i + 1] || '';

      if (c === '/' && d === '*') {
        e = s.indexOf('*/', i + 2);
        var closed = e >= 0 && e + 2 <= stop;
        j = closed ? e + 2 : stop;
        st.com = closed ? 0 : 1;
        o.tok('com', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '/' && d === '/') {
        o.tok('com', s.slice(i, stop));
        i = stop;
        continue;
      }
      if (c === '"' || c === '\'' || c === '`') {
        j = closeQuote(s, i + 1, c);
        if (j < 0 || j > stop) {
          j = stop;
          if (c === '`') st.str = '`';   // only template literals span lines
        }
        o.tok('str', s.slice(i, j));
        i = j;
        continue;
      }
      if (isDigit(c) || (c === '.' && isDigit(d) && !(i > 0 && isJsIdent(s[i - 1])))) {
        j = i + 1;
        while (j < stop && (isAlnum(s[j]) || s[j] === '.' || s[j] === '_')) j++;
        o.tok('num', s.slice(i, j));
        i = j;
        continue;
      }
      if (isAlpha(c) || c === '_' || c === '$') {
        j = i + 1;
        while (j < stop && isJsIdent(s[j])) j++;
        var word = s.slice(i, j);
        var cls = KW_JS.hasOwnProperty(word) ? 'kw' : (s[j] === '(' ? 'fn' : '');
        if (cls) o.tok(cls, word);
        else o.text(word);
        i = j;
        continue;
      }
      o.text(c);
      i++;
    }
    return i;
  }

  // Inside "<name ... >": attributes and their quoted values, up to the ">".
  function scanTag(s, i, st, o) {
    var n = s.length, j;
    while (i < n) {
      var c = s[i];
      if (st.str) {
        j = s.indexOf(st.str, i);
        if (j < 0) { o.tok('str', s.slice(i)); return n; }
        o.tok('str', s.slice(i, j + 1));
        st.str = '';
        i = j + 1;
        continue;
      }
      if (c === '"' || c === '\'') {
        j = s.indexOf(c, i + 1);
        if (j < 0) { o.tok('str', s.slice(i)); st.str = c; return n; }
        o.tok('str', s.slice(i, j + 1));
        i = j + 1;
        continue;
      }
      if (c === '>' || (c === '/' && s[i + 1] === '>')) {
        var len = c === '>' ? 1 : 2;
        o.tok('tag', s.substr(i, len));
        if (len === 1 && st.tagName === 'style') { st.sub = 'css'; st.depth = 0; }
        if (len === 1 && st.tagName === 'script') st.sub = 'js';
        st.tag = 0;
        st.tagName = '';
        return i + len;
      }
      if (isAlpha(c) || c === '_' || c === ':' || c === '@') {
        j = i + 1;
        while (j < n && (isAlnum(s[j]) || '-_:.@'.indexOf(s[j]) >= 0)) j++;
        o.tok('attr', s.slice(i, j));
        i = j;
        continue;
      }
      o.text(c);
      i++;
    }
    return i;
  }

  function scanHtml(s, st, o) {
    var n = s.length, i = 0, j, e;
    while (i < n) {
      if (st.sub === 'css') {
        i = scanCss(s, i, st, o, true);
        if (i < n) { st.sub = ''; st.depth = 0; }
        continue;
      }
      if (st.sub === 'js') {
        i = scanJs(s, i, st, o, true);
        if (i < n) { st.sub = ''; st.com = 0; st.str = ''; }
        continue;
      }
      if (st.com) {
        e = s.indexOf('-->', i);
        j = e < 0 ? n : e + 3;
        o.tok('com', s.slice(i, j));
        if (e >= 0) st.com = 0;
        i = j;
        continue;
      }
      if (st.tag) {
        i = scanTag(s, i, st, o);
        continue;
      }
      var c = s[i], d = s[i + 1] || '';
      if (c === '<' && s.substr(i, 4) === '<!--') {
        e = s.indexOf('-->', i + 4);
        j = e < 0 ? n : e + 3;
        st.com = e < 0 ? 1 : 0;
        o.tok('com', s.slice(i, j));
        i = j;
        continue;
      }
      if (c === '<' && (isAlpha(d) || d === '/' || d === '!')) {
        j = i + 1;
        if (d === '/' || d === '!') j++;
        while (j < n && (isAlnum(s[j]) || s[j] === '-' || s[j] === ':')) j++;
        o.tok('tag', s.slice(i, j));
        st.tag = 1;
        st.tagName = s.slice(i + 1, j).toLowerCase();
        i = j;
        continue;
      }
      o.text(c);
      i++;
    }
  }

  // -> { html, state }: the line's highlighted markup and the state after it.
  function tokenizeLine(lang, line, state) {
    var st = copyState(state);
    var o = new Out();
    if (lang === 'css') scanCss(line, 0, st, o, false);
    else if (lang === 'javascript') scanJs(line, 0, st, o, false);
    else if (lang === 'html') scanHtml(line, st, o);
    else o.text(line);
    o.flush();
    return { html: o.html, state: st };
  }

  // ---- Text helpers ---------------------------------------------------------

  function lineStartAt(v, pos) { return v.lastIndexOf('\n', pos - 1) + 1; }
  function lineEndAt(v, pos) { var k = v.indexOf('\n', pos); return k < 0 ? v.length : k; }
  function spaces(n) { return new Array(n + 1).join(' '); }

  function countNewlines(v, end) {
    var count = 0;
    for (var k = v.indexOf('\n'); k >= 0 && k < end; k = v.indexOf('\n', k + 1)) count++;
    return count;
  }

  function el(tag, cls) {
    var e = document.createElement(tag);
    if (cls) e.className = 'boat-rudder-code-editor__' + cls;
    return e;
  }

  // ---- Editor ---------------------------------------------------------------

  var editors = [];

  function CodeEditor(ta) {
    this.ta = ta;
    this.lang = ta.getAttribute('data-code-editor');
    this.unit = parseInt(ta.getAttribute('data-indent'), 10) || INDENT_UNIT;
    this.cache = [];          // per line: { text, startKey, start, end, node }
    this.lineCount = 0;
    this.saved = ta.value;
    this.dirty = false;
    this.submitting = false;
    this.escapedTab = false;
    this.plain = false;
    this.frame = 0;
    this.rendered = null;     // the value the colored layer shows
  }

  // Builds the editor around the textarea. Returns false (and leaves the
  // page as it was) when the theme's stylesheet lacks the editor's rules -
  // e.g. an admin CSS customized before the editor existed - since a
  // transparent textarea without its colored layer would be unusable.
  CodeEditor.prototype.enhance = function () {
    var ta = this.ta;
    var root = el('div');
    root.className = 'boat-rudder-code-editor';
    var gutter = el('div', 'gutter');
    var gutterLines = el('div', 'gutter-lines');
    var gutterActive = el('div', 'gutter-active');
    var body = el('div', 'body');
    var highlight = el('div', 'highlight');
    var lines = el('div', 'lines');
    var active = el('div', 'active-line');
    var status = el('div', 'status');
    var position = el('span', 'status-position');
    var state = el('span', 'status-state');
    var hint = el('span', 'status-hint');

    gutter.setAttribute('aria-hidden', 'true');
    highlight.setAttribute('aria-hidden', 'true');
    gutterLines.appendChild(gutterActive);
    gutter.appendChild(gutterLines);
    lines.appendChild(active);
    highlight.appendChild(lines);

    ta.parentNode.insertBefore(root, ta);
    body.appendChild(highlight);
    body.appendChild(ta);
    root.appendChild(gutter);
    root.appendChild(body);
    root.appendChild(status);
    status.appendChild(position);
    status.appendChild(document.createTextNode(' \u00b7 ' + (LANG_NAMES[this.lang] || 'Plain text') +
                                               ' \u00b7 Spaces: ' + this.unit + ' '));
    status.appendChild(state);
    status.appendChild(hint);
    ta.classList.add('boat-rudder-code-editor__input');

    if (getComputedStyle(root).getPropertyValue('--br-code-editor-ready').trim() !== '1') {
      root.parentNode.insertBefore(ta, root);
      root.parentNode.removeChild(root);
      ta.classList.remove('boat-rudder-code-editor__input');
      return false;
    }

    ta.setAttribute('wrap', 'off');
    ta.setAttribute('spellcheck', 'false');
    ta.setAttribute('autocapitalize', 'off');
    ta.setAttribute('autocomplete', 'off');
    hint.textContent = ta.readOnly
      ? 'Read-only \u00b7 Ctrl+G line'
      : 'Ctrl+S save \u00b7 Ctrl+/ comment \u00b7 Ctrl+G line \u00b7 Esc, Tab: leave';

    this.root = root;
    this.gutterLines = gutterLines;
    this.gutterNumbers = document.createTextNode('');
    gutterLines.appendChild(this.gutterNumbers);
    this.gutterActive = gutterActive;
    this.lines = lines;
    this.active = active;
    this.position = position;
    this.stateLabel = state;

    var cs = getComputedStyle(ta);
    this.lineHeight = parseFloat(cs.lineHeight) || 20;
    this.padTop = parseFloat(cs.paddingTop) || 0;

    var self = this;
    ta.addEventListener('input', function () { self.schedule(); });
    ta.addEventListener('scroll', function () { self.syncScroll(); });
    ta.addEventListener('keydown', function (e) { self.onKeyDown(e); });
    ta.addEventListener('keyup', function () { self.schedule(); });
    ta.addEventListener('mouseup', function () { self.schedule(); });
    ta.addEventListener('focus', function () { self.schedule(); });
    document.addEventListener('selectionchange', function () {
      if (document.activeElement === ta) self.schedule();
    });
    // A page that saves the form itself (fetch) cancels the submit and
    // fires 'code-editor:saved' on the textarea once the save succeeded.
    if (ta.form) ta.form.addEventListener('submit', function (e) {
      if (!e.defaultPrevented) self.submitting = true;
    });
    ta.addEventListener('code-editor:saved', function () {
      self.saved = ta.value;
      self.rendered = null;
      self.schedule();
    });

    this.update();
    return true;
  };

  CodeEditor.prototype.schedule = function () {
    var self = this;
    if (this.frame) return;
    this.frame = requestAnimationFrame(function () { self.frame = 0; self.update(); });
  };

  // Compares against the rendered text rather than trusting 'input' alone:
  // not every change fires one (an undo from script, a value set by another
  // script), and every keyup/selection change lands here too.
  CodeEditor.prototype.update = function () {
    if (this.ta.value !== this.rendered) {
      this.rendered = this.ta.value;
      this.render();
      var dirty = this.ta.value !== this.saved;
      if (dirty !== this.dirty) {
        this.dirty = dirty;
        this.stateLabel.textContent = dirty ? '\u25cf Unsaved changes' : '';
        this.root.classList.toggle('boat-rudder-code-editor--dirty', dirty);
      }
    }
    this.updateCursor();
    this.syncScroll();
  };

  // Re-tokenizes only what changed: the lines between the unchanged prefix
  // and suffix, then the suffix lines for as long as the state they start in
  // differs from the one they were tokenized with (e.g. after opening a /*).
  CodeEditor.prototype.render = function () {
    var text = this.ta.value;
    var next = text.split('\n');
    var old = this.cache;

    if (next.length !== this.lineCount) this.renderGutter(next.length);

    if (next.length > MAX_HIGHLIGHT_LINES) {
      if (!this.plain) {
        this.plain = true;
        this.root.classList.add('boat-rudder-code-editor--plain');
        old.forEach(function (line) { line.node.parentNode.removeChild(line.node); });
        this.cache = [];
      }
      return;
    }
    if (this.plain) {
      this.plain = false;
      this.root.classList.remove('boat-rudder-code-editor--plain');
    }

    var p = 0;
    while (p < old.length && p < next.length && old[p].text === next[p]) p++;
    var s = 0;
    while (s < old.length - p && s < next.length - p &&
           old[old.length - 1 - s].text === next[next.length - 1 - s]) s++;

    var state = p > 0 ? old[p - 1].end : initialState();
    var anchor = s > 0 ? old[old.length - s].node : null;
    for (var r = p; r < old.length - s; r++) this.lines.removeChild(old[r].node);

    var added = [];
    var frag = document.createDocumentFragment();
    for (var k = p; k < next.length - s; k++) {
      var line = this.tokenize(next[k], state, el('div', 'line'));
      frag.appendChild(line.node);
      added.push(line);
      state = line.end;
    }
    this.lines.insertBefore(frag, anchor);

    var tail = old.slice(old.length - s);
    for (var t = 0; t < tail.length; t++) {
      if (tail[t].startKey === stateKey(state)) break;
      tail[t] = this.tokenize(tail[t].text, state, tail[t].node);
      state = tail[t].end;
    }
    this.cache = old.slice(0, p).concat(added, tail);
  };

  CodeEditor.prototype.tokenize = function (text, state, node) {
    var r = tokenizeLine(this.lang, text, state);
    node.innerHTML = r.html;
    return { text: text, start: state, startKey: stateKey(state), end: r.state, node: node };
  };

  CodeEditor.prototype.renderGutter = function (count) {
    this.lineCount = count;
    var numbers = new Array(count);
    for (var k = 0; k < count; k++) numbers[k] = k + 1;
    this.gutterNumbers.nodeValue = numbers.join('\n');
    this.root.style.setProperty('--br-code-editor-digits', Math.max(2, String(count).length));
  };

  CodeEditor.prototype.updateCursor = function () {
    var v = this.ta.value, pos = this.ta.selectionStart;
    var line = countNewlines(v, pos);
    var col = pos - lineStartAt(v, pos) + 1;
    this.position.textContent = 'Ln ' + (line + 1) + ', Col ' + col;
    var top = (this.padTop + line * this.lineHeight) + 'px';
    this.active.style.top = top;
    this.gutterActive.style.top = top;
    this.currentLine = line;
  };

  CodeEditor.prototype.syncScroll = function () {
    var x = this.ta.scrollLeft, y = this.ta.scrollTop;
    this.lines.style.transform = 'translate(' + (-x) + 'px,' + (-y) + 'px)';
    this.gutterLines.style.transform = 'translateY(' + (-y) + 'px)';
  };

  // Replaces [start, end) with `text` through the browser's own editing
  // command, so Ctrl+Z undoes it like any typed text; setRangeText() is the
  // fallback where execCommand is gone (no undo for that step then).
  CodeEditor.prototype.replace = function (start, end, text, selStart, selEnd) {
    var ta = this.ta;
    ta.focus();
    ta.setSelectionRange(start, end);
    var ok = false;
    if (start !== end || text) {
      try {
        ok = text ? document.execCommand('insertText', false, text)
                  : document.execCommand('delete', false);
      } catch (err) { ok = false; }
      if (!ok) {
        ta.setRangeText(text, start, end, 'end');
        ta.dispatchEvent(new Event('input', { bubbles: true }));
      }
    }
    ta.setSelectionRange(selStart, selEnd === undefined ? selStart : selEnd);
  };

  // The language the caret's line is written in: HTML's <style>/<script>
  // contents are CSS/JS.
  CodeEditor.prototype.langAtCaret = function () {
    if (this.lang !== 'html') return this.lang;
    var line = this.cache[countNewlines(this.ta.value, this.ta.selectionStart)];
    var sub = line ? line.start.sub : '';
    return sub === 'css' ? 'css' : sub === 'js' ? 'javascript' : 'html';
  };

  CodeEditor.prototype.onKeyDown = function (e) {
    if (e.isComposing) return;
    var mod = e.ctrlKey || e.metaKey;
    var key = e.key;

    if (this.ta.readOnly) {
      if (mod && !e.altKey && (key === 'g' || key === 'G')) { e.preventDefault(); this.goToLine(); }
      return;
    }

    if (key === 'Escape') { this.escapedTab = true; return; }
    if (key === 'Tab' && this.escapedTab) { this.escapedTab = false; return; }
    this.escapedTab = false;

    if (mod && !e.altKey && (key === 's' || key === 'S')) { e.preventDefault(); this.save(); return; }
    if (mod && !e.altKey && key === '/') { e.preventDefault(); this.toggleComment(); return; }
    if (mod && !e.altKey && (key === 'g' || key === 'G')) { e.preventDefault(); this.goToLine(); return; }
    if (mod || e.altKey) return;

    if (key === 'Tab') { e.preventDefault(); this.indent(e.shiftKey ? -1 : 1); return; }
    if (key === 'Enter' && !e.shiftKey) { e.preventDefault(); this.newline(); return; }
    if (key === '}' && this.langAtCaret() !== 'html' && this.dedentBeforeClose()) e.preventDefault();
  };

  CodeEditor.prototype.indent = function (dir) {
    var v = this.ta.value, a = this.ta.selectionStart, b = this.ta.selectionEnd;
    var multi = v.slice(a, b).indexOf('\n') >= 0;
    if (dir > 0 && !multi) {
      var n = this.unit - ((a - lineStartAt(v, a)) % this.unit);
      this.replace(a, b, spaces(n), a + n);
      return;
    }

    var ls = lineStartAt(v, a);
    var le = lineEndAt(v, b > a && v[b - 1] === '\n' ? b - 1 : b);
    var block = v.slice(ls, le);
    var lines = block.split('\n');
    var unit = this.unit, first = 0, total = 0;
    var out = lines.map(function (line, idx) {
      var d;
      if (dir > 0) {
        d = line.length ? unit : 0;
        line = spaces(d) + line;
      } else {
        var m = /^(\t| {1,})/.exec(line);
        d = m ? -(m[0] === '\t' ? 1 : Math.min(m[0].length, unit)) : 0;
        line = line.slice(-d);
      }
      if (idx === 0) first = d;
      total += d;
      return line;
    }).join('\n');
    if (out === block) return;

    var na = Math.max(ls, a + first);
    this.replace(ls, le, out, na, a === b ? na : b + total);
  };

  CodeEditor.prototype.newline = function () {
    var v = this.ta.value, a = this.ta.selectionStart, b = this.ta.selectionEnd;
    var before = v.slice(lineStartAt(v, a), a);
    var indent = /^[ \t]*/.exec(before)[0];
    var trimmed = before.replace(/\s+$/, '');
    var last = trimmed.slice(-1);
    var rest = v.slice(b, lineEndAt(v, b));
    var gap = /^[ \t]*/.exec(rest)[0].length;
    var after = rest.slice(gap);

    var opens = '{(['.indexOf(last) >= 0;
    var closes = opens && after[0] === '})]'['{(['.indexOf(last)];
    if (!opens && this.langAtCaret() === 'html') {
      var m = /<([a-zA-Z][\w:-]*)(?:\s[^<>]*)?>$/.exec(trimmed);
      opens = !!m && !VOID_TAGS[m[1].toLowerCase()] && trimmed.slice(-2) !== '/>';
      closes = opens && after.slice(0, 2) === '</';
    }

    if (!opens) {
      this.replace(a, b, '\n' + indent, a + 1 + indent.length);
      return;
    }
    var inner = indent + spaces(this.unit);
    var text = '\n' + inner + (closes ? '\n' + indent : '');
    // Whitespace between the caret and the closer would end up after the new indent.
    this.replace(a, closes ? b + gap : b, text, a + 1 + inner.length);
  };

  // Typing "}" on a line with nothing but indentation before the caret
  // outdents it one level first.
  CodeEditor.prototype.dedentBeforeClose = function () {
    var v = this.ta.value, a = this.ta.selectionStart;
    if (a !== this.ta.selectionEnd) return false;
    var ls = lineStartAt(v, a);
    var before = v.slice(ls, a);
    if (!/^ +$/.test(before) || before.length < this.unit) return false;
    var keep = before.length - this.unit;
    this.replace(ls, a, spaces(keep) + '}', ls + keep + 1);
    return true;
  };

  CodeEditor.prototype.toggleComment = function () {
    var v = this.ta.value, a = this.ta.selectionStart, b = this.ta.selectionEnd;
    var ls = lineStartAt(v, a);
    var le = lineEndAt(v, b > a && v[b - 1] === '\n' ? b - 1 : b);
    var block = v.slice(ls, le);
    var lang = this.langAtCaret();
    var out, shift;

    if (lang === 'javascript') {
      var lines = block.split('\n');
      var code = lines.filter(function (l) { return /\S/.test(l); });
      var commented = code.length > 0 && code.every(function (l) { return /^\s*\/\//.test(l); });
      out = lines.map(function (l) {
        if (!/\S/.test(l)) return l;
        return commented ? l.replace(/^(\s*)\/\/ ?/, '$1') : l.replace(/^(\s*)/, '$1// ');
      }).join('\n');
      shift = (out.split('\n')[0].length - lines[0].length);
    } else {
      var open = lang === 'css' ? '/*' : '<!--';
      var close = lang === 'css' ? '*/' : '-->';
      var m = /^(\s*)([\s\S]*?)(\s*)$/.exec(block);
      var core = m[2];
      if (core.slice(0, open.length) === open && core.slice(-close.length) === close) {
        var inner = core.slice(open.length, core.length - close.length);
        var lead = /^ ?/.exec(inner)[0].length;
        inner = inner.slice(lead).replace(/ $/, '');
        out = m[1] + inner + m[3];
        shift = -(open.length + lead);
      } else if (core) {
        out = m[1] + open + ' ' + core + ' ' + close + m[3];
        shift = open.length + 1;
      } else {
        return;
      }
    }
    if (out === block) return;

    if (a === b) {
      var caret = Math.max(ls, a + shift);
      this.replace(ls, le, out, caret);
    } else {
      this.replace(ls, le, out, ls, ls + out.length);
    }
  };

  CodeEditor.prototype.goToLine = function () {
    var answer = window.prompt('Go to line (1-' + this.lineCount + '):', String((this.currentLine || 0) + 1));
    var n = parseInt(answer, 10);
    if (!n) return;
    n = Math.max(1, Math.min(n, this.lineCount));
    var v = this.ta.value, pos = 0;
    for (var k = 1; k < n; k++) pos = v.indexOf('\n', pos) + 1;
    this.ta.focus();
    this.ta.setSelectionRange(pos, pos);
    this.ta.scrollTop = Math.max(0, (n - 1) * this.lineHeight - this.ta.clientHeight / 2);
    this.schedule();
  };

  // requestSubmit() fires the submit event, which csrf.js needs to add the
  // form's token; form.submit() would skip it.
  CodeEditor.prototype.save = function () {
    var form = this.ta.form;
    if (!form) return;
    if (form.requestSubmit) {
      form.requestSubmit();
    } else {
      var button = form.querySelector('[type="submit"]');
      if (button) button.click();
    }
  };

  window.addEventListener('beforeunload', function (e) {
    var unsaved = editors.some(function (ed) { return ed.dirty && !ed.submitting; });
    if (!unsaved) return;
    e.preventDefault();
    e.returnValue = '';
  });

  function init() {
    var list = document.querySelectorAll('textarea[data-code-editor]');
    for (var k = 0; k < list.length; k++) {
      var ta = list[k];
      if (ta.getAttribute('data-code-editor-ready')) continue;
      ta.setAttribute('data-code-editor-ready', '1');
      var editor = new CodeEditor(ta);
      if (editor.enhance()) editors.push(editor);
    }
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
