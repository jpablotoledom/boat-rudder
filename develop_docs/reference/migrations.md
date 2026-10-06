# Boat Rudder - Database Migrations

One-off `mongosh` scripts that reshape existing data when the code's expectations change. They
live in `scripts/migrations/` and are **run by hand** - the server never runs them, and there is
no record in the database of which ones were applied.

---

## Conventions

| Rule | Detail |
|---|---|
| Name | `YYYY-MM-DD-<what-it-does>.js` - the date the change landed, so `ls` lists them in order |
| Runtime | Plain `mongosh` script using the global `db`; no npm packages |
| Target | Run against the **site's** database (`mongodb_db` in `configs/settings.conf`), one database per site |
| Idempotent | Mandatory. Running a migration twice must be harmless: select only documents still in the old shape, or consume a staging collection and drop it |
| Self-documenting | The header comment says what changes, why, the exact command line, and why a re-run is safe |
| Output | `print()` a summary of what was touched, so the operator can tell a no-op from a real run |
| Code first | Deploy the code that understands the new shape before (or together with) running the migration. Write migrations so the old shape keeps working (or is safely ignored) until migrated |

---

## Running one

```bash
# 1. Back up first (uses mongodb_db from configs/settings.conf)
./scripts/mongodb_dump.sh

# 2. Run the migration against the site's database
mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/<file>.js
```

Use the same URI as `mongodb_uri`, with the database name appended. `mongosh` must be installed
(it is not a build dependency). Restore with `./scripts/mongodb_restore.sh` if needed
([scripts.md](scripts.md)).

### Keeping track

Because nothing records applied migrations, keep a note per deployment (for example in the host's
own runbook) of which files were run and when. Every migration being idempotent means that, when
in doubt, re-running is safe.

---

## Catalog

### `2026-10-03-merge-analytics.js`

**Why:** to bring analytics recorded elsewhere (the-retro-center-old, same bucket shape) into this
site without overwriting what the running server already counted.

**Steps:**

```bash
mongorestore --db <mongodb_db> --collection page_visits_daily_import  <dump>/page_visits_daily.bson
mongorestore --db <mongodb_db> --collection entry_visits_daily_import <dump>/entry_visits_daily.bson
mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/2026-10-03-merge-analytics.js
```

**What:** for every document in each `*_import` collection, an `updateOne` upsert into the live
collection with `$setOnInsert` of the identity fields (`date`, `year`, `month`, `week`,
`entry_type`, `slug`) and `$inc` of **every numeric counter** (`total` and every nested
`by_*` key), sent as one unordered `bulkWrite`. Days present on both sides are summed; new days
are created.

**Idempotence:** the staging collections are dropped after merging, so a second run finds nothing
(`not found, skipped`) and can't double-count. Restoring the same dump twice and running it twice
*would* double-count - restore once.

---

## Writing a new migration

1. Name it `scripts/migrations/<today>-<verb>-<thing>.js`.
2. Start with the header comment: the reason, the exact before/after shape, the command line, and
   the idempotence argument.
3. Query only documents in the old shape; update with `$set`/`$inc` rather than replacing whole
   documents where possible.
4. `print()` counts at the end.
5. Add it to the catalog above and, if it changes a collection's shape, update
   [data-model.md](data-model.md).

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
