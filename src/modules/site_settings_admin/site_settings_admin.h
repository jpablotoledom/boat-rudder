#ifndef SITE_SETTINGS_ADMIN_H
#define SITE_SETTINGS_ADMIN_H

#include "../../db/cms_site_settings.h"

// /dashboard/settings - the site name form, plus links to the banner/footer
// editors. `error_message` is shown above the form if non-NULL/non-empty.
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *site_settings_general_page(int epoch, const char *site_name, const char *error_message);

// /dashboard/settings/banner and /dashboard/settings/footer - one
// <textarea> + asset upload/browse widget per epoch (-1..3), pre-filled
// from `values[cms_site_settings_epoch_index(epoch)]` (the *stored* DB
// value, "" if unset - not the file-fallback-resolved value, so the admin
// can tell "nothing saved" apart from "saved text matching the file").
// Returns a malloc'd string, or NULL on a missing template / allocation
// failure.
char *site_settings_banner_page(int epoch, char *const values[SITE_SETTINGS_EPOCH_COUNT]);
char *site_settings_footer_page(int epoch, char *const values[SITE_SETTINGS_EPOCH_COUNT]);

// /dashboard/settings/preview - a static control panel (epoch + screen size
// pickers) driving an iframe of "/" via the ?preview_epoch=<N> override in
// http_router.c. No dynamic content, so this just loads the epoch3 template
// as-is. Returns a malloc'd string, or NULL on a missing template /
// allocation failure.
char *site_settings_preview_page(int epoch);

#endif // SITE_SETTINGS_ADMIN_H
