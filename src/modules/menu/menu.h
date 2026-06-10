#ifndef MENU_H
#define MENU_H

// Builds the navigation menu for `epoch`. `current_url` is reserved for
// future "selected item" highlighting.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *menu(const char *current_url, int epoch);

#endif // MENU_H
