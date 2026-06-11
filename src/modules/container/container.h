#ifndef CONTAINER_H
#define CONTAINER_H

// Loads the page container template for `epoch` and resolves
// {{PAGE_TITLE}} to "<title>page_title</title>". The returned string still
// contains positional %s placeholders for menu, slider, home_content and
// home_blog.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *container(int epoch, const char *page_title);

#endif // CONTAINER_H
