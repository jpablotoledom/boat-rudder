#include "cms_site_settings.h"
#include "mongodb_manager.h"
#include "../utils/config_loader.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SITE_SETTINGS_COLLECTION "site_settings"
#define SITE_NAME_DEFAULT "Boat Rudder"

int cms_site_settings_epoch_index(int epoch) {
    if (epoch < -1 || epoch > 3) return -1;
    return epoch + 1;
}

// BSON can't hold a field starting with '-', so epoch -1 gets a spelled-out
// key; every other epoch's key mirrors its on-disk filename suffix.
static const char *epoch_field_name(int epoch) {
    switch (epoch) {
        case -1: return "epoch_neg1";
        case 0:  return "epoch0";
        case 1:  return "epoch1";
        case 2:  return "epoch2";
        case 3:  return "epoch3";
        default: return NULL;
    }
}

// Like bson_lang.c's resolve_lang_map(), but for a plain map<string,string>
// with no language fallback: returns `parent.field.key`, or "" if any of
// `field`, `key`, or `parent` itself is absent/not a string.
static char *nested_string_field(const bson_t *parent, const char *field, const char *key) {
    bson_iter_t iter, sub_iter;

    if (!bson_iter_init_find(&iter, parent, field) || !BSON_ITER_HOLDS_DOCUMENT(&iter))
        return strdup("");

    uint32_t len;
    const uint8_t *data;
    bson_iter_document(&iter, &len, &data);

    bson_t sub;
    bson_init_static(&sub, data, len);

    if (bson_iter_init_find(&sub_iter, &sub, key) && BSON_ITER_HOLDS_UTF8(&sub_iter))
        return strdup(bson_iter_utf8(&sub_iter, NULL));

    return strdup("");
}

static char *load_theme_file(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *body = path ? read_file_to_string(path) : NULL;
    free(path);
    return body ? body : strdup("");
}

int cms_get_site_settings(CmsSiteSettings *out) {
    strncpy(out->site_name, SITE_NAME_DEFAULT, sizeof(out->site_name) - 1);
    out->site_name[sizeof(out->site_name) - 1] = '\0';
    for (int i = 0; i < SITE_SETTINGS_EPOCH_COUNT; i++) {
        out->banner_html[i] = strdup("");
        out->footer_html[i] = strdup("");
    }

    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return 1;

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "site_name") && BSON_ITER_HOLDS_UTF8(&iter)) {
            strncpy(out->site_name, bson_iter_utf8(&iter, NULL), sizeof(out->site_name) - 1);
            out->site_name[sizeof(out->site_name) - 1] = '\0';
        }

        for (int epoch = -1; epoch <= 3; epoch++) {
            int i = cms_site_settings_epoch_index(epoch);
            const char *field = epoch_field_name(epoch);

            free(out->banner_html[i]);
            out->banner_html[i] = nested_string_field(doc, "banner_html", field);

            free(out->footer_html[i]);
            out->footer_html[i] = nested_string_field(doc, "footer_html", field);
        }
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_site_settings: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return 1;
}

void cms_site_settings_free(CmsSiteSettings *settings) {
    if (!settings) return;
    for (int i = 0; i < SITE_SETTINGS_EPOCH_COUNT; i++) {
        free(settings->banner_html[i]);
        free(settings->footer_html[i]);
        settings->banner_html[i] = NULL;
        settings->footer_html[i] = NULL;
    }
}

// site_settings is a singleton: every write is an upsert against the empty
// filter {}, same shape as mongoc_collection_find_with_opts()'s read side
// above - the first save creates the document, every later save updates it.
static int upsert_set(bson_t *set_doc_owner /* consumed */) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) {
        bson_destroy(set_doc_owner);
        return -1;
    }

    bson_t *query = bson_new();
    bson_t *update = bson_new();
    BSON_APPEND_DOCUMENT(update, "$set", set_doc_owner);
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_site_settings: upsert failed: %s", error.message);

    bson_destroy(set_doc_owner);
    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_site_name(const char *name) {
    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "site_name", -1, name, -1);
    return upsert_set(set_doc);
}

// Shared by cms_update_site_banner()/cms_update_site_footer(): $set
// site_settings.<top_field>.<epoch field> = html.
static int update_epoch_field(const char *top_field, int epoch, const char *html) {
    const char *field = epoch_field_name(epoch);
    if (!field) return -1;

    char dotted[64];
    snprintf(dotted, sizeof(dotted), "%s.%s", top_field, field);

    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, dotted, -1, html, -1);
    return upsert_set(set_doc);
}

int cms_update_site_banner(int epoch, const char *html) {
    return update_epoch_field("banner_html", epoch, html);
}

int cms_update_site_footer(int epoch, const char *html) {
    return update_epoch_field("footer_html", epoch, html);
}

char *cms_get_site_banner(int epoch) {
    if (cms_site_settings_epoch_index(epoch) < 0) return strdup("");

    char *db_value = NULL;
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);
        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc))
            db_value = nested_string_field(doc, "banner_html", epoch_field_name(epoch));
        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (db_value && db_value[0]) return db_value;
    free(db_value);
    return load_theme_file("mainbanner/mainbanner_epoch%d.html", epoch);
}

char *cms_get_site_footer(int epoch) {
    if (cms_site_settings_epoch_index(epoch) < 0) return strdup("");

    char *db_value = NULL;
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);
        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc))
            db_value = nested_string_field(doc, "footer_html", epoch_field_name(epoch));
        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (db_value && db_value[0]) return db_value;
    free(db_value);
    return load_theme_file("layout/footer_epoch%d.html", epoch);
}

char *cms_get_site_name(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return strdup(SITE_NAME_DEFAULT);

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "site_name") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup(SITE_NAME_DEFAULT);
}

// Not via generate_url_theme() - that function calls request_theme(), which
// calls cms_get_active_theme_key() (this file) to resolve the value being
// validated here; going through generate_url_theme() would just be a more
// roundabout way of building the same "./html/themes/<key>" path.
static int theme_directory_exists(const char *key) {
    if (!key || !key[0]) return 0;

    char path[256];
    int n = snprintf(path, sizeof(path), "./html/themes/%s", key);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *cms_get_active_theme_key(void) {
    char *db_value = NULL;

    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            if (bson_iter_init_find(&iter, doc, "active_theme") && BSON_ITER_HOLDS_UTF8(&iter))
                db_value = strdup(bson_iter_utf8(&iter, NULL));
        }

        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (db_value && theme_directory_exists(db_value)) return db_value;
    free(db_value);
    return strdup(theme); // configs/settings.conf fallback - see request_theme.h
}

int cms_set_active_theme(const char *key) {
    if (!theme_directory_exists(key)) return -1;

    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "active_theme", -1, key, -1);
    return upsert_set(set_doc);
}
