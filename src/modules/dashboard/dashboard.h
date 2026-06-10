#ifndef DASHBOARD_H
#define DASHBOARD_H

// Loads the static "Welcome to dashboard" content fragment for `epoch`. The
// MVP dashboard has no dynamic placeholders.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *dashboard(int epoch);

#endif // DASHBOARD_H
