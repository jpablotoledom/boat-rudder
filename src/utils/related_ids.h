#ifndef RELATED_IDS_H
#define RELATED_IDS_H

#include <stddef.h>

// The extra_data of the blocks that point at other entries: ids joined by
// '|', each a 24-digit hex ObjectId - related-posts (two at most; none means
// automatic) and entry-index. No database in here, so it can be tested alone.

#define RELATED_POSTS_MAX 2
#define ENTRY_INDEX_MAX 20
#define RELATED_ID_BUF_SIZE 25
// The longest list, its separators and terminator.
#define RELATED_EXTRA_BUF_SIZE (ENTRY_INDEX_MAX * RELATED_ID_BUF_SIZE)

// Whether `s` is a 24-digit hex string.
int related_id_is_valid(const char *s);

// Reads the ids of `extra_data` in order, up to `max`: anything that isn't an
// id, a repeated id and `exclude` (the entry's own id, or NULL) are skipped.
// Returns how many were written to ids[].
size_t related_ids_parse(const char *extra_data, const char *exclude,
                         char ids[][RELATED_ID_BUF_SIZE], size_t max);

// `extra_data` as it should be stored: what related_ids_parse() keeps, joined
// by '|'. out needs RELATED_EXTRA_BUF_SIZE bytes; max <= ENTRY_INDEX_MAX.
void related_ids_normalize(const char *extra_data, const char *exclude, size_t max, char *out);

// Splits a comma-separated list in place, keeping the valid ids, up to `max`.
// Returns how many pointers (into csv) were written to out[].
size_t related_ids_split_csv(char *csv, const char **out, size_t max);

#endif // RELATED_IDS_H
