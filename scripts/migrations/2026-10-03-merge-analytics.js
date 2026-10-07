// Merges analytics day-buckets restored into staging collections
// (page_visits_daily_import / entry_visits_daily_import) into the live
// page_visits_daily / entry_visits_daily by ADDING every counter - so visits
// the running server already recorded on the same days are kept, not
// overwritten the way a `mongorestore --drop` would.
//
// Used to bring the-retro-center-old's analytics (same bucket shape - see
// analytics.c) into this site's database:
//   mongorestore --db <mongodb_db> --collection page_visits_daily_import  <dump>/page_visits_daily.bson
//   mongorestore --db <mongodb_db> --collection entry_visits_daily_import <dump>/entry_visits_daily.bson
//   mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/2026-10-03-merge-analytics.js
// The staging collections are dropped once merged, so running it again with
// nothing restored is a no-op (and never double-counts).

// Flattens a bucket's numeric counters into dotted $inc paths:
// { total: 3, by_epoch: { epoch3: 2 } } -> { "total": 3, "by_epoch.epoch3": 2 }
function counters(doc, prefix, out) {
  Object.keys(doc).forEach(function (key) {
    var value = doc[key];
    var path = prefix ? prefix + '.' + key : key;
    if (typeof value === 'number' || value instanceof NumberInt || value instanceof NumberLong) {
      if (prefix || key === 'total') out[path] = NumberInt(Number(value));
    } else if (value && typeof value === 'object' && !(value instanceof ObjectId)) {
      counters(value, path, out);
    }
  });
  return out;
}

// Fields copied as-is when the day-bucket doesn't exist yet in the target.
var IDENTITY_FIELDS = ['date', 'year', 'month', 'week', 'entry_type', 'slug'];

function mergeCollection(source, target) {
  if (!db.getCollectionNames().includes(source)) {
    print(source + ': not found, skipped');
    return;
  }
  var ops = [];
  db[source].find().forEach(function (doc) {
    var setOnInsert = {};
    IDENTITY_FIELDS.forEach(function (f) { if (doc[f] !== undefined) setOnInsert[f] = doc[f]; });
    ops.push({ updateOne: {
      filter: { _id: doc._id },
      update: { $setOnInsert: setOnInsert, $inc: counters(doc, '', {}) },
      upsert: true
    } });
  });
  if (ops.length) {
    var r = db[target].bulkWrite(ops, { ordered: false });
    print(target + ': ' + ops.length + ' buckets merged (' + r.upsertedCount + ' new days, ' +
          r.modifiedCount + ' existing days summed)');
  }
  db[source].drop();
}

mergeCollection('page_visits_daily_import', 'page_visits_daily');
mergeCollection('entry_visits_daily_import', 'entry_visits_daily');
