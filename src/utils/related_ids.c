#include "related_ids.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

#define RELATED_ID_LEN 24

static int span_is_id(const char *s, size_t len) {
    if (len != RELATED_ID_LEN) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

int related_id_is_valid(const char *s) {
    return s && span_is_id(s, strlen(s));
}

size_t related_ids_parse(const char *extra_data, const char *exclude,
                         char ids[][RELATED_ID_BUF_SIZE], size_t max) {
    size_t count = 0;
    const char *p = extra_data ? extra_data : "";
    while (count < max) {
        size_t len = strcspn(p, "|");
        if (span_is_id(p, len)) {
            char id[RELATED_ID_BUF_SIZE];
            memcpy(id, p, len);
            id[len] = '\0';

            int keep = !(exclude && strcasecmp(id, exclude) == 0);
            for (size_t i = 0; keep && i < count; i++)
                if (strcasecmp(ids[i], id) == 0) keep = 0;
            if (keep) memcpy(ids[count++], id, RELATED_ID_BUF_SIZE);
        }
        if (p[len] == '\0') break;
        p += len + 1;
    }
    return count;
}

void related_ids_normalize(const char *extra_data, const char *exclude, size_t max, char *out) {
    char ids[ENTRY_INDEX_MAX][RELATED_ID_BUF_SIZE];
    size_t count = related_ids_parse(extra_data, exclude, ids, max < ENTRY_INDEX_MAX ? max : ENTRY_INDEX_MAX);
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (i) strcat(out, "|");
        strcat(out, ids[i]);
    }
}

size_t related_ids_split_csv(char *csv, const char **out, size_t max) {
    size_t count = 0;
    char *p = csv;
    while (p && *p && count < max) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        if (related_id_is_valid(p)) out[count++] = p;
        p = comma ? comma + 1 : NULL;
    }
    return count;
}
