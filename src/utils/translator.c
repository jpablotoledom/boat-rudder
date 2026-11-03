#include "translator.h"
#include "config_loader.h"
#include "log.h"
#include <bson/bson.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AZURE_URL            "https://api.cognitive.microsofttranslator.com/translate"
#define CONNECT_TIMEOUT_SECS 10
#define TIMEOUT_SECS         60
#define MAX_RESPONSE_BYTES   (1024 * 1024)

void translator_init(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        LOG_ERROR("translator: curl_global_init failed");
}

void translator_cleanup(void) {
    curl_global_cleanup();
}

typedef struct {
    char  *data;
    size_t len;
} Buffer;

static size_t collect(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > MAX_RESPONSE_BYTES) return 0;
    char *grown = realloc(b->data, b->len + n + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, ptr, n);
    b->data = grown;
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

// Azure's language code for an ISO 639-1 one: the same, but Chinese needs
// its script.
static const char *azure_lang(const char *code) {
    return strcmp(code, "zh") == 0 ? "zh-Hans" : code;
}

// Azure answers a JSON array, which libbson only reads inside a document:
// parses {"a": <body>}. NULL if it isn't JSON.
static bson_t *parse_wrapped(const Buffer *body) {
    if (!body->data) return NULL;
    size_t len = body->len + 7;
    char *json = malloc(len + 1);
    if (!json) return NULL;
    snprintf(json, len + 1, "{\"a\":%s}", body->data);
    bson_t *doc = bson_new_from_json((const uint8_t *)json, -1, NULL);
    free(json);
    return doc;
}

// The message Azure sends with an error ({"error": {"code", "message"}}),
// else a reason for its status. A refused key most often means a missing or
// wrong region, so the reason names the one sent.
static void azure_error(long status, const char *region, const Buffer *body, char *error, size_t error_size) {
    bson_t *doc = parse_wrapped(body);
    bson_iter_t iter;
    const char *message = doc && bson_iter_init(&iter, doc) && bson_iter_find_descendant(&iter, "a.error.message", &iter) &&
                                  BSON_ITER_HOLDS_UTF8(&iter)
                              ? bson_iter_utf8(&iter, NULL) : NULL;
    if (status == 401 && !region[0])
        snprintf(error, error_size, "Azure refused the key. No region is saved: add your Translator "
                 "resource's Location/Region (e.g. eastus) in Settings, Translation");
    else if (status == 401)
        snprintf(error, error_size, "Azure refused the key or the region (%.40s): check both in "
                 "Settings, Translation", region);
    else if (status == 403)
        snprintf(error, error_size, "Azure quota used up for this month");
    else if (status == 429)
        snprintf(error, error_size, "Azure is busy, try again in a moment");
    else if (message)
        snprintf(error, error_size, "Azure: %.160s", message);
    else
        snprintf(error, error_size, "Azure answered %ld", status);
    if (doc) bson_destroy(doc);
}

// Reads [{"translations": [{"text": ...}]}, ...] into out[0..count).
static int parse_translations(const Buffer *body, size_t count, char **out) {
    bson_t *doc = parse_wrapped(body);
    if (!doc) return -1;
    bson_iter_t iter, list;
    size_t n = 0;
    if (bson_iter_init_find(&iter, doc, "a") && BSON_ITER_HOLDS_ARRAY(&iter) &&
        bson_iter_recurse(&iter, &list)) {
        while (n < count && bson_iter_next(&list)) {
            bson_iter_t text;
            if (!BSON_ITER_HOLDS_DOCUMENT(&list) || !bson_iter_recurse(&list, &text) ||
                !bson_iter_find_descendant(&text, "translations.0.text", &text) ||
                !BSON_ITER_HOLDS_UTF8(&text))
                break;
            out[n] = strdup(bson_iter_utf8(&text, NULL));
            if (!out[n]) break;
            n++;
        }
    }
    bson_destroy(doc);
    if (n == count) return 0;
    for (size_t i = 0; i < n; i++) free(out[i]);
    return -1;
}

int translator_azure(const char *key, const char *region, const char *const *texts, size_t count,
                     const char *source, const char *target, int html, char **out,
                     char *error, size_t error_size) {
    if (!key || !key[0]) {
        snprintf(error, error_size, "No Azure key is saved");
        return -1;
    }
    if (count == 0) return 0;

    char url[512];
    snprintf(url, sizeof(url), "%s?api-version=3.0&from=%s&to=%s&textType=%s",
             translate_azure_url[0] ? translate_azure_url : AZURE_URL,
             azure_lang(source), azure_lang(target), html ? "html" : "plain");

    // The body is [{"Text": ...}, ...]; libbson writes it - every string
    // escaped, UTF-8 kept - as the array "a" of a document, cut out below.
    bson_t request = BSON_INITIALIZER, list;
    bson_append_array_begin(&request, "a", -1, &list);
    for (size_t i = 0; i < count; i++) {
        char index[16];
        const char *index_key;
        bson_uint32_to_string((uint32_t)i, &index_key, index, sizeof(index));
        bson_t item;
        bson_append_document_begin(&list, index_key, -1, &item);
        bson_append_utf8(&item, "Text", -1, texts[i], -1);
        bson_append_document_end(&list, &item);
    }
    bson_append_array_end(&request, &list);
    char *json = bson_as_relaxed_extended_json(&request, NULL);
    bson_destroy(&request);
    // {"a" : [ ... ] } -> [ ... ]
    char *array = json ? strchr(json, '[') : NULL;
    char *array_end = json ? strrchr(json, ']') : NULL;
    if (array && array_end) array_end[1] = '\0';

    char auth[200], region_header[100];
    snprintf(auth, sizeof(auth), "Ocp-Apim-Subscription-Key: %s", key);
    snprintf(region_header, sizeof(region_header), "Ocp-Apim-Subscription-Region: %s", region ? region : "");
    CURL *curl = array && array_end ? curl_easy_init() : NULL;
    if (!curl) {
        bson_free(json);
        snprintf(error, error_size, "Out of memory");
        return -1;
    }
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    if (region && region[0]) headers = curl_slist_append(headers, region_header);
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");

    Buffer body = { 0 };
    char curl_error[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, array);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

    int rc = -1;
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (res != CURLE_OK) {
        LOG_WARN("translator: %s: %s", url, curl_error[0] ? curl_error : curl_easy_strerror(res));
        snprintf(error, error_size, "Could not reach Azure");
    } else if (status != 200) {
        azure_error(status, region ? region : "", &body, error, error_size);
        LOG_WARN("translator: Azure answered %ld", status);
    } else if (parse_translations(&body, count, out) != 0) {
        snprintf(error, error_size, "Azure's answer could not be read");
    } else {
        rc = 0;
    }

    free(body.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    bson_free(json);
    return rc;
}
