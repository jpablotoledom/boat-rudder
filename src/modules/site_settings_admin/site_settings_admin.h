#ifndef SITE_SETTINGS_ADMIN_H
#define SITE_SETTINGS_ADMIN_H

#include "../../db/cms_themes.h"
#include "../../utils/detect_epoch.h"
#include <stdbool.h>
#include <stddef.h>

// /dashboard/settings - the site name form, plus links to Themes and
// Preview. `error_message` is shown above the form if non-NULL/non-empty.
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *site_settings_general_page(int epoch, const char *site_name, const char *error_message);

// /dashboard/settings/themes/<key>/banner and .../footer - one <textarea>
// + asset upload/browse widget per epoch (-1..3) for theme `key`
// specifically (not the whole site - see
// theme-scoped-personalization-plan.md), pre-filled from
// `values[epoch_to_index(epoch)]` (the *stored* DB value for that theme,
// "" if unset - not the file-fallback-resolved value, so the admin can
// tell "nothing saved" apart from "saved text matching the file"). The
// asset browser's upload/list/delete calls carry `key` explicitly (as
// `&theme=<key>`), never the admin's own active theme, so editing one
// theme's assets can never land in another's directory. Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
char *site_settings_banner_page(int epoch, const char *key, char *const values[EPOCH_COUNT]);
char *site_settings_footer_page(int epoch, const char *key, char *const values[EPOCH_COUNT]);

// /dashboard/settings/themes/<key>/logo - one panel per epoch (-1..3),
// pre-filled from `configs[epoch_to_index(epoch)]` (cms_get_theme_logo_config()'s
// stored value for that theme/epoch - LOGO_MODE_UNSET if nothing saved yet).
// The panel's shape differs per epoch (see CmsLogoConfig's own doc comment
// in cms_themes.h for exactly which mode(s) each epoch offers): epoch 0 is
// a plain text field; epoch -1/1/2 are image-upload only (navbar + footer);
// epoch 3 is a radio choice between the two. Returns a malloc'd string, or
// NULL on a missing template / allocation failure.
char *site_settings_logo_page(int epoch, const char *key, const CmsLogoConfig configs[EPOCH_COUNT]);

// /dashboard/settings/themes/<key>/css - a full-file editor for that
// theme's epoch 3 stylesheet (styles_epoch3.css, served dynamically by
// cms_get_theme_css() - see http_router.c's match_theme_css_url()).
// `value` is the *effective* CSS to pre-fill the textarea with (the DB
// override if the theme has been customized, else the theme's own on-disk
// file), not just the stored override: unlike banner/footer/logo's small
// raw-markup snippets (fine to start empty), a full stylesheet editor needs
// to start from something editable. The page also renders a small
// "Restore original" form (posting to .../css/restore, which clears the
// override back to that on-disk file) whenever a customization is stored.
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *site_settings_css_page(int epoch, const char *key, const char *value);

// /dashboard/settings/preview - a static control panel (epoch + screen size
// pickers) driving an iframe of "/" via the ?preview_epoch=<N> override in
// http_router.c. No dynamic content, so this just loads the epoch3 template
// as-is. Returns a malloc'd string, or NULL on a missing template /
// allocation failure.
char *site_settings_preview_page(int epoch);

// One theme discovered under html/themes/ (readdir(), not a DB catalog -
// see theme-system-plan.md §5), paired with its one shared set of colors -
// applied to epoch 1/2/3 alike, per cms_themes.h; there is no per-epoch
// split here.
typedef struct {
    char key[64];
    bool active;
    CmsThemeColors colors;
} ThemeEntry;

// /dashboard/settings/themes - one panel per discovered theme: a "Set
// active" action (omitted for the active theme), a single color form that
// applies to every epoch that has a color model, and links to that
// theme's own banner/footer editors. Returns a malloc'd string, or NULL on
// a missing template / allocation failure.
char *site_settings_themes_page(int epoch, const ThemeEntry *themes, size_t count);

#endif // SITE_SETTINGS_ADMIN_H
