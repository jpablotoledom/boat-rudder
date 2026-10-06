#ifndef SHORT_LINKS_H
#define SHORT_LINKS_H

// Short codes for QR codes: a dense QR (a long gallery URL, easily 60+
// characters once the domain is included) forces a bigger module grid,
// which is what was making the Cello-rendered QR overflow its own <nobr>
// row width and cutting off the corners a reader actually needs to lock
// onto (see qr_generator.h). Routing QR codes through "/qr/<code>" instead
// of the page's own long URL keeps every QR - image or text-block alike -
// as small as the code itself, regardless of how long the real path is.

// Looks up an existing short code for `target_path` (e.g. "/gallery/<id>"),
// or mints and stores a new one if none exists yet - repeat calls for the
// same target_path are idempotent and return the same code. Returns a
// malloc'd short code (caller must free), or NULL if mongodb is not ready
// or the write failed.
char *short_link_get_or_create(const char *target_path);

// Resolves a short code back to its target_path. Returns a malloc'd string
// (caller must free), or NULL if the code doesn't exist or mongodb is not
// ready.
char *short_link_resolve(const char *code);

#endif // SHORT_LINKS_H
