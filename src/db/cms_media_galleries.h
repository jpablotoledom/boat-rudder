#ifndef CMS_MEDIA_GALLERIES_H
#define CMS_MEDIA_GALLERIES_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char   id[25];
    char **urls;
    size_t url_count;
    char   entry_id[25];
} CmsMediaGallery;

// Creates or updates a media_galleries document. If gallery_id is NULL or
// empty, inserts a new document; otherwise updates the existing one.
// urls_csv is a semicolon-separated list of image URLs.
// out_id receives the 24-char hex _id. Returns 0 on success.
int cms_upsert_media_gallery(const char *gallery_id, const char *entry_id,
                              const char *urls_csv, char *out_id);

// Reads one gallery document by _id. Returns true if found.
bool cms_get_media_gallery(const char *id_hex, CmsMediaGallery *out);

void cms_media_gallery_free(CmsMediaGallery *g);

// Resolves the public URL and title of the entry a gallery belongs to, for
// the gallery page's "< Back to <title>" link. `entry_id_hex` is
// CmsMediaGallery.entry_id. `out_url` becomes "/blog/<link>" or
// "/page/<link>" depending on the entry's type; `out_title` is the title in
// `lang`, falling back to "" if the entry cannot be found.
// Returns true if the entry was found.
bool cms_get_entry_backlink(const char *entry_id_hex, const char *lang,
                             char *out_url, size_t url_size,
                             char *out_title, size_t title_size);

#endif
