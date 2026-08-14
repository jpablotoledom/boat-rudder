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
        char *item = (epoch >= 3)
            ? render_template(item_tpl, entry->category_links[i], entry->category_names[i])
            : render_template(item_tpl, entry->category_names[i]);
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
        if (gallery_id) {
            char *tpl = load_template("elements/gallery/gallery-link_epoch%d.html", epoch);
            if (!tpl) return strdup("");
            char *result = render_template(tpl, gallery_id);
            free(tpl);
            return result ? result : strdup("");
        }
        int count = 1;
        for (const char *p = block->text; *p; p++) if (*p == ';') count++;
        char *tpl = load_template("elements/gallery/gallery-nolink_epoch%d.html", epoch);
        if (!tpl) return strdup("");
        char *result = render_template(tpl, count);
        free(tpl);
        return result ? result : strdup("");
    }

    if (epoch >= 3) {
        char *container_tpl = load_template("elements/gallery/gallery-container_epoch%d.html", epoch);
        char *item_tpl      = load_template("elements/gallery/gallery-item_epoch%d.html", epoch);
        char *more_tpl      = load_template("elements/gallery/gallery-item-more_epoch%d.html", epoch);
        char *hidden_tpl    = load_template("elements/gallery/gallery-item-hidden_epoch%d.html", epoch);
        if (!container_tpl || !item_tpl) {
            free(container_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
            return strdup("");
        }

        int total = 0;
        for (const char *p = block->text; *p; p++) if (*p == ';') total++;
        total++;

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
                char *item = (thumb && full) ? render_template(item_tpl, thumb, full) : NULL;
                if (item) items_html = str_append(items_html, item);
                free(item);
            } else if (i == max_visible - 1 && remaining > 0 && more_tpl) {
                char *item = render_template(more_tpl, thumb, full, remaining + 1);
                if (item) items_html = str_append(items_html, item);
                free(item);
            } else if (hidden_tpl) {
                char *item = render_template(hidden_tpl, thumb ? thumb : "", full ? full : "");
                if (item) items_html = str_append(items_html, item);
                free(item);
            }
            free(thumb);
            free(full);
        }

        for (int i = 0; i < count; i++) free(urls[i]);
        free(urls);

        char *result = items_html ? render_template(container_tpl, items_html) : NULL;
        free(items_html);
        free(container_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
        return result ? result : strdup("");
    }

    // Epochs 1-2: table layout with links to /gallery/<id>?img=N
    char *wrap_tpl      = load_template("elements/gallery/gallery_epoch%d.html", epoch);
    char *row_start_tpl = load_template("elements/gallery/gallery-row-start_epoch%d.html", epoch);
    char *row_end_tpl   = load_template("elements/gallery/gallery-row-end_epoch%d.html", epoch);
    char *cell_tpl      = gallery_id
        ? load_template("elements/gallery/gallery-cell_epoch%d.html", epoch)
        : load_template("elements/gallery/gallery-cell-nolink_epoch%d.html", epoch);
    if (!wrap_tpl || !row_start_tpl || !row_end_tpl || !cell_tpl) {
        free(wrap_tpl); free(row_start_tpl); free(row_end_tpl); free(cell_tpl);
        return strdup("");
    }

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
        if (epoch == 1 && thumb) {
            char *dot = strrchr(thumb, '.'); if (dot) strcpy(dot, ".gif");
        }

        char *cell = gallery_id
            ? render_template(cell_tpl, gallery_id, img_idx, thumb ? thumb : "")
            : render_template(cell_tpl, thumb ? thumb : "");
        free(thumb);

        if (col == 0) {
            char *rs = render_template(row_start_tpl);
            rows_html = str_append(rows_html, rs);
            free(rs);
        }
        if (cell) { rows_html = str_append(rows_html, cell); free(cell); }
        col++;
        if (col >= 3) {
            char *re = render_template(row_end_tpl);
            rows_html = str_append(rows_html, re);
            free(re);
            col = 0;
        }

        tok = strtok_r(NULL, ";", &saveptr);
        img_idx++;
    }
    if (col > 0) {
        char *re = render_template(row_end_tpl);
        rows_html = str_append(rows_html, re);
        free(re);
    }
    free(copy);
    free(row_start_tpl); free(row_end_tpl); free(cell_tpl);

    char *result = rows_html ? render_template(wrap_tpl, rows_html) : NULL;
    free(rows_html);
    free(wrap_tpl);
    return result ? result : strdup("");
}

static char *render_separator(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/separator/separator_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->extra_data ? block->extra_data : "");
    free(tpl);
    return result;
}

static char *render_link(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/link/link_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl,
        block->extra_data ? block->extra_data : "#",
        block->text       ? block->text       : "");
    free(tpl);
    return result;
}

static char *render_list(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    int ordered = (block->extra_data && strcmp(block->extra_data, "ol") == 0);
    const char *container_fmt = ordered
        ? "elements/list/list-container-ol_epoch%d.html"
        : "elements/list/list-container_epoch%d.html";

    char *container_tpl = load_template(container_fmt, epoch);
    char *item_tpl      = load_template("elements/list/list-item_epoch%d.html", epoch);
    if (!container_tpl || !item_tpl) {
        free(container_tpl); free(item_tpl);
        return strdup("");
    }

    char *items = strdup("");
    char *copy  = strdup(block->text);
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, "\n", &saveptr); tok; tok = strtok_r(NULL, "\n", &saveptr)) {
        while (*tok == '\r') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end >= tok && (*end == '\r' || *end == '\n')) *end-- = '\0';
        if (!*tok) continue;
        char *item = render_template(item_tpl, tok);
        items = item ? str_append(items, item) : NULL;
        free(item);
    }
    free(copy);

    char *result = items ? render_template(container_tpl, items) : NULL;
    free(items);
    free(container_tpl);
    free(item_tpl);
    return result ? result : strdup("");
}

static char *youtube_to_embed(const char *url) {
    if (!url || !url[0]) return NULL;
    if (strstr(url, "youtube.com/embed/")) return strdup(url);
    const char *v = strstr(url, "v=");
    if (v) {
        v += 2;
        char id[12] = {0};
        for (int i = 0; i < 11 && v[i] && v[i] != '&'; i++) id[i] = v[i];
        if (id[0]) {
            char *e = malloc(64);
            if (e) snprintf(e, 64, "https://www.youtube.com/embed/%s", id);
            return e;
        }
    }
    const char *ytbe = strstr(url, "youtu.be/");
    if (ytbe) {
        ytbe += 9;
        char id[12] = {0};
        for (int i = 0; i < 11 && ytbe[i] && ytbe[i] != '?' && ytbe[i] != '&'; i++) id[i] = ytbe[i];
        if (id[0]) {
            char *e = malloc(64);
            if (e) snprintf(e, 64, "https://www.youtube.com/embed/%s", id);
            return e;
        }
    }
    return strdup(url);
}

static char *render_youtube_embed(const CmsContentBlock *block, int epoch) {
    if (!block->extra_data || !block->extra_data[0]) return strdup("");
    char *tpl = load_template("elements/youtube-embed/youtube-embed_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *embed = youtube_to_embed(block->extra_data);
    char *result = embed ? render_template(tpl, embed) : NULL;
    free(embed);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_code_text(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/code-text/code-text_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl,
        block->extra_data ? block->extra_data : "",
        block->text);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_generic(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/generic/generic_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text ? block->text : "");
    free(tpl);
    return result ? result : strdup("");
}

static char *render_image_paragraph(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/image-paragraph/image-paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char align_attr[32] = "";
    const char *a = block->extra_data;
    if (a && (strcmp(a, "left") == 0 || strcmp(a, "right") == 0))
        snprintf(align_attr, sizeof(align_attr), "align=\"%s\"", a);
    char *result = render_template(tpl, block->text, align_attr);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_table(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    char *table_tpl  = load_template("elements/table/table_epoch%d.html",             epoch);
    char *row_tpl    = load_template("elements/table/table-row_epoch%d.html",          epoch);
    char *cell_tpl   = load_template("elements/table/table-cell_epoch%d.html",         epoch);
    char *header_tpl = load_template("elements/table/table-header-cell_epoch%d.html",  epoch);
    if (!table_tpl || !row_tpl || !cell_tpl || !header_tpl) {
        free(table_tpl); free(row_tpl); free(cell_tpl); free(header_tpl);
        return strdup("");
    }

    int has_header = (block->extra_data && strcmp(block->extra_data, "header") == 0);
    char *rows_html = strdup("");
    char *copy = strdup(block->text);
    char *row_sp = NULL;
    int row_idx = 0;

    for (char *row_tok = strtok_r(copy, "\n", &row_sp);
         row_tok && rows_html;
         row_tok = strtok_r(NULL, "\n", &row_sp), row_idx++) {
        while (*row_tok == '\r') row_tok++;
        char *end = row_tok + strlen(row_tok) - 1;
        while (end >= row_tok && (*end == '\r' || *end == '\n')) *end-- = '\0';
        if (!*row_tok) continue;

        char *cell_tpl_cur = (has_header && row_idx == 0) ? header_tpl : cell_tpl;
        char *cells = strdup("");
        char *row_copy = strdup(row_tok);
        char *cell_sp  = NULL;
        for (char *ct = strtok_r(row_copy, "|", &cell_sp); ct && cells; ct = strtok_r(NULL, "|", &cell_sp)) {
            char *cell = render_template(cell_tpl_cur, ct);
            cells = cell ? str_append(cells, cell) : NULL;
            free(cell);
        }
        free(row_copy);
        char *row = cells ? render_template(row_tpl, cells) : NULL;
        rows_html = row ? str_append(rows_html, row) : NULL;
        free(cells); free(row);
    }
    free(copy);

    char *result = rows_html ? render_template(table_tpl, rows_html) : NULL;
    free(rows_html);
    free(table_tpl); free(row_tpl); free(cell_tpl); free(header_tpl);
    return result ? result : strdup("");
}

static char *render_social_networks(const CmsContentBlock *block, int epoch) {
    if (!block->extra_data || !block->extra_data[0]) return strdup("");
    char *pipe = strchr(block->extra_data, '|');
    if (!pipe) return strdup("");

    char *tpl = load_template("elements/social-networks/social-networks_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char icon[64] = {0};
    size_t icon_len = (size_t)(pipe - block->extra_data);
    if (icon_len >= sizeof(icon)) icon_len = sizeof(icon) - 1;
    strncpy(icon, block->extra_data, icon_len);
    const char *url = pipe + 1;

    char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "/themes/dark/assets/social-networks/%s.svg", icon);

    const char *name = (block->text && block->text[0]) ? block->text : icon;
    char *result = render_template(tpl, url, icon_path, name, name);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_block(const CmsContentBlock *block, int epoch) {
    if (strcmp(block->type, "tittle") == 0)          return render_tittle(block, epoch);
    if (strcmp(block->type, "paragraph") == 0)       return render_paragraph(block, epoch);
    if (strcmp(block->type, "byline") == 0)          return render_byline(block, epoch);
    if (strcmp(block->type, "image") == 0)           return render_image(block, epoch);
    if (strcmp(block->type, "gallery") == 0)         return render_gallery(block, epoch);
    if (strcmp(block->type, "separator") == 0)       return render_separator(block, epoch);
    if (strcmp(block->type, "link") == 0)            return render_link(block, epoch);
    if (strcmp(block->type, "list") == 0)            return render_list(block, epoch);
    if (strcmp(block->type, "youtube-embed") == 0)   return render_youtube_embed(block, epoch);
    if (strcmp(block->type, "code-text") == 0)       return render_code_text(block, epoch);
    if (strcmp(block->type, "generic") == 0)         return render_generic(block, epoch);
    if (strcmp(block->type, "image-paragraph") == 0) return render_image_paragraph(block, epoch);
    if (strcmp(block->type, "table") == 0)           return render_table(block, epoch);
    if (strcmp(block->type, "social-networks") == 0) return render_social_networks(block, epoch);
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

    char *meta_html;
    if (epoch >= 3) {
        char *meta_tpl = load_template("entry/entry-meta_epoch%d.html", epoch);
        const char *author = entry->header_author ? entry->header_author : "";
        meta_html = meta_tpl ? render_template(meta_tpl, author, categories_html) : categories_html;
        free(meta_tpl);
        if (meta_tpl) free(categories_html);
    } else {
        meta_html = categories_html;
    }

    if (!meta_html) return NULL;

    char *content_html = entry_page_render_content(entry, epoch);
    if (!content_html) {
        free(meta_html);
        return NULL;
    }

    char *result = str_append(meta_html, content_html);
    free(content_html);
    return result;
}
