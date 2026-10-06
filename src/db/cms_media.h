#ifndef CMS_MEDIA_H
#define CMS_MEDIA_H

#include <stdbool.h>
#include <stddef.h>

#define MEDIA_LIST_LIMIT 200
#define MEDIA_PAGE_SIZE   36

typedef struct {
    char id[25];
    char name[256];
    char date[20];
    char format[8];
    char author_id[25];
    char author_username[64];
    char dir_id[25];
    char dir_name[256];
    char dir_parent[20];
} CmsMediaItem;

typedef struct {
    char id[25];
    char name[256];
    char parent[20];
    char author_id[25];
    char author_name[64];
} CmsMediaDirectory;

// Directories ---

void cms_get_media_directories(CmsMediaDirectory **out, size_t *out_count);
void cms_media_directories_free(CmsMediaDirectory *items, size_t count);

bool cms_get_media_directory_by_id(const char *id_hex, CmsMediaDirectory *out);

// Looks up a directory by its exact name, scoped to one author - used to
// find (or decide whether to create) that author's "default" directory.
bool cms_get_media_directory_by_name(const char *name, const char *author_id,
                                     CmsMediaDirectory *out);

int cms_create_media_directory(const char *name, const char *parent,
                               const char *author_id, char *out_id);

int cms_rename_media_directory(const char *id_hex, const char *new_name);

int cms_delete_media_directory(const char *id_hex);

// Media items ---

void cms_get_media_items(const char *dir_id, int skip, int limit,
                         CmsMediaItem **out, size_t *out_count);
void cms_media_items_free(CmsMediaItem *items, size_t count);

// Same shape as one row of cms_get_media_items(), for a single _id - the
// author username and directory name/parent a caller needs to find the
// item's files on disk before deleting or moving it.
bool cms_get_media_item_by_id(const char *id_hex, CmsMediaItem *out);

int cms_insert_media(const char *filename, const char *author_id,
                     const char *dir_id, char *out_id);

int cms_delete_media(const char *id_hex);

// Reassigns the item's dir_id. The caller is responsible for moving the
// files on disk to match - this only updates the database record.
int cms_move_media(const char *id_hex, const char *dest_dir_id);

#endif
