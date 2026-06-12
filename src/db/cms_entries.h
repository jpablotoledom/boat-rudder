#ifndef CMS_ENTRIES_H
#define CMS_ENTRIES_H

#include <stddef.h>

// Maximum number of "entries" documents (type == "blog", enabled == true) returned by
// cms_get_blog_entries() for the home blog list, newest header.date first.
#define HOME_BLOG_LIMIT 10

// Maximum number of "entries" documents (type == "blog", enabled == true) returned by
// cms_get_blog_entries() for the "/blogs" listing page, newest header.date first.
#define BLOG_LIST_LIMIT 50

// Maximum number of "entries" documents (any type, enabled == true) returned by
// cms_get_admin_entries() for the /dashboard/entries listing, newest header.date first.
#define ENTRIES_LIST_LIMIT 100

// One ordered, typed content block from `entries.content[]` (see
// develop_docs/plans/cms-entry-model-plan.md). `text` is the block's
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

// One "entries" document, for blog/admin list views. Lighter than CmsEntry:
// no content[] (not needed for a list view).
typedef struct {
    char *link; // entries.link, for building /page/<link> or /blog/<link>
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
} CmsBlogListItem;

// Looks up db.entries.find({type: "blog", enabled: true})
//   .sort({"header.date": -1}).limit(limit),
// resolving header.* and categories[] to `lang` for each match (same lang convention as
// cms_get_entry_by_link). `limit` is HOME_BLOG_LIMIT for the home blog list or
// BLOG_LIST_LIMIT for the "/blogs" listing page. On success, *out points to a malloc'd
// array of *out_count items (possibly 0) that must be passed to cms_blog_list_free(). On a
// DB error or if mongodb is not ready, *out = NULL and *out_count = 0 - both callers are
// decorative and must never fail the page.
void cms_get_blog_entries(const char *lang, size_t limit, CmsBlogListItem **out, size_t *out_count);

// Looks up db.entries.find({enabled: true}).sort({"header.date": -1}).limit(ENTRIES_LIST_LIMIT),
// resolving header.*, categories[] and type for each match (same lang convention as
// cms_get_entry_by_link), for the /dashboard/entries listing. On success, *out points to a
// malloc'd array of *out_count items (possibly 0) that must be passed to
// cms_blog_list_free(). On a DB error or if mongodb is not ready, *out = NULL and
// *out_count = 0.
void cms_get_admin_entries(const char *lang, CmsBlogListItem **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with items == NULL.
void cms_blog_list_free(CmsBlogListItem *items, size_t count);

#endif // CMS_ENTRIES_H
