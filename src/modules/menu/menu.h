#ifndef MENU_H
#define MENU_H

// Builds the navigation menu for `epoch`. `current_url` is reserved for
// future "selected item" highlighting.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *menu(const char *current_url, int epoch);

// The same navigation bar - logo, language and theme selectors, user link -
// with no menu items (an empty #navbarMenu on epoch 3): the under-construction
// page's (see buildUnderConstructionWebSite()). It lays out like home's, so
// the logo stays out on every path, as it does on "/", and WML shows this bar
// in full rather than its compact "[Menu]" link (/menu is closed then too).
char *menu_without_items(int epoch);

#endif // MENU_H
