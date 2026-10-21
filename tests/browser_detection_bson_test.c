// src/db/browser_detection_bson.c: the factory rules survive a round trip
// through BSON, the JSON shape the dashboard posts parses, and malformed
// input is refused rather than silently defaulted or truncated.

#include "../src/db/browser_detection_bson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *what, int condition) {
    checks++;
    if (!condition) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static int same_rule(const BrowserRule *a, const BrowserRule *b) {
    if (strcmp(a->name, b->name) || a->enabled != b->enabled ||
        strcmp(a->version_after, b->version_after) || a->epoch != b->epoch ||
        a->dashboard != b->dashboard || a->contains_count != b->contains_count ||
        a->excludes_count != b->excludes_count || a->tier_count != b->tier_count)
        return 0;
    for (size_t i = 0; i < a->contains_count; i++)
        if (strcmp(a->contains[i], b->contains[i])) return 0;
    for (size_t i = 0; i < a->excludes_count; i++)
        if (strcmp(a->excludes[i], b->excludes[i])) return 0;
    for (size_t i = 0; i < a->tier_count; i++)
        if (strcmp(a->tiers[i].from, b->tiers[i].from) || a->tiers[i].epoch != b->tiers[i].epoch ||
            a->tiers[i].dashboard != b->tiers[i].dashboard)
            return 0;
    return 1;
}

static void round_trip(void) {
    BrowserDetection *factory = browser_detection_factory();
    BrowserDetection *back = malloc(sizeof(*back));
    bson_t doc = BSON_INITIALIZER;
    char error[160] = "";

    browser_detection_to_bson(factory, &doc);
    expect("factory parses back", browser_detection_from_bson(&doc, back, error, sizeof(error)) == 0);
    expect("same fallback", back->fallback_epoch == factory->fallback_epoch);
    expect("same rule count", back->rule_count == factory->rule_count);
    expect("same case count", back->case_count == factory->case_count);
    for (size_t i = 0; i < factory->rule_count && i < back->rule_count; i++)
        expect(factory->rules[i].name, same_rule(&factory->rules[i], &back->rules[i]));
    for (size_t i = 0; i < factory->case_count && i < back->case_count; i++) {
        const BrowserCase *a = &factory->cases[i], *b = &back->cases[i];
        expect(a->name, !strcmp(a->name, b->name) && !strcmp(a->user_agent, b->user_agent) &&
                        a->epoch == b->epoch && a->dashboard == b->dashboard);
    }
    expect("round trip still valid", browser_detection_validate(back, NULL) == NULL);

    bson_destroy(&doc);
    free(factory);
    free(back);
}

// Parses `json` into a fresh BrowserDetection; returns from_bson's result.
static int parse_json(const char *json, BrowserDetection *out, char *error, size_t size) {
    bson_t doc;
    bson_error_t bson_error;
    if (!bson_init_from_json(&doc, json, -1, &bson_error)) {
        // Callers' buffers are 160 bytes: keep the message to what fits.
        snprintf(error, size, "not JSON: %.140s", bson_error.message);
        return -2;
    }
    int rc = browser_detection_from_bson(&doc, out, error, size);
    bson_destroy(&doc);
    return rc;
}

static void from_json(void) {
    BrowserDetection *d = malloc(sizeof(*d));
    char error[160];

    // What the page posts: plain JSON numbers and booleans.
    const char *ok =
        "{\"fallback_epoch\": 1,"
        " \"rules\": [{\"name\": \"Chrome\", \"enabled\": true, \"contains\": [\"Chrome/\"],"
        "             \"excludes\": [], \"version_after\": \"Chrome/\","
        "             \"tiers\": [{\"from\": \"87\", \"epoch\": 3, \"dashboard\": false}],"
        "             \"epoch\": 2, \"dashboard\": false}],"
        " \"cases\": [{\"name\": \"c\", \"user_agent\": \"Chrome/90\", \"epoch\": 3, \"dashboard\": false}]}";
    expect("page JSON parses", parse_json(ok, d, error, sizeof(error)) == 0);
    expect("...one rule, one tier, one case",
           d->rule_count == 1 && d->rules[0].tier_count == 1 && d->case_count == 1);
    BrowserMatch m;
    browser_detection_classify(d, "Chrome/90", &m);
    expect("...and classifies", m.epoch == 3 && m.rule == 0);

    char long_token[200];
    memset(long_token, 'a', sizeof(long_token) - 1);
    long_token[sizeof(long_token) - 1] = '\0';
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"fallback_epoch\": 1, \"rules\": [{\"name\": \"x\", \"enabled\": true,"
             " \"contains\": [\"%s\"], \"excludes\": [], \"version_after\": \"\", \"tiers\": [],"
             " \"epoch\": 2, \"dashboard\": false}], \"cases\": []}", long_token);
    expect("too-long token refused, not truncated", parse_json(json, d, error, sizeof(error)) == -1);

    expect("missing field refused",
           parse_json("{\"fallback_epoch\": 1, \"rules\": [{\"name\": \"x\"}], \"cases\": []}",
                      d, error, sizeof(error)) == -1);
    expect("wrong type refused",
           parse_json("{\"fallback_epoch\": \"1\", \"rules\": [], \"cases\": []}",
                      d, error, sizeof(error)) == -1);
    expect("fractional epoch refused",
           parse_json("{\"fallback_epoch\": 1.5, \"rules\": [], \"cases\": []}",
                      d, error, sizeof(error)) == -1);
    expect("empty lists are fine",
           parse_json("{\"fallback_epoch\": 2, \"rules\": [], \"cases\": []}",
                      d, error, sizeof(error)) == 0 && d->rule_count == 0);
    free(d);
}

int main(void) {
    round_trip();
    from_json();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
