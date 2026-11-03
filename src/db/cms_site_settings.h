#ifndef CMS_SITE_SETTINGS_H
#define CMS_SITE_SETTINGS_H

#include <stdbool.h>

// site_settings is a singleton holding what's genuinely site-wide, not
// theme-specific: the site's name and which theme is currently active.
// Banner/footer content and colors are theme-scoped and live in `themes`
// instead (src/db/cms_themes.h) - see theme-scoped-personalization-plan.md
// for why banner/footer moved out of here.

// Returns a malloc'd copy of site_settings.site_name, or "Boat Rudder" if
// unset/unreachable. Never NULL - site personalization must never fail a
// page.
char *cms_get_site_name(void);

// db.site_settings.updateOne({}, {$set: {site_name}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_site_name(const char *name);

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

// site_settings.hide_home_blog: true hides the "Latest Blog Posts" list on
// the home page (every epoch). false if unset/unreachable - the list shows
// by default.
bool cms_get_hide_home_blog(void);

// db.site_settings.updateOne({}, {$set: {hide_home_blog}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_hide_home_blog(bool hide);

// "Under construction" mode: while site_settings.under_construction is true,
// every visitor but an Administrador (and an Autor, with
// cms_get_under_construction_authors()) gets the page entry whose link is
// site_settings.under_construction_page instead of the site's content (see
// http_router.c's serve_under_construction()). Fills *enabled (false if
// unset/unreachable) and, if page_link is non-NULL, *page_link with a
// malloc'd copy of the link ("" if unset; NULL only on allocation failure).
void cms_get_under_construction(bool *enabled, char **page_link);

// site_settings.under_construction_authors: whether Autores may still sign
// in, edit, and see the real site while it is under construction (an
// Administrador always may). false if unset/unreachable.
bool cms_get_under_construction_authors(void);

// db.site_settings.updateOne({}, {$set: {under_construction,
// under_construction_page, under_construction_authors}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_under_construction(bool enabled, const char *page_link, bool authors);

// site_settings.comments_enabled: blog comments on/off for the whole site
// (comments.h). false if unset/unreachable.
bool cms_get_comments_enabled(void);

// db.site_settings.updateOne({}, {$set: {comments_enabled}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_comments_enabled(bool enabled);

// Machine translation in the entry editor (entry-translation-plan.md):
// site_settings.translate_provider ("" off, "azure" Azure AI Translator
// through the server, "local" a LibreTranslate-compatible server the
// editor's browser calls), the Azure key and its resource's region, and the
// local server's URL and optional key.
#define TRANSLATE_LOCAL_URL_DEFAULT "http://localhost:5000"
typedef struct {
    char provider[8];
    char azure_key[128];
    char azure_region[64];
    char local_url[256];
    char local_key[128];
} CmsTranslateSettings;

// Fills `out`: provider "" and local_url TRANSLATE_LOCAL_URL_DEFAULT where
// unset/unreachable, "" for the keys.
void cms_get_translate_settings(CmsTranslateSettings *out);

// $set of the five fields, upserted; a NULL `azure_key` keeps the stored
// one. Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_translate_settings(const char *provider, const char *azure_key, const char *azure_region,
                                  const char *local_url, const char *local_key);

#define SITE_SECRET_BYTES 32

// The site's secret key: 32 random bytes generated once (libsodium) and kept
// in site_settings.secret (hex), then cached for the process. It keys the
// HMACs that make challenge tokens and the comment "remember me" cookie
// unforgeable, and hashes visitor addresses before they're stored. Fills
// `key` and returns 0, or -1 if MongoDB is not ready / on a DB error.
int cms_get_site_secret(unsigned char key[SITE_SECRET_BYTES]);

#endif // CMS_SITE_SETTINGS_H
