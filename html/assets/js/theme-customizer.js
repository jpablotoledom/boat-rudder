// Theme customizer (/dashboard/settings/themes/<key>/customize, rendered by
// site_settings_customize_page()). The sidebar holds every epoch's panel for
// each part of the page; this script shows the chosen epoch's, keeps a
// preview of the site in that theme and epoch beside them, and saves every
// form in place:
//
//   - Epoch: panels carry data-epoch="<N>", color rows data-epochs="<N> ...";
//     only the chosen epoch's are shown (remembered per browser).
//   - Preview: an iframe of the site with ?theme=<key>&preview_epoch=<N>
//     (http_router.c's resolve_epoch()/request_theme_set()), kept on every
//     link followed inside it; the open section (an accordion: opening one
//     closes the others) scrolls it to its part of the page - top, middle or
//     bottom, the same in every epoch (scrollPreview()). Epoch -1 is WML, which no iframe renders: it is fetched and
//     translated into a small phone screen instead (wap*() below).
//   - Saving: every sidebar/drawer form is posted with fetch to the same
//     endpoint its own page uses, then the preview reloads.
//   - Live (epoch 3): color changes set their --br-color-* variable, and the
//     CSS drawer's text is injected as a <style>, inside the preview before
//     anything is saved.
//   - The colors form's groups marked data-section="<section>" are moved into
//     that section - Navbar, Content (body, links, home content), Home blog,
//     Footer - each section getting its own "Save colors"; the fields stay
//     in the form through the form="" attribute. What is left (the table and
//     code blocks) is the "Data colors" section.
(function () {
  'use strict';

  var root = document.querySelector('.boat-rudder-customizer');
  if (!root) return;

  var PREFIX = 'boat-rudder-customizer__';
  var STORE_KEY = 'boat-rudder-customizer';
  var theme = root.dataset.theme;
  var frame = q('.' + PREFIX + 'frame');
  var stage = q('.' + PREFIX + 'stage');
  var wap = q('.' + PREFIX + 'wap');
  var statusEl = q('.' + PREFIX + 'status');
  var sizeSelect = q('.' + PREFIX + 'size select');
  var drawer = q('.' + PREFIX + 'drawer');
  var drawerToggle = q('.' + PREFIX + 'drawer-toggle');
  var drawerBody = q('.' + PREFIX + 'drawer-body');
  var cssForm = q('.' + PREFIX + 'css-form');
  var cssInput = cssForm.querySelector('textarea[name="css"]');
  var liveToggle = q('.' + PREFIX + 'live input');
  var colorsForm = document.getElementById('boat-rudder-theme-colors');

  var epoch = 3;
  var framePath = '/';
  var activeSection = null;
  var liveVars = {};
  var dirtyForms = [];

  function q(sel) { return root.querySelector(sel); }
  function qa(sel, el) { return Array.prototype.slice.call((el || root).querySelectorAll(sel)); }

  function load() {
    try { return JSON.parse(localStorage.getItem(STORE_KEY)) || {}; } catch (e) { return {}; }
  }
  function store(patch) {
    try {
      var data = load();
      for (var k in patch) data[k] = patch[k];
      localStorage.setItem(STORE_KEY, JSON.stringify(data));
    } catch (e) { /* private mode: nothing remembered */ }
  }

  function setStatus(text, isError) {
    statusEl.textContent = text;
    statusEl.classList.toggle(PREFIX + 'status--error', !!isError);
  }

  // ---- Colors form: groups moved to their sections ------------------------

  if (colorsForm) {
    qa('fieldset[data-section]', colorsForm).forEach(function (fieldset) {
      var body = q('[data-section="' + fieldset.dataset.section + '"] .' + PREFIX + 'section-body');
      if (!body) return;
      qa('input, select, textarea, button', fieldset).forEach(function (el) {
        el.setAttribute('form', colorsForm.id);
      });
      body.appendChild(fieldset);
    });
    // One "Save colors" per section that received groups, after the last.
    qa('.' + PREFIX + 'section-body').forEach(function (body) {
      if (!body.querySelector('fieldset[data-section]')) return;
      var save = document.createElement('button');
      save.type = 'submit';
      save.className = 'boat-rudder-btn boat-rudder-btn--primary ' + PREFIX + 'save-colors';
      save.textContent = 'Save colors';
      save.setAttribute('form', colorsForm.id);
      body.appendChild(save);
    });
  }

  // ---- Epoch --------------------------------------------------------------

  function setEpoch(next) {
    epoch = next;
    store({ epoch: epoch });
    root.dataset.epoch = String(epoch);

    qa('.' + PREFIX + 'epoch').forEach(function (b) {
      b.setAttribute('aria-pressed', String(Number(b.dataset.epoch) === epoch));
    });
    qa('.boat-rudder-dashboard__settings-panel[data-epoch]').forEach(function (panel) {
      panel.hidden = Number(panel.dataset.epoch) !== epoch;
    });
    qa('[data-epochs]').forEach(function (el) {
      el.hidden = el.dataset.epochs.split(' ').indexOf(String(epoch)) < 0;
    });
    qa('fieldset').forEach(function (fs) {
      var rows = qa('[data-epochs]', fs);
      if (rows.length) fs.hidden = rows.every(function (r) { return r.hidden; });
    });
    if (colorsForm) {
      var panel = colorsForm.closest('.boat-rudder-dashboard__settings-panel');
      panel.hidden = !colorsForm.querySelector('fieldset:not([hidden])');
    }

    qa('.' + PREFIX + 'save-colors').forEach(function (button) {
      button.hidden = !button.parentNode.querySelector('fieldset[data-section]:not([hidden])');
    });

    qa('.' + PREFIX + 'section-body').forEach(function (body) {
      var empty = body.querySelector('.' + PREFIX + 'empty');
      var visible = Array.prototype.some.call(body.children, function (child) {
        return child !== empty && !child.hidden && !child.matches('.' + PREFIX + 'hint');
      });
      if (!empty) {
        empty = document.createElement('p');
        empty.className = PREFIX + 'empty';
        empty.textContent = 'Nothing to customize here for this epoch.';
        body.appendChild(empty);
      }
      empty.hidden = visible;
    });

    loadPreview();
  }

  qa('.' + PREFIX + 'epoch').forEach(function (b) {
    b.addEventListener('click', function () { setEpoch(Number(b.dataset.epoch)); });
  });

  // ---- Preview ------------------------------------------------------------

  function withParams(path) {
    var url = new URL(path, location.origin);
    url.searchParams.set('theme', theme);
    url.searchParams.set('preview_epoch', String(epoch));
    return url.pathname + url.search + url.hash;
  }

  function loadPreview() {
    if (epoch === -1) {
      frame.hidden = true;
      wap.hidden = false;
      wapLoad(framePath);
      return;
    }
    wap.hidden = true;
    frame.hidden = false;
    frame.src = withParams(framePath);
  }

  function applySize() {
    var value = sizeSelect.value;
    store({ size: value });
    if (value === 'full') {
      frame.style.width = '100%';
      frame.style.height = '100%';
      return;
    }
    var parts = value.split('x');
    frame.style.width = parts[0] + 'px';
    frame.style.height = parts[1] + 'px';
  }

  function frameDoc() {
    try { return frame.contentDocument; } catch (e) { return null; }
  }

  // Links inside the preview know nothing about it: each same-site one gets
  // the theme and epoch, so following it stays in the preview.
  function keepParams(doc) {
    qa('a[href]', doc).forEach(function (a) {
      var href = a.getAttribute('href');
      if (!href || href.charAt(0) === '#' || /^([a-z][a-z0-9+.-]*:|\/\/)/i.test(href)) return;
      a.setAttribute('href', withParams(href));
    });
  }

  // The site's theme switcher (epoch 3's drop-down, epoch 1/2's link to
  // /theme) would swap the theme being previewed - and /theme/set would
  // change the admin's own theme cookie - so it is taken out of the preview.
  function isThemeLink(a) {
    try {
      var path = new URL(a.getAttribute('href'), location.origin).pathname;
      return path === '/theme' || path === '/theme/set';
    } catch (e) { return false; }
  }

  function hideThemeSwitcher(doc) {
    qa('.boat-rudder-navbar-theme', doc).forEach(function (el) { el.style.display = 'none'; });
    qa('a[href]', doc).forEach(function (a) {
      if (!isThemeLink(a) || a.closest('.boat-rudder-navbar-theme')) return;
      var box = a.closest('td');
      if (box && box.textContent.trim() === a.textContent.trim()) {
        box.style.display = 'none';
        return;
      }
      // Epoch 1: "[<a>Dark</a>]" - the brackets go with the link.
      var prev = a.previousSibling, next = a.nextSibling;
      if (prev && prev.nodeType === 3) prev.nodeValue = prev.nodeValue.replace(/\[\s*$/, '');
      if (next && next.nodeType === 3) next.nodeValue = next.nodeValue.replace(/^\s*\]/, '');
      a.remove();
    });
  }

  // Epoch 0 pages carry no colors at all - a text browser shows them in its
  // own. Unstyled inside the iframe, their black default text would sit on
  // the dashboard's own background, so they get the admin theme's colors.
  function epoch0Colors(doc) {
    if (epoch !== 0 || !doc.head || doc.getElementById('br-customizer-epoch0')) return;
    var cs = getComputedStyle(root);
    var get = function (name) { return cs.getPropertyValue(name).trim(); };
    var style = doc.createElement('style');
    style.id = 'br-customizer-epoch0';
    style.textContent = 'html{background:' + get('--br-admin-surface') + ';color:' + get('--br-admin-text') +
      ';}a:link,a:visited{color:' + get('--br-admin-accent') + ';}';
    doc.head.appendChild(style);
  }

  // The open section's part of the page, by position - the same in every
  // epoch, whatever its markup: data-scroll="top" (logo, navbar, banner,
  // content), "middle" (home blog) or "bottom" (footer); none for the data
  // colors. Only the preview scrolls, never the dashboard around it.
  function scrollPreview(smooth) {
    var where = activeSection && activeSection.dataset.scroll;
    if (!where) return;
    var behavior = smooth && !reduceMotion ? 'smooth' : 'auto';

    function place(el, setTop) {
      var max = Math.max(0, el.scrollHeight - el.clientHeight);
      setTop(where === 'bottom' ? max : where === 'middle' ? max / 2 : 0);
    }

    if (epoch === -1) {
      place(wapScreen, function (top) { wapScreen.scrollTo({ top: top, behavior: behavior }); });
      return;
    }
    var doc = frameDoc();
    if (!doc || !doc.documentElement) return;
    place(doc.scrollingElement || doc.documentElement, function (top) {
      frame.contentWindow.scrollTo({ top: top, behavior: behavior });
    });
  }

  function applyLive() {
    var doc = frameDoc();
    if (!doc || !doc.head || epoch !== 3) return;

    for (var name in liveVars) doc.documentElement.style.setProperty(name, liveVars[name]);

    var style = doc.getElementById('br-customizer-live-css');
    var on = liveToggle.checked && !drawerBody.hidden;
    if (!on) {
      if (style) style.remove();
      return;
    }
    if (!style) {
      style = doc.createElement('style');
      style.id = 'br-customizer-live-css';
      doc.head.appendChild(style);
    }
    style.textContent = cssInput.value;
  }

  frame.addEventListener('load', function () {
    var doc = frameDoc();
    if (!doc) return;
    try {
      var url = new URL(frame.contentWindow.location.href);
      url.searchParams.delete('theme');
      url.searchParams.delete('preview_epoch');
      framePath = url.pathname + url.search;
    } catch (e) { /* keep the last known path */ }
    hideThemeSwitcher(doc);
    keepParams(doc);
    epoch0Colors(doc);
    applyLive();
    scrollPreview(false);
  });

  sizeSelect.addEventListener('change', applySize);
  q('.' + PREFIX + 'reload').addEventListener('click', loadPreview);

  // ---- Sections -----------------------------------------------------------

  // An accordion: opening one section closes the others, both animated -
  // the body's height (and padding) slides between 0 and its own, and a
  // closing <details> only loses [open] once the slide is over. Instant
  // with prefers-reduced-motion.
  var ANIMATION_MS = 220;
  var reduceMotion = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;

  function slide(section, opening) {
    var body = section.querySelector('.' + PREFIX + 'section-body');
    if (section.sliding) section.sliding.cancel();
    if (opening) section.open = true;
    section.closing = !opening;
    if (reduceMotion || !body.animate) {
      if (!opening) section.open = false;
      section.closing = false;
      return;
    }

    var cs = getComputedStyle(body);
    var shown = { height: body.offsetHeight + 'px', paddingTop: cs.paddingTop,
                  paddingBottom: cs.paddingBottom, opacity: 1 };
    var hidden = { height: '0px', paddingTop: '0px', paddingBottom: '0px', opacity: 0 };
    body.style.overflow = 'hidden';
    var anim = body.animate(opening ? [hidden, shown] : [shown, hidden],
                            { duration: ANIMATION_MS, easing: 'ease-in-out' });
    section.sliding = anim;
    anim.onfinish = function () {
      section.sliding = null;
      body.style.overflow = '';
      if (!opening) section.open = false;
      section.closing = false;
      if (opening) section.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    };
    anim.oncancel = function () { body.style.overflow = ''; };
  }

  qa('.' + PREFIX + 'section').forEach(function (section) {
    section.querySelector('summary').addEventListener('click', function (e) {
      e.preventDefault();
      var opening = !section.open || section.closing;
      if (opening) {
        qa('.' + PREFIX + 'section[open]').forEach(function (other) {
          if (other !== section && !other.closing) slide(other, false);
        });
      }
      slide(section, opening);
      store({ open: opening ? section.dataset.section : null });
    });
    section.addEventListener('toggle', function () {
      if (!section.open) return;
      activeSection = section;
      scrollPreview(true);
    });
  });

  // ---- Saving -------------------------------------------------------------

  // Per section, the forms with unsaved changes made in it - the colors
  // form spans three sections, but only the one that was edited is marked.
  function markDirty(form, dirty, field) {
    var i = dirtyForms.indexOf(form);
    if (dirty && i < 0) dirtyForms.push(form);
    if (!dirty && i >= 0) dirtyForms.splice(i, 1);

    if (dirty) {
      var section = field && field.closest && field.closest('.' + PREFIX + 'section');
      if (section) section.classList.add(PREFIX + 'section--dirty');
      setStatus('\u25cf Unsaved changes');
      return;
    }
    qa('.' + PREFIX + 'section--dirty').forEach(function (section) {
      var still = dirtyForms.some(function (f) {
        return section.contains(f) || qa('[form="' + f.id + '"]', section).length > 0;
      });
      if (!still) section.classList.remove(PREFIX + 'section--dirty');
    });
  }

  root.addEventListener('input', function (e) {
    var form = e.target.form || (e.target.closest && e.target.closest('form'));
    if (form && form !== cssForm && root.contains(form)) markDirty(form, true, e.target);
    if (e.target === cssInput) applyLive();
    if (form === colorsForm) liveColor(e.target);
  });
  root.addEventListener('change', function (e) {
    var form = e.target.form;
    if (form && form !== cssForm && e.target.type !== 'file') markDirty(form, true, e.target);
  });

  // Capture phase: runs before the code editor's own submit listener, which
  // then sees the submit cancelled (csrf.js, on document, has already added
  // the token).
  root.addEventListener('submit', function (e) {
    var form = e.target;
    if (!form.closest('.' + PREFIX + 'sidebar, .' + PREFIX + 'drawer')) return;
    e.preventDefault();

    setStatus('Saving\u2026');
    fetch(form.action, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(new FormData(form)),
      credentials: 'same-origin'
    }).then(function (r) {
      if (!r.ok) throw new Error(r.status);
      markDirty(form, false);
      if (form === cssForm) cssInput.dispatchEvent(new Event('code-editor:saved'));
      if (form === colorsForm) liveVars = {};
      setStatus(dirtyForms.length ? '\u25cf Unsaved changes' : 'Saved');
      loadPreview();
    }).catch(function () {
      setStatus('Could not save - try again', true);
    });
  }, true);

  window.addEventListener('beforeunload', function (e) {
    if (!dirtyForms.length) return;
    e.preventDefault();
    e.returnValue = '';
  });

  // ---- Live colors (epoch 3) ----------------------------------------------

  // A color input "x" (and its "x-alpha" opacity range, for backgrounds)
  // sets --br-color-x, the variable page_layout.c's {{THEME_COLORS}} block
  // defines - except the two fields with no variable of their own.
  function liveColor(input) {
    var name = input.name || '';
    if (!name || name === 'logo-font' || name === 'body-background-epoch1') return;
    var base = name.replace(/-alpha$/, '');
    var color = colorsForm.elements[base];
    if (!color || color.type !== 'color') return;

    var value = color.value;
    var alpha = colorsForm.elements[base + '-alpha'];
    if (alpha && Number(alpha.value) < 100) {
      var hex = Math.round(Number(alpha.value) * 2.55).toString(16);
      value += (hex.length < 2 ? '0' : '') + hex;
    }
    liveVars['--br-color-' + base] = value;
    applyLive();
  }

  // ---- CSS drawer ---------------------------------------------------------

  function setDrawer(open) {
    drawerBody.hidden = !open;
    drawerToggle.setAttribute('aria-expanded', String(open));
    root.classList.toggle(PREFIX + '--drawer-open', open);
    store({ drawer: open });
    applyLive();
  }
  drawerToggle.addEventListener('click', function () { setDrawer(drawerBody.hidden); });
  liveToggle.addEventListener('change', applyLive);

  // ---- WAP (epoch -1) preview ---------------------------------------------
  //
  // The site's WML deck is fetched like the iframe would, then each card's
  // text, images and links are rebuilt as HTML in a phone-sized screen.
  // Images point at their PNG twin (a .wbmp is unreadable to browsers);
  // links load the next deck here, "#card" ones switch cards.

  var wapTitle = q('.' + PREFIX + 'wap-title');
  var wapScreen = q('.' + PREFIX + 'wap-screen');
  var wapKeys = q('.' + PREFIX + 'wap-keys');
  var wapDeck = null;
  var wapWarning = '';

  var WAP_INLINE = { b: 'b', strong: 'strong', i: 'i', em: 'em', u: 'u', small: 'small', big: 'big' };
  var WAP_SKIP = { go: 1, prev: 1, refresh: 1, noop: 1, postfield: 1, setvar: 1, onevent: 1,
                   timer: 1, head: 1, template: 1, access: 1, meta: 1 };

  function wapMessage(text) {
    var pre = document.createElement('pre');
    pre.textContent = text;
    wapTitle.textContent = '';
    wapKeys.textContent = '';
    wapScreen.textContent = '';
    wapScreen.appendChild(pre);
  }

  function wapLoad(path) {
    framePath = path;
    wapMessage('Loading\u2026');
    fetch(withParams(path), { credentials: 'same-origin' })
      .then(function (r) { return r.arrayBuffer(); })
      .then(function (buf) {
        var head = new TextDecoder('iso-8859-1').decode(buf.slice(0, 200));
        var enc = /encoding="([^"]+)"/i.exec(head);
        var text;
        try { text = new TextDecoder(enc ? enc[1] : 'utf-8').decode(buf); }
        catch (e) { text = new TextDecoder('utf-8').decode(buf); }
        text = text.replace(/&nbsp;/g, '&#160;').replace(/&shy;/g, '&#173;');

        // WML is XML, and a real phone (or its gateway) rejects a deck that
        // isn't well-formed. Shown anyway, read by the forgiving HTML parser,
        // under a warning - so the mistake is visible, not just the result.
        var doc = new DOMParser().parseFromString(text, 'application/xml');
        var error = doc.getElementsByTagName('parsererror')[0];
        wapWarning = '';
        if (error) {
          wapWarning = 'Invalid WML - a real phone may reject this page: ' +
            (error.textContent || '').replace(/\s+/g, ' ')
              .replace(/^.*?following errors:\s*/i, '').replace(/\s*Below is a rendering.*$/i, '')
              .trim().slice(0, 200);
          doc = new DOMParser().parseFromString(text, 'text/html');
        }
        var cards = doc.getElementsByTagName('card');
        if (!cards.length) {
          wapMessage('Not a WML deck:\n\n' + text.slice(0, 2000));
          return;
        }
        wapDeck = doc;
        var hash = path.split('#')[1];
        wapShowCard(hash && doc.querySelector('card[id="' + hash + '"]') || cards[0]);
      })
      .catch(function () { wapMessage('Could not load the WML page.'); });
  }

  function wapShowCard(card) {
    wapTitle.textContent = card.getAttribute('title') || '';
    wapScreen.textContent = '';
    wapKeys.textContent = '';
    if (wapWarning) {
      var warn = document.createElement('p');
      warn.className = PREFIX + 'wap-warning';
      warn.textContent = '\u26a0 ' + wapWarning;
      wapScreen.appendChild(warn);
    }
    Array.prototype.forEach.call(card.childNodes, function (n) {
      var out = wapNode(n);
      if (out) wapScreen.appendChild(out);
    });
    wapScreen.scrollTop = 0;
    scrollPreview(false);
  }

  function wapLink(label, href) {
    var a = document.createElement('a');
    a.href = '#';
    a.className = PREFIX + 'wap-link';
    if (label) a.appendChild(label);
    a.addEventListener('click', function (e) {
      e.preventDefault();
      if (!href) return;
      if (href.charAt(0) === '#') {
        var card = wapDeck && wapDeck.querySelector('card[id="' + href.slice(1) + '"]');
        if (card) wapShowCard(card);
      } else if (!/^[a-z][a-z0-9+.-]*:/i.test(href) || href.indexOf(location.origin) === 0) {
        wapLoad(href.replace(location.origin, ''));
      }
    });
    return a;
  }

  function wapChildren(src, dest) {
    Array.prototype.forEach.call(src.childNodes, function (n) {
      var out = wapNode(n);
      if (out) dest.appendChild(out);
    });
    return dest;
  }

  // Whitespace-only text between block tags is the deck's own indentation.
  function wapNode(n) {
    if (n.nodeType === 3) {
      var block = /^(card|p|table|tr|wml)$/i.test(n.parentNode.localName);
      return block && !/\S/.test(n.nodeValue) ? null : document.createTextNode(n.nodeValue);
    }
    if (n.nodeType !== 1) return null;
    var tag = n.localName.toLowerCase();
    if (WAP_SKIP[tag]) return null;

    if (tag === 'br') return document.createElement('br');
    if (tag === 'p') {
      var p = wapChildren(n, document.createElement('p'));
      var align = n.getAttribute('align');
      if (align === 'center' || align === 'right') p.style.textAlign = align;
      return p;
    }
    if (WAP_INLINE[tag]) return wapChildren(n, document.createElement(WAP_INLINE[tag]));
    if (tag === 'a') return wapLink(wapChildren(n, document.createDocumentFragment()), n.getAttribute('href'));
    if (tag === 'anchor') {
      var go = n.getElementsByTagName('go')[0];
      return wapLink(wapChildren(n, document.createDocumentFragment()), go && go.getAttribute('href'));
    }
    if (tag === 'img') {
      var alt = n.getAttribute('alt') || '';
      var img = document.createElement('img');
      img.alt = alt;
      img.src = (n.getAttribute('src') || '').replace(/\.wbmp$/i, '.png');
      img.addEventListener('error', function () {
        img.replaceWith(document.createTextNode(alt ? '[' + alt + ']' : '[image]'));
      });
      return img;
    }
    if (tag === 'table' || tag === 'tr' || tag === 'td') return wapChildren(n, document.createElement(tag));
    if (tag === 'do') {
      var goDo = n.getElementsByTagName('go')[0];
      var label = document.createTextNode(n.getAttribute('label') || n.getAttribute('type') || 'Option');
      var key = wapLink(label, goDo && goDo.getAttribute('href'));
      key.classList.add(PREFIX + 'wap-key');
      wapKeys.appendChild(key);
      return null;
    }
    if (tag === 'input') {
      var input = document.createElement('input');
      input.disabled = true;
      input.value = n.getAttribute('value') || '';
      return input;
    }
    if (tag === 'select') {
      var select = document.createElement('select');
      select.disabled = true;
      Array.prototype.forEach.call(n.getElementsByTagName('option'), function (o) {
        var opt = document.createElement('option');
        opt.textContent = o.textContent;
        select.appendChild(opt);
      });
      return select;
    }
    return wapChildren(n, document.createDocumentFragment());
  }

  // ---- Start --------------------------------------------------------------

  var saved = load();
  if (saved.size) sizeSelect.value = saved.size;
  applySize();
  var openName = Array.isArray(saved.open) ? saved.open[0] : saved.open;
  var openSection = q('.' + PREFIX + 'section[data-section="' + (openName || 'logo') + '"]');
  if (openSection && openName !== null) { openSection.open = true; activeSection = openSection; }
  if (saved.drawer) setDrawer(true);
  var start = typeof saved.epoch === 'number' && saved.epoch >= -1 && saved.epoch <= 3 ? saved.epoch : 3;
  setEpoch(start);
})();
