#ifndef GENERATE_URL_THEME_H
#define GENERATE_URL_THEME_H

// Builds the on-disk path for <subpath> (`subpath_fmt` with its single "%d"
// replaced by `epoch`), checking the active theme (request_theme()) first
// and falling back to the shared, theme-agnostic template tree if the theme
// does not have its own copy of that file - see
// develop_docs/plans/theme-system-plan.md §3. Callers never need to know
// which of the two they got back.
//
// Examples (theme "dark", which overrides menu/ but not error/):
//   generate_url_theme("menu/menu_epoch%d.html", -1)
//       -> "./html/themes/dark/menu/menu_epoch-1.html"   (theme override)
//   generate_url_theme("error/error_epoch%d.html", -1)
//       -> "./html/templates/error/error_epoch-1.html"   (shared fallback)
//
// Returns a malloc'd string, or NULL on allocation/formatting failure.
char *generate_url_theme(const char *subpath_fmt, int epoch);

#endif // GENERATE_URL_THEME_H
