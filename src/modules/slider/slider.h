#ifndef SLIDER_H
#define SLIDER_H

// Loads the static banner/slider block for `epoch`. The MVP slider has no
// dynamic placeholders.
//
// Returns a malloc'd string, or NULL on failure (missing template or
// allocation failure). The caller must free() the returned buffer.
char *slider(int epoch);

#endif // SLIDER_H
