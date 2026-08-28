#ifndef CMS_SITE_SETTINGS_H
#define CMS_SITE_SETTINGS_H

// Number of epochs with a personalizable banner/footer slot (-1, 0, 1, 2, 3).
#define SITE_SETTINGS_EPOCH_COUNT 5

// Maps epoch (-1..3) to a banner_html/footer_html array index (0..4), or -1
// if `epoch` is out of that range.
int cms_site_settings_epoch_index(int epoch);

// The `site_settings` singleton document: site identity fields an admin can
// edit from /dashboard/settings instead of redeploying.
typedef struct {
    char site_name[128]; // "Boat Rudder" if unset/unreachable

    // Raw markup per epoch (index via cms_site_settings_epoch_index()).
    // Always malloc'd, "" (not NULL) when the DB has no value for that
    // epoch - this is the *stored* value, distinct from the
    // file-fallback-resolved value cms_get_site_banner()/
    // cms_get_site_footer() return. The admin forms need to tell "nothing
    // saved yet" apart from "saved text that happens to equal the file".
    char *banner_html[SITE_SETTINGS_EPOCH_COUNT];
    char *footer_html[SITE_SETTINGS_EPOCH_COUNT];
} CmsSiteSettings;

// db.site_settings.findOne({}). Fills `out` with the stored document, or
// with the defaults described above for any missing field/document/
// unreachable DB. Always returns 1 - site personalization must never fail a
// page or an admin form. Caller must call cms_site_settings_free(out).
int cms_get_site_settings(CmsSiteSettings *out);

// Frees every banner_html/footer_html entry. Safe to call after a failed
// cms_get_site_settings() (still returns 1, so this is always paired).
void cms_site_settings_free(CmsSiteSettings *settings);

// db.site_settings.updateOne({}, {$set: {site_name}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_site_name(const char *name);

// db.site_settings.updateOne({}, {$set: {"banner_html.<field for epoch>":
// html}}, {upsert: true}). Returns 0 on success, -1 if epoch is outside
// -1..3, on a DB error, or if mongodb is not ready. An empty `html` clears
// that epoch back to the on-disk theme default.
int cms_update_site_banner(int epoch, const char *html);
int cms_update_site_footer(int epoch, const char *html);

// Render-path getters: return a malloc'd string, never NULL - the DB value
// for `epoch` if non-empty, otherwise the current on-disk theme file's
// contents (mainbanner/mainbanner_epoch<N>.html / layout/footer_epoch<N>.html),
// so a fresh install or an unedited epoch renders exactly as it does today.
// Used by mainbanner() and page_layout_wrap() instead of
// cms_get_site_settings(), which loads every field and is scoped to the
// /dashboard/settings forms.
char *cms_get_site_banner(int epoch);
char *cms_get_site_footer(int epoch);

// Returns a malloc'd copy of site_settings.site_name, or "Boat Rudder" if
// unset/unreachable. Cheaper than cms_get_site_settings() for render-path
// callers that only need the name.
char *cms_get_site_name(void);

// Returns a malloc'd theme key: site_settings.active_theme if set and a
// directory exists at html/themes/<key>/, otherwise
// configs/settings.conf's `theme` value. Never NULL/empty - "which theme
// renders this request" must never fail. This is what request_theme.c
// calls once per request; render-path code should go through
// request_theme() instead of calling this directly.
char *cms_get_active_theme_key(void);

// db.site_settings.updateOne({}, {$set: {active_theme: key}}, {upsert:
// true}). Returns 0 on success, -1 if html/themes/<key>/ does not exist,
// on a DB error, or if mongodb is not ready.
int cms_set_active_theme(const char *key);

#endif // CMS_SITE_SETTINGS_H
