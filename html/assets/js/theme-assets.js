// Image uploads of the theme editors' panels - banner, footer, logo and the
// home blog background - on their own pages and in the theme customizer.
// Every panel is a .boat-rudder-dashboard__settings-panel carrying
// data-theme / data-component / data-epoch, which pick the upload directory
// html/content/themes/<theme>/<component>/epoch<epoch>/ (see http_router.c's
// theme_assets_dir()):
//
//   - .boat-rudder-dashboard__asset-list (banner, footer): the directory's
//     files as a file list - thumbnail, name, "Copy URL" (the path to paste
//     into the markup) and "Delete"; a file input without data-target
//     uploads into it.
//   - a single-image field (logo, home blog background): a hidden input
//     holding the stored filename, shown as a one-row list
//     (ul[data-current-for="<field>"]) with the same Copy URL / Delete; the
//     file input with data-target="<field>" uploads and fills it. Delete
//     removes the file, empties every field of the panel using it and saves
//     the form, so the setting never points at a missing file.
//   - epoch -1 (WAP): PNG only; the server keeps it and writes its WBMP
//     twin, the file WML uses - the list shows each pair as one entry.
//   - input[name="mode"] radios (epoch 3 logo): show the matching
//     .boat-rudder-dashboard__logo-mode fieldset, disabling the other.
//
// Changing a field fires 'input' on its form, which the customizer listens
// to for its "unsaved" state.
(function () {
  'use strict';

  // A directory's file list (banner, footer) - not a single-image field's.
  var DIR_LIST = '.boat-rudder-dashboard__asset-list:not([data-current-for])';

  function qs(sel, root) { return (root || document).querySelector(sel); }
  function qsa(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }

  function apiUrl(action, panel, extra) {
    return '/dashboard/api/theme-assets/' + action +
      '?theme=' + encodeURIComponent(panel.dataset.theme) +
      '&component=' + encodeURIComponent(panel.dataset.component) +
      '&epoch=' + encodeURIComponent(panel.dataset.epoch) + (extra || '');
  }

  function assetPath(panel, name) {
    return '/content/themes/' + panel.dataset.theme + '/' + panel.dataset.component +
      '/epoch' + panel.dataset.epoch + '/' + name;
  }

  function upload(panel, file) {
    var fd = new FormData();
    fd.append('file', file);
    return fetch(apiUrl('upload', panel), { method: 'POST', body: fd })
      .then(function (r) {
        if (!r.ok) return r.text().then(function (t) { throw new Error(t || r.status); });
        return r.json();
      });
  }

  function touched(el) {
    var form = el.form || el.closest('form');
    if (form) form.dispatchEvent(new Event('input', { bubbles: true }));
  }

  // Copies the file's path (what the markup needs, no host). The clipboard
  // API needs a secure context (HTTPS or localhost); a LAN address over
  // plain HTTP falls back to copying from a selected, off-screen textarea.
  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text);
    return new Promise(function (resolve, reject) {
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      ta.remove();
      if (ok) resolve(); else reject();
    });
  }

  function listMessage(list, text) {
    list.innerHTML = '';
    var li = document.createElement('li');
    li.className = 'boat-rudder-dashboard__asset-empty';
    li.textContent = text;
    list.appendChild(li);
  }

  function button(label, extraClass, onClick) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = 'boat-rudder-btn boat-rudder-btn--sm' + (extraClass ? ' ' + extraClass : '');
    b.textContent = label;
    b.addEventListener('click', onClick);
    return b;
  }

  // One row per uploaded file: thumbnail, name (the full path on hover),
  // "Copy URL" and "Delete". `entry` is { name, thumb } - on epoch -1 the
  // WBMP (what WML references, so what is copied) shown through its PNG
  // twin (wapEntries()); deleting either one removes both, server-side.
  function assetRow(panel, entry, onDelete) {
    var name = entry.name;
    var path = assetPath(panel, name);
    var li = document.createElement('li');
    li.className = 'boat-rudder-dashboard__asset';

    var thumb = document.createElement('img');
    thumb.className = 'boat-rudder-dashboard__asset-thumb';
    thumb.src = assetPath(panel, entry.thumb || name);
    thumb.alt = '';
    thumb.loading = 'lazy';
    thumb.addEventListener('error', function () {
      var ext = document.createElement('span');
      ext.className = 'boat-rudder-dashboard__asset-thumb';
      ext.textContent = (name.split('.').pop() || '?').toUpperCase();
      thumb.replaceWith(ext);
    });

    var label = document.createElement('span');
    label.className = 'boat-rudder-dashboard__asset-name';
    label.textContent = entry.thumb ? name + ' + PNG' : name;
    label.title = path;

    var copy = button('Copy URL', '', function () {
      copyText(path).then(function () {
        copy.textContent = 'Copied!';
      }, function () {
        copy.textContent = 'Copy failed';
      }).then(function () {
        setTimeout(function () { copy.textContent = 'Copy URL'; }, 1500);
      });
    });

    var del = button('Delete', 'boat-rudder-btn--danger', function () {
      if (onDelete) { onDelete(name); return; }
      if (!confirm('Delete ' + name + '?')) return;
      fetch(apiUrl('delete', panel, '&file=' + encodeURIComponent(name)), { method: 'POST' })
        .then(function () { loadAssets(panel); });
    });

    li.appendChild(thumb);
    li.appendChild(label);
    li.appendChild(copy);
    li.appendChild(del);
    return li;
  }

  // Epoch -1 uploads are a PNG plus the WBMP the server made from it: one
  // entry per pair, named after the WBMP, thumbnailed by the PNG. A file
  // without its twin (an older upload) stays an entry of its own.
  function wapEntries(files) {
    var have = {};
    files.forEach(function (f) { have[f.toLowerCase()] = f; });
    var entries = [];
    files.forEach(function (f) {
      var base = f.replace(/\.[^.]+$/, '');
      var lower = f.toLowerCase();
      if (/\.png$/.test(lower) && have[base.toLowerCase() + '.wbmp']) return;
      var png = /\.wbmp$/.test(lower) && have[base.toLowerCase() + '.png'];
      entries.push(png ? { name: f, thumb: png } : { name: f });
    });
    return entries;
  }

  function loadAssets(panel) {
    var list = qs(DIR_LIST, panel);
    listMessage(list, 'Loading\u2026');

    fetch(apiUrl('list', panel))
      .then(function (r) { return r.json(); })
      .then(function (data) {
        var files = (data.files || []).slice().sort(function (a, b) {
          return a.localeCompare(b, undefined, { sensitivity: 'base', numeric: true });
        });
        if (files.length === 0) {
          listMessage(list, 'No files uploaded yet.');
          return;
        }
        var entries = panel.dataset.epoch === '-1'
          ? wapEntries(files)
          : files.map(function (name) { return { name: name }; });
        list.innerHTML = '';
        entries.forEach(function (entry) { list.appendChild(assetRow(panel, entry)); });
      })
      .catch(function () { listMessage(list, 'Could not load files.'); });
  }

  // A single-image field's list: its current file, or a note that the
  // theme's own image is used.
  function renderField(panel, fieldName) {
    var list = qs('ul[data-current-for="' + fieldName + '"]', panel);
    var input = qs('input[name="' + fieldName + '"]', panel);
    if (!list || !input) return;
    var name = input.value;
    if (!name) {
      listMessage(list, 'No image - the theme\'s own.');
      return;
    }
    var entry = { name: name };
    if (panel.dataset.epoch === '-1' && /\.wbmp$/i.test(name)) entry.thumb = name.replace(/\.wbmp$/i, '.png');
    list.innerHTML = '';
    list.appendChild(assetRow(panel, entry, function (file) {
      if (!confirm('Delete ' + file + '? The theme\'s own image is used instead (saved right away).')) return;
      fetch(apiUrl('delete', panel, '&file=' + encodeURIComponent(file)), { method: 'POST' })
        .then(function (r) {
          if (!r.ok) throw new Error(r.status);
          qsa('input[type="hidden"]', panel).forEach(function (other) {
            if (other.value !== file) return;
            other.value = '';
            var otherList = qs('ul[data-current-for="' + other.name + '"]', panel);
            if (otherList) renderField(panel, other.name);
          });
          var form = input.form;
          if (form.requestSubmit) form.requestSubmit(); else form.submit();
        })
        .catch(function () { alert('Could not delete ' + file + '.'); });
    }));
  }

  qsa('.boat-rudder-dashboard__settings-panel[data-component]').forEach(function (panel) {
    qsa('ul[data-current-for]', panel).forEach(function (list) {
      renderField(panel, list.dataset.currentFor);
    });

    if (qs(DIR_LIST, panel)) loadAssets(panel);

    // Epoch -1 takes PNG only - the server makes the WBMP from it.
    if (panel.dataset.epoch === '-1') {
      qsa('.boat-rudder-dashboard__asset-upload', panel).forEach(function (input) {
        input.accept = 'image/png';
      });
    }

    qsa('.boat-rudder-dashboard__asset-upload', panel).forEach(function (input) {
      input.addEventListener('change', function () {
        var file = input.files[0];
        if (!file) return;
        var targetName = input.dataset.target;

        upload(panel, file).then(function (data) {
          input.value = '';
          if (!targetName) {
            if (qs(DIR_LIST, panel)) loadAssets(panel);
            return;
          }
          if (!data.ok) return;
          var target = qs('input[name="' + targetName + '"]', panel);
          if (target) { target.value = data.filename; touched(target); }
          renderField(panel, targetName);
        }).catch(function (err) {
          input.value = '';
          alert('Upload failed: ' + err.message +
                (panel.dataset.epoch === '-1' ? '\n\nEpoch -1 takes a PNG only.' : ''));
        });
      });
    });

    qsa('input[name="mode"]', panel).forEach(function (radio) {
      radio.addEventListener('change', function () {
        qsa('.boat-rudder-dashboard__logo-mode', panel).forEach(function (fs) {
          fs.hidden = fs.dataset.mode !== radio.value;
          fs.disabled = fs.hidden;
        });
      });
    });
  });
})();
