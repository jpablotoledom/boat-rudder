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
// every visitor but an Administrador gets the page entry whose link is
// site_settings.under_construction_page instead of the site's content (see
// http_router.c's serve_under_construction()). Fills *enabled (false if
// unset/unreachable) and, if page_link is non-NULL, *page_link with a
// malloc'd copy of the link ("" if unset; NULL only on allocation failure).
void cms_get_under_construction(bool *enabled, char **page_link);

// db.site_settings.updateOne({}, {$set: {under_construction,
// under_construction_page}}, {upsert: true}). Returns 0 on success, -1 on a
// DB error or if mongodb is not ready.
int cms_update_under_construction(bool enabled, const char *page_link);

#endif // CMS_SITE_SETTINGS_H
