#ifndef CMS_ENTRIES_ADMIN_H
#define CMS_ENTRIES_ADMIN_H

#include "cms_languages.h"
#include <stdbool.h>
#include <stddef.h>

// One `entries.content[]` block, all languages, for the editor (no lang
// resolution - see cms_entries.h's CmsContentBlock for the resolved, public
// equivalent).
typedef struct {
    char  *id;          // content[]._id as 24-char hex
    char  *type;        // "tittle" | "paragraph" | "image" | "byline"
    int    order;
    char **text_values; // parallel to langs[] passed to cms_get_entry_for_edit() /
                         // cms_update_entry_content() - exact value of text.<lang>,
                         // "" if that language's key is absent
    char  *extra_data;  // untranslated
} CmsContentBlockEdit;

// One `entries` document, all languages, for /dashboard/entries/<id>/edit.
typedef struct {
    char   *id;
    char   *link;
    char   *type;   // "page" | "blog"
    bool    enabled;
    char   *created_by;  // entries.created_by as 24-char hex, "" if absent

    char  **category_ids;  // entries.categories[] as 24-char hex strings
    size_t  category_count;

    char   *header_image_url;
    char  **header_title_values;   // parallel to langs[], exact header.title.<lang>
    char  **header_summary_values; // parallel to langs[], exact header.summary.<lang>
    char   *header_author_name;    // display name from users collection (via header.author_id)
    char    header_date[16];       // "YYYY-MM-DD", "" if absent

    CmsContentBlockEdit *content; // sorted by order
    size_t content_count;
} CmsEntryEdit;

// db.entries.findOne({_id: id_hex}) - no `enabled` filter (admins must edit
// disabled entries too), no lang resolution: every map<lang,string> field is
// read exactly for each of langs[] (no "en" fallback). Returns 1 and fills
// *out on success, 0 if id_hex is invalid/not found, mongodb is not ready, or
// on a DB error. *out is zero-initialized on failure and must still be passed
// to cms_entry_edit_free() in either case.
int cms_get_entry_for_edit(const char *id_hex, const CmsLanguageItem *langs,
                            size_t lang_count, CmsEntryEdit *out);

// Frees every malloc'd field of *entry (including the per-language arrays,
// which have lang_count entries) and zeroes it.
void cms_entry_edit_free(CmsEntryEdit *entry, size_t lang_count);

// db.entries.insertOne({link:"", type: type, enabled:false, categories:[],
// header:{}, content:[][, created_by: ObjectId(created_by_id_hex)]}).
// `created_by` is only set if created_by_id_hex is a valid ObjectId hex
// string. Returns a malloc'd 24-char hex id of the new document, or NULL on
// a DB error or if mongodb is not ready.
char *cms_create_entry(const char *created_by_id_hex, const char *type);

// db.entries.updateOne({_id: id_hex},
//   {$set: {link, type, enabled, categories: [<category_ids as ObjectId>...]}}).
// Entries of category_ids that are not valid ObjectIds are skipped. Returns 0
// on success, -1 if id_hex is invalid, on a DB error, or if mongodb is not ready.
int cms_update_entry_meta(const char *id_hex, const char *link, const char *type,
                           bool enabled, char *const *category_ids, size_t category_count);

// db.entries.updateOne({_id: id_hex}, {$set: {
//   "header.image_url": image_url,
//   "header.title.<lang>": title_values[i], "header.summary.<lang>": summary_values[i]
//   for each langs[i], "header.date": <date as BSON UTC datetime> (only if date != "")
// }}). `date` is "YYYY-MM-DD" or "" (leaves header.date untouched). Returns 0 on
// success, -1 if id_hex is invalid, on a DB error, or if mongodb is not ready.
int cms_update_entry_header(const char *id_hex, const char *image_url, const char *date,
                             const CmsLanguageItem *langs, size_t lang_count,
                             char *const *title_values, char *const *summary_values);

// db.entries.updateOne({_id: id_hex}, {$set: {content: [...]}}) - full array
// replace, sourced from blocks[0..block_count). Every blocks[i].id must already
// be a valid 24-char hex id (new blocks are created individually via
// cms_add_entry_content_block() before this is ever called); blocks with an
// invalid id are skipped. Returns 0 on success, -1 if id_hex is invalid, on a
// DB error, or if mongodb is not ready.
int cms_update_entry_content(const char *id_hex, const CmsLanguageItem *langs,
                              size_t lang_count, const CmsContentBlockEdit *blocks,
                              size_t block_count);

// db.entries.updateOne({_id: id_hex},
//   {$push: {content: {_id: <new ObjectId>, type, order, text: {}, extra_data: ""}}}).
// Writes the new block's 24-char hex id to out_block_id (out_block_id_size must
// be >= 25). Returns 0 on success, -1 if id_hex is invalid, out_block_id_size is
// too small, on a DB error, or if mongodb is not ready.
int cms_add_entry_content_block(const char *id_hex, const char *type, int order,
                                 char *out_block_id, size_t out_block_id_size);

// db.entries.updateOne({_id: id_hex}, {$pull: {content: {_id: block_id}}}).
// Returns 0 on success, -1 if id_hex/block_id is invalid, on a DB error, or if
// mongodb is not ready.
int cms_remove_entry_content_block(const char *id_hex, const char *block_id);

// db.entries.deleteOne({_id: id_hex}). Returns 0 on success, -1 if id_hex is
// invalid, on a DB error, or if mongodb is not ready.
int cms_delete_entry(const char *id_hex);

#endif // CMS_ENTRIES_ADMIN_H
