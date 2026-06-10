#ifndef CONTAINER_H
#define CONTAINER_H

// Loads the page container template for `epoch` and resolves
// {{PAGE_TITLE}}. The returned string still contains 3 positional %s
// placeholders, in order: menu, slider, home_content.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *container(int epoch);

#endif // CONTAINER_H
