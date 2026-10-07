#ifndef MEDIA_ADMIN_H
#define MEDIA_ADMIN_H

#include "../../db/cms_media.h"

char *media_admin_page(int epoch, const CmsMediaDirectory *dirs, size_t dir_count,
                       const CmsMediaItem *items, size_t item_count);

char *media_admin_render_directories(const CmsMediaDirectory *dirs, size_t count, int epoch);

char *media_admin_render_items(const CmsMediaItem *items, size_t count, int epoch);

char *media_admin_render_directory_item(const CmsMediaDirectory *dir, int epoch);

char *media_admin_modal(int epoch, const CmsMediaDirectory *dirs, size_t dir_count,
                        const CmsMediaItem *items, size_t item_count);

#endif
