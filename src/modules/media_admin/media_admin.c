#include "media_admin.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

char *media_admin_render_directory_item(const CmsMediaDirectory *dir, int epoch) {
    char *tpl = load_template("dashboard/media/media-directory_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // 12 %s: id, id, id, name, id, name, id, name, id, parent, author_name, author_id
    char *result = render_template(tpl,
        dir->id, dir->id, dir->id, dir->name,
        dir->id, dir->name, dir->id, dir->name,
        dir->id, dir->parent, dir->author_name, dir->author_id);

    free(tpl);
    return result;
}

char *media_admin_render_directories(const CmsMediaDirectory *dirs, size_t count, int epoch) {
    char *container_tpl = load_template("dashboard/media/media-directory-container_epoch%d.html", epoch);
    if (!container_tpl) return NULL;

    char *items_html = strdup("");
    for (size_t i = 0; items_html && i < count; i++) {
        char *item = media_admin_render_directory_item(&dirs[i], epoch);
        items_html = item ? str_append(items_html, item) : NULL;
        free(item);
    }

    char *result = items_html ? render_template(container_tpl, items_html) : NULL;
    free(items_html);
    free(container_tpl);
    return result;
}

char *media_admin_render_items(const CmsMediaItem *items, size_t count, int epoch) {
    char *item_tpl = load_template("dashboard/media/item-photo_epoch%d.html", epoch);
    if (!item_tpl) return NULL;

    char *html = strdup("");
    for (size_t i = 0; html && i < count; i++) {
        const CmsMediaItem *m = &items[i];

        char base[256], ext[16] = "";
        strncpy(base, m->name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *dot = strrchr(base, '.');
        if (dot) {
            strncpy(ext, dot, sizeof(ext) - 1);
            *dot = '\0';
        }

        char thumb_path[1024];
        snprintf(thumb_path, sizeof(thumb_path), "posts/%s/%s/%s_small%s",
                 m->author_username, m->dir_name, base, ext);

        char content_path[1024];
        snprintf(content_path, sizeof(content_path), "posts/%s/%s/%s%s",
                 m->author_username, m->dir_name, base, ext);

        // 8 %s: id, thumb_path, name, id, id, id, id, content_path
        char *item = render_template(item_tpl,
            m->id, thumb_path, m->name,
            m->id, m->id, m->id, m->id, content_path);
        html = item ? str_append(html, item) : NULL;
        free(item);
    }

    free(item_tpl);
    return html;
}

char *media_admin_page(int epoch, const CmsMediaDirectory *dirs, size_t dir_count,
                       const CmsMediaItem *items, size_t item_count) {
    char *tpl = load_template("dashboard/media/media_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *dirs_html = media_admin_render_directories(dirs, dir_count, epoch);
    char *items_html = media_admin_render_items(items, item_count, epoch);

    char *result = render_template(tpl,
        dirs_html ? dirs_html : "",
        items_html ? items_html : "");

    free(dirs_html);
    free(items_html);
    free(tpl);
    return result;
}

char *media_admin_modal(int epoch, const CmsMediaDirectory *dirs, size_t dir_count,
                        const CmsMediaItem *items, size_t item_count) {
    char *modal_tpl = load_template("dashboard/media/media-modal_epoch%d.html", epoch);
    if (!modal_tpl) return NULL;

    char *page = media_admin_page(epoch, dirs, dir_count, items, item_count);
    char *result = page ? render_template(modal_tpl, page) : NULL;

    free(page);
    free(modal_tpl);
    return result;
}
