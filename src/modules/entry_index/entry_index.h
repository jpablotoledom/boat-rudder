#ifndef ENTRY_INDEX_H
#define ENTRY_INDEX_H

// The entry-index content block: a table-like list of up to ENTRY_INDEX_MAX
// (utils/related_ids.h) blog entries chosen by the author, in their order -
// picture, title and summary, each row linking to its entry. Its extra_data
// is the entries' ids joined by '|'; it stores no text of its own.

// The block's HTML/WML for `epoch` in the reader's language, "" when none of
// its entries can be shown. `entry_id` is the entry it sits in (never listed;
// may be NULL). Malloc'd; NULL on a missing template or allocation failure.
char *entry_index_render(const char *extra_data, const char *entry_id, int epoch);

#endif // ENTRY_INDEX_H
