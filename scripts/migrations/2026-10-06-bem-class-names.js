// Renames the old `boat-rudder__block__element` CSS classes stored in the
// database to the BEM names the code uses since 2026-10-06
// (`boat-rudder-block__element--modifier`, see style-guide.md's CSS section):
//   - themes: every string field - the saved epoch 3 stylesheet override
//     (css_epoch3) and the banner/footer/logo markup;
//   - site_settings: every string field;
//   - entries: every string field (a "generic" block's raw HTML can carry
//     classes; prose that mentions the old namespace is rewritten too).
// A saved css_epoch3 also gets its state selectors converted the same way
// the shipped stylesheets were: `.x.active` -> `.x--active`,
// `body.is-dragging` -> `body.boat-rudder-entry-editor-mode--dragging`, ...
//
//   mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/2026-10-06-bem-class-names.js
//
// Re-running is a no-op: the new names contain no `boat-rudder__`, and the
// state conversions only match the old `.x.state` form.

// Elements that had elements of their own (`a__b__c`): `a__b` becomes the
// block `a-b`, so `a__b__c` -> `a-b__c` and `a__b--m` -> `a-b--m`.
var PROMOTED = [
  'dashboard__color-alpha', 'dashboard__color-popover', 'dashboard__vga-palette',
  'entry-editor__add-block', 'entry-editor__advanced', 'entry-editor__autosave-toggle',
  'entry-editor__block', 'entry-editor__block-toolbar', 'entry-editor__img-preview',
  'entry-editor__meta-section', 'entry-editor__preview', 'entry-editor__publish-toggle',
  'entry-editor__toolbar', 'home-blog__item', 'home-content__item', 'menu__icon',
  'navbar__lang', 'navbar__theme'
];

function convertToken(tok) {
  var rest = tok.slice('boat-rudder__'.length);
  var trail = '';
  if (rest.slice(-2) === '__') { rest = rest.slice(0, -2); trail = '__'; }
  var i = rest.indexOf('--');
  var base = i < 0 ? rest : rest.slice(0, i);
  var sep = i < 0 ? '' : '--';
  var mod = i < 0 ? '' : rest.slice(i + 2);
  var parts = base ? base.split('__') : [];
  parts = parts.map(function (p) {
    if (/_onslidelogo$/.test(p)) { sep = '--'; mod = 'onslidelogo'; return p.replace(/_onslidelogo$/, ''); }
    return p;
  }).map(function (p) { return p.replace(/_/g, '-'); });
  mod = mod.replace(/_/g, '-');
  if (parts.length === 0) return 'boat-rudder-';
  var name;
  if (PROMOTED.indexOf(parts.join('__')) >= 0) name = parts.join('-');
  else if (parts.length >= 3) name = parts.slice(0, -1).join('-') + '__' + parts[parts.length - 1];
  else name = parts.join('__');
  return 'boat-rudder-' + name + (sep ? sep + mod : '') + trail;
}

function convertText(s) {
  return s.replace(/(^|[^\w-])(boat-rudder__[A-Za-z0-9_-]*)/g, function (_, pre, tok) {
    return pre + convertToken(tok);
  });
}

var STATES = {
  'active': 'active', 'disabled': 'disabled', 'done': 'done', 'drag-over': 'drag-over',
  'error': 'error', 'is-expanded': 'expanded', 'lang--open': 'open', 'theme--open': 'open',
  'menu--open': 'open', 'on': 'on', 'open': 'open', 'published': 'published',
  'saved': 'saved', 'selected': 'selected'
};

function escapeRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

function convertCss(s) {
  s = convertText(s);
  s = s.split('.boat-rudder-navbar__menu:not(.menu--open)')
       .join('.boat-rudder-navbar__menu:not(.boat-rudder-navbar__menu--open)');
  s = s.replace(/body\.is-dragging(?![\w-])/g, 'body.boat-rudder-entry-editor-mode--dragging');
  s = s.replace(/\.(boat-rudder-[\w-]+)\.type-([a-z-]+)(?![\w-])/g, '.$1--$2');
  Object.keys(STATES).forEach(function (st) {
    var re = new RegExp('\\.(boat-rudder-[\\w-]+)\\.' + escapeRe(st) + '(?![\\w-])', 'g');
    s = s.replace(re, '.$1--' + STATES[st]);
  });
  s = s.replace(/\.navbar__toggle(?![\w-])/g, '.boat-rudder-navbar__toggle');
  s = s.replace(/(\.boat-rudder-navbar__toggle\s+)\.bar(?![\w-])/g, '$1.boat-rudder-navbar__toggle-bar');
  s = s.replace(/\n\.none \{ display: none; \}\n/g, '\n');
  return s;
}

// Walks a document and rewrites every string; returns the $set it needs.
function rewrite(value, path, set, isCssField) {
  if (typeof value === 'string') {
    var next = isCssField(path) ? convertCss(value) : convertText(value);
    if (next !== value) set[path] = next;
  } else if (Array.isArray(value)) {
    value.forEach(function (v, i) { rewrite(v, path + '.' + i, set, isCssField); });
  } else if (value && typeof value === 'object' && !(value instanceof ObjectId) &&
             !(value instanceof Date)) {
    Object.keys(value).forEach(function (k) {
      rewrite(value[k], path ? path + '.' + k : k, set, isCssField);
    });
  }
}

function migrate(collection, isCssField) {
  var touched = 0, fields = 0;
  db[collection].find().forEach(function (doc) {
    var set = {};
    Object.keys(doc).forEach(function (k) { if (k !== '_id') rewrite(doc[k], k, set, isCssField); });
    var n = Object.keys(set).length;
    if (n) {
      db[collection].updateOne({ _id: doc._id }, { $set: set });
      touched++; fields += n;
    }
  });
  print(collection + ': ' + touched + ' document(s), ' + fields + ' field(s) rewritten');
}

migrate('themes', function (path) { return /^css_/.test(path); });
migrate('site_settings', function () { return false; });
migrate('entries', function () { return false; });
