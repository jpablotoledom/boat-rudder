#ifndef CMS_ENTRIES_H
#define CMS_ENTRIES_H

#include <stddef.h>

// One ordered, typed content block from `entries.content[]` (see
// develop_docs/cms-entry-model-plan.md). `text` is the block's
// `map<lang,string>` resolved to the requested language (falling back to
// "en", then ""). `extra_data` is untranslated, type-specific configuration.
typedef struct {
    char *type;
    int   order;
    char *text;
    char *extra_data;
} CmsContentBlock;

// One self-contained `entries` document, with `header` and `content[]`
// already resolved to the requested language.
typedef struct {
    char *link;
    char *type; // "page" | "blog" | ...

    char *header_image_url;
    char *header_title;
    char *header_summary;
    char *header_author;
    char  header_date[16]; // "YYYY-MM-DD", empty if absent

    // entries.categories[] (ObjectId[]) resolved to entry_categories.name,
    // for the requested lang. NULL/0 if the entry has no categories.
    char  **category_names;
    size_t  category_count;

    CmsContentBlock *content;
    size_t content_count; // sorted by content[].order
} CmsEntry;

// Looks up db.entries.findOne({link, enabled: true}) and fills *out with the
// document's header and content blocks, with all map<lang,string> fields
// resolved to `lang` (Boat Rudder's configs/settings.conf "Eng"/"Esp"
// convention, mapped internally to ISO 639-1 "en"/"es", falling back to
// "en"). Returns 1 and fills *out on success, 0 if not found, mongodb is not
// ready, or on a DB error. *out is zero-initialized on failure and must be
// passed to cms_entry_free() in either case.
int cms_get_entry_by_link(const char *link, const char *lang, CmsEntry *out);

// Frees every malloc'd field of *entry and zeroes it.
void cms_entry_free(CmsEntry *entry);

#endif // CMS_ENTRIES_H
