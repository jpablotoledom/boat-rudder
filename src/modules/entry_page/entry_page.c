#include "entry_page.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *render_tittle(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/tittle/tittle_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text);
    free(tpl);
    return result;
}

static char *render_paragraph(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/paragraph/paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text);
    free(tpl);
    return result;
}

static char *render_byline(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/byline/byline_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text, block->extra_data);
    free(tpl);
    return result;
}

static char *render_image(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/image/image_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *result = (epoch >= 1)
        ? render_template(tpl, block->text, block->extra_data, block->extra_data)
        : render_template(tpl, block->extra_data);
    free(tpl);
    return result;
}

// Renders entry->category_names as a "tags" block. Returns "" (no block) if
// the entry has no categories.
static char *render_categories(const CmsEntry *entry, int epoch) {
    if (entry->category_count == 0) return strdup("");

    char *item_tpl = load_template("elements/category/category_epoch%d.html", epoch);
    char *wrap_tpl = load_template("entry/entry-categories_epoch%d.html", epoch);
    if (!item_tpl || !wrap_tpl) {
        free(item_tpl);
        free(wrap_tpl);
        return strdup("");
    }

    char *items = strdup("");
    for (size_t i = 0; items && i < entry->category_count; i++) {
        char *item = render_template(item_tpl, entry->category_names[i]);
        items = str_append(items, item);
        free(item);
    }

    char *result = items ? render_template(wrap_tpl, items) : NULL;
    free(items);
    free(item_tpl);
    free(wrap_tpl);
    return result;
}

static char *render_gallery(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    const char *gallery_id = (block->extra_data && strlen(block->extra_data) == 24)
                              ? block->extra_data : NULL;

    if (epoch <= 0) {
        char *tpl = load_template("elements/gallery/gallery_epoch%d.html", epoch);
        if (!tpl) return strdup("");
        if (gallery_id) {
            char link[128];
            snprintf(link, sizeof(link), "<a href=\"/gallery/%s\">View gallery</a>", gallery_id);
            char *result = render_template(tpl, link);
            free(tpl);
            return result;
        }
        int count = 1;
        for (const char *p = block->text; *p; p++) if (*p == ';') count++;
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "Gallery: %d images", count);
        char *result = render_template(tpl, count_str);
        free(tpl);
        return result;
    }

    if (epoch >= 3) {
        char *container_tpl = load_template("elements/gallery/gallery-container_epoch%d.html", epoch);
        char *item_tpl = load_template("elements/gallery/gallery-item_epoch%d.html", epoch);
        char *more_tpl = load_template("elements/gallery/gallery-item-more_epoch%d.html", epoch);
        if (!container_tpl || !item_tpl) {
            free(container_tpl); free(item_tpl); free(more_tpl);
            return strdup("");
        }

        // Count total images
        int total = 0;
        for (const char *p = block->text; *p; p++) if (*p == ';') total++;
        total++;

        // Collect all URLs
        char **urls = calloc(total, sizeof(char *));
        char *copy = strdup(block->text);
        char *saveptr = NULL;
        int count = 0;
        for (char *tok = strtok_r(copy, ";", &saveptr); tok && count < total; tok = strtok_r(NULL, ";", &saveptr)) {
            while (*tok == ' ') tok++;
            if (*tok) urls[count++] = strdup(tok);
        }
        free(copy);

        int max_visible = (count <= 5) ? count : 5;
        int remaining = count - max_visible;

        char *items_html = strdup("");
        for (int i = 0; i < count && items_html; i++) {
            char *thumb = image_url_variant(urls[i], "_small");
            char *full  = image_url_variant(urls[i], "_full");

            if (i < max_visible - 1 || remaining == 0) {
                // Normal visible item
                char *item = (thumb && full) ? render_template(item_tpl, thumb, full) : NULL;
                if (item) items_html = str_append(items_html, item);
                free(item);
            } else if (i == max_visible - 1 && remaining > 0 && more_tpl) {
                // "+N" overlay item (uses the 5th image as background)
                char *item = render_template(more_tpl, thumb, full, remaining + 1);
                if (item) items_html = str_append(items_html, item);
                free(item);
            } else {
                // Hidden items — still in DOM for lightbox navigation
                char hidden[1024];
                snprintf(hidden, sizeof(hidden),
                    "<div class=\"boat-rudder__gallery-item\" style=\"display:none\">"
                    "<img class=\"boat-rudder__gallery-item__image\" src=\"%s\" data-full=\"%s\" alt=\"\">"
                    "</div>", thumb ? thumb : "", full ? full : "");
                items_html = str_append(items_html, hidden);
            }
            free(thumb);
            free(full);
        }

        for (int i = 0; i < count; i++) free(urls[i]);
        free(urls);

        char *result = items_html ? render_template(container_tpl, items_html) : NULL;
        free(items_html);
        free(container_tpl);
        free(item_tpl);
        free(more_tpl);
        return result ? result : strdup("");
    }

    // Epochs 1-2: table layout with links to /gallery/<id>?img=N
    char *wrap_tpl = load_template("elements/gallery/gallery_epoch%d.html", epoch);
    if (!wrap_tpl) return strdup("");

    char *rows_html = strdup("");
    char *copy = strdup(block->text);
    char *saveptr = NULL;
    char *tok = strtok_r(copy, ";", &saveptr);
    int col = 0;
    int img_idx = 0;

    while (tok && rows_html) {
        while (*tok == ' ') tok++;
        if (!*tok) { tok = strtok_r(NULL, ";", &saveptr); continue; }

        char *thumb = (epoch == 1) ? image_url_variant(tok, "_micro") : image_url_variant(tok, "_small");
        if (epoch == 1) {
            char *dot = thumb ? strrchr(thumb, '.') : NULL;
            if (dot) { strcpy(dot, ".gif"); }
        }

        char cell[1024];
        if (gallery_id) {
            snprintf(cell, sizeof(cell),
                     "<td align=\"center\" width=\"33%%%%\">"
                     "<a href=\"/gallery/%s?img=%d\">"
                     "<img src=\"%s\" alt=\"\" width=\"150\" border=\"0\">"
                     "</a></td>", gallery_id, img_idx, thumb ? thumb : "");
        } else {
            snprintf(cell, sizeof(cell),
                     "<td align=\"center\" width=\"33%%%%\">"
                     "<img src=\"%s\" alt=\"\" width=\"150\" border=\"0\">"
                     "</td>", thumb ? thumb : "");
        }
        free(thumb);

        if (col == 0) rows_html = str_append(rows_html, "<tr>");
        rows_html = str_append(rows_html, cell);
        col++;
        if (col >= 3) { rows_html = str_append(rows_html, "</tr>"); col = 0; }

        tok = strtok_r(NULL, ";", &saveptr);
        img_idx++;
    }
    if (col > 0) rows_html = str_append(rows_html, "</tr>");
    free(copy);

    char *result = rows_html ? render_template(wrap_tpl, rows_html) : NULL;
    free(rows_html);
    free(wrap_tpl);
    return result ? result : strdup("");
}

static char *render_block(const CmsContentBlock *block, int epoch) {
    if (strcmp(block->type, "tittle") == 0)    return render_tittle(block, epoch);
    if (strcmp(block->type, "paragraph") == 0) return render_paragraph(block, epoch);
    if (strcmp(block->type, "byline") == 0)    return render_byline(block, epoch);
    if (strcmp(block->type, "image") == 0)     return render_image(block, epoch);
    if (strcmp(block->type, "gallery") == 0)   return render_gallery(block, epoch);
    return strdup("");
}

char *entry_page_render_content(const CmsEntry *entry, int epoch) {
    char *result = strdup("");
    if (!result) return NULL;

    for (size_t i = 0; i < entry->content_count; i++) {
        char *block_html = render_block(&entry->content[i], epoch);
        if (!block_html) {
            free(result);
            return NULL;
        }

        result = str_append(result, block_html);
        free(block_html);
        if (!result) return NULL;
    }

    return result;
}

char *entry_page(const CmsEntry *entry, int epoch) {
    char *categories_html = render_categories(entry, epoch);
    if (!categories_html) return NULL;

    char *content_html = entry_page_render_content(entry, epoch);
    if (!content_html) {
        free(categories_html);
        return NULL;
    }

    char *result = str_append(categories_html, content_html);
    free(content_html);
    return result;
}
