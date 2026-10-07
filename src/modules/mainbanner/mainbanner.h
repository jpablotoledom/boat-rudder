#ifndef MAINBANNER_H
#define MAINBANNER_H

// Loads the home page's mainbanner block for `epoch` (a banner, not a
// slider - there is no rotation/carousel), personalizable per epoch from
// /dashboard/settings/banner.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *mainbanner(int epoch);

#endif // MAINBANNER_H
