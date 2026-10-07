#ifndef HOME_CONTENT_H
#define HOME_CONTENT_H

// Builds the home page's welcome text plus a static "updates" list for
// `epoch` and `lang`. `lang` is reserved for future translations.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *home_content(int epoch, const char *lang);

#endif // HOME_CONTENT_H
