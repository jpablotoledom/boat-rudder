#ifndef TEMPLATE_UTILS_H
#define TEMPLATE_UTILS_H

// Returns a new malloc'd string with the first occurrence of `needle` in
// `src` replaced by `replacement`. If `needle` does not occur in `src`, a
// plain copy of `src` is returned. Returns NULL on allocation failure.
char *str_replace_first(const char *src, const char *needle, const char *replacement);

// Renders a printf-style template into a new malloc'd, NUL-terminated
// string. Returns NULL on allocation/formatting failure.
char *render_template(const char *tpl, ...);

// Appends `src` to the malloc'd string `dst` (NULL is treated as an empty
// string), freeing `dst`. Returns the new buffer, or NULL on allocation
// failure (in which case `dst` has already been freed).
char *str_append(char *dst, const char *src);

#endif // TEMPLATE_UTILS_H
