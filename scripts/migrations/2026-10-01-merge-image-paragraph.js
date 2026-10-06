// Folds every "image-paragraph" content block into an "image" block with a
// float alignment - the two block types were merged (see entry_page.c's
// parse_image_options(): "float-left"/"float-right").
//
// Run once per database, against the site's mongodb_db:
//   mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/2026-10-01-merge-image-paragraph.js
// Safe to run again: it only ever touches blocks still typed image-paragraph.
//
// Per block:
//   type        "image-paragraph"       -> "image"
//   text        ".../photo_full.jpg"    -> ".../photo.jpg"  (image blocks store the bare
//                                          path; the renderer appends the size suffix)
//   extra_data  "left" | "right" | ""   -> "|30|float-left" | "|30|float-right" | "|30|left"
//               (caption|width|align; 30% is the closest size to the old fixed 250px)

function stripFull(path) {
  return typeof path === 'string' ? path.replace(/_full(\.[A-Za-z0-9]+)$/, '$1') : path;
}

function convertText(text) {
  if (text && typeof text === 'object') {
    var out = {};
    Object.keys(text).forEach(function (lang) { out[lang] = stripFull(text[lang]); });
    return out;
  }
  return stripFull(text);
}

function convertExtra(extra) {
  if (extra === 'left') return '|30|float-left';
  if (extra === 'right') return '|30|float-right';
  return '|30|left';
}

var entries = 0, blocks = 0;
db.entries.find({ 'content.type': 'image-paragraph' }).forEach(function (entry) {
  var changed = 0;
  var content = entry.content.map(function (block) {
    if (block.type !== 'image-paragraph') return block;
    changed++;
    return Object.assign({}, block, {
      type: 'image',
      text: convertText(block.text),
      extra_data: convertExtra(block.extra_data),
    });
  });
  if (changed) {
    db.entries.updateOne({ _id: entry._id }, { $set: { content: content } });
    entries++;
    blocks += changed;
    print('  ' + (entry.link || entry._id) + ': ' + changed + ' block(s)');
  }
});
print('Migrated ' + blocks + ' image-paragraph block(s) in ' + entries + ' entr' + (entries === 1 ? 'y' : 'ies') + '.');
