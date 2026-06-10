#ifndef GENERATE_URL_THEME_H
#define GENERATE_URL_THEME_H

// Builds the on-disk path "./html/themes/<theme>/<subpath>", where <theme>
// comes from the global `theme` config value and <subpath> is `subpath_fmt`
// with its single "%d" replaced by `epoch`.
//
// Example: generate_url_theme("menu/menu_epoch%d.html", -1)
//       -> "./html/themes/dark/menu/menu_epoch-1.html"
//
// Returns a malloc'd string, or NULL on allocation/formatting failure.
char *generate_url_theme(const char *subpath_fmt, int epoch);

#endif // GENERATE_URL_THEME_H
