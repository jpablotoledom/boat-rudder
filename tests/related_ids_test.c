// src/utils/related_ids.c: what the related-posts block keeps of its
// extra_data, at render and at save.

#include "../src/utils/related_ids.h"
#include <stdio.h>
#include <string.h>

#define A "6aebaffe77c8e02a6805ecd2"
#define B "6aebb00877c8e02a6805ecd3"
#define C "6aebb01a77c8e02a6805ecd4"

static int failures = 0;
static int checks = 0;

static void expect_normal(const char *input, const char *exclude, const char *want) {
    char out[RELATED_EXTRA_BUF_SIZE];
    related_ids_normalize(input, exclude, RELATED_POSTS_MAX, out);
    checks++;
    if (strcmp(out, want) != 0) {
        printf("FAIL \"%s\": got \"%s\", wanted \"%s\"\n", input ? input : "(null)", out, want);
        failures++;
    }
}

int main(void) {
    expect_normal("", NULL, "");
    expect_normal(NULL, NULL, "");
    expect_normal(A, NULL, A);
    expect_normal(A "|" B, NULL, A "|" B);
    expect_normal(B "|" A, NULL, B "|" A);          // the order is the author's
    expect_normal(A "|" B "|" C, NULL, A "|" B);    // two at most
    expect_normal(A "|" A, NULL, A);                // no repeats
    expect_normal(A "|" B, A, B);                   // never the entry itself
    expect_normal("nonsense|" B, NULL, B);
    expect_normal("|" A, NULL, A);
    expect_normal(A "|", NULL, A);
    expect_normal("6aebaffe77c8e02a6805ecdZ", NULL, "");   // not hex
    expect_normal("6aebaffe", NULL, "");                   // too short
    expect_normal("6aebaffe77c8e02a6805ecd2ff", NULL, ""); // too long

    // The index's limit: twenty kept of twenty-one, in order.
    char many[32 * 22] = "", want[32 * 21] = "";
    for (int i = 0; i < 21; i++) {
        char id[RELATED_ID_BUF_SIZE];
        snprintf(id, sizeof(id), "6aebaffe77c8e02a6805ec%02x", i);
        if (i) strcat(many, "|");
        strcat(many, id);
        if (i < ENTRY_INDEX_MAX) {
            if (i) strcat(want, "|");
            strcat(want, id);
        }
    }
    char out[RELATED_EXTRA_BUF_SIZE];
    related_ids_normalize(many, NULL, ENTRY_INDEX_MAX, out);
    checks++;
    if (strcmp(out, want) != 0) {
        printf("FAIL index limit: got \"%s\"\n", out);
        failures++;
    }

    char csv[] = A ",x," B "," C;
    const char *ids[2];
    size_t n = related_ids_split_csv(csv, ids, 2);
    checks++;
    if (n != 2 || strcmp(ids[0], A) != 0 || strcmp(ids[1], B) != 0) {
        printf("FAIL split_csv: %zu ids\n", n);
        failures++;
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
