#include "entry_page.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/qr_generator/qr_generator.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Where generated QR assets are written. Matches generate_url_theme(), which
// likewise resolves templates against "./html" from the process working dir.
#define HTML_ROOT_DIR "./html"

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// On presentational blocks extra_data names a style variant, never raw markup.
// The previous CMS stored whole attributes there (style="font-size: 18px") and
// interpolated them unescaped, so anyone with editor access could inject
// arbitrary HTML into a page. Here only [a-z0-9-] passes and the renderer
// builds the class itself; anything else yields no modifier at all.
//
// Writes a leading-space class suffix ("" when there is no valid modifier) so
// templates can append it directly to their base class.
static void modifier_class(const char *extra_data, const char *base,
                           char *out, size_t out_size) {
    out[0] = '\0';
    if (!extra_data || !extra_data[0]) return;

    for (const char *p = extra_data; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) return;
    }
    snprintf(out, out_size, " %s--%s", base, extra_data);
}

// extra_data holds the heading level ("1".."6"). Anything else - including the
// empty value most blocks were migrated with - falls back to the level this
// renderer used to hardcode, so untagged headings keep looking the same.
static const char *heading_level(const char *extra_data) {
    if (extra_data && extra_data[0] >= '1' && extra_data[0] <= '6' && !extra_data[1])
        return extra_data;
    return "2";
}

static char *render_tittle(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/tittle/tittle_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // WML has no headings; its template renders bold text and takes the text alone.
    char *result;
    if (epoch == EPOCH_WML) {
        result = render_template(tpl, block->text);
    } else {
        const char *level = heading_level(block->extra_data);
        result = render_template(tpl, level, block->text, level);
    }
    free(tpl);
    return result;
}

// Paragraph text is stored either as HTML (what the dashboard editor writes)
// or as plain text with literal newlines (what the migration produced). HTML
// collapses a bare newline into a space, so a stored "\n" would silently
// disappear; expand it into a line break the target epoch understands.
// Returns a malloc'd copy, or NULL on allocation failure.
static char *expand_newlines(const char *text, int epoch) {
    if (!text) return strdup("");

    // WML is XHTML-strict and needs the self-closing form.
    const char *br = (epoch == EPOCH_WML) ? "<br/>" : "<br>";
    size_t br_len = strlen(br);

    size_t breaks = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') breaks++;
    if (breaks == 0) return strdup(text);

    char *out = malloc(strlen(text) + breaks * br_len + 1);
    if (!out) return NULL;

    char *w = out;
    for (const char *p = text; *p; p++) {
        if (*p == '\r') continue;          // normalise CRLF to a single break
        if (*p == '\n') {
            memcpy(w, br, br_len);
            w += br_len;
        } else {
            *w++ = *p;
        }
    }
    *w = '\0';
    return out;
}

static char *render_paragraph(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/paragraph/paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *text = expand_newlines(block->text, epoch);
    char *result;
    if (!text) {
        result = NULL;
    } else if (epoch <= EPOCH_PRESTANDARD) {
        // Text-only and WML templates carry no classes, so they take the text alone.
        result = render_template(tpl, text);
    } else {
        char mod[64];
        modifier_class(block->extra_data, "boat-rudder__paragraph", mod, sizeof(mod));
        result = render_template(tpl, mod, text);
    }
    free(text);
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

// Blocks store the bare image path; the size variant is chosen here, so older
// browsers get a lighter file instead of the full-size one. Mirrors what
// render_gallery() already does, including epoch 1's GIF (those clients predate
// progressive JPEG). Epochs <= 0 render no image at all.
static char *image_for_epoch(const char *url, int epoch) {
    if (!url || !url[0]) return strdup("");

    const char *suffix = (epoch >= EPOCH_MODERN) ? "_full"
                       : (epoch == EPOCH_MIDDLE) ? "_half"
                                                 : "_medium";
    char *variant = image_url_variant(url, suffix);
    if (variant && epoch == EPOCH_EARLY) {
        char *dot = strrchr(variant, '.');
        if (dot) strcpy(dot, ".gif");
    }
    return variant;
}

// An image block's extra_data packs three fields as "caption|width|align",
// following the same convention as social-networks ("icon|url"). Width and
// align are validated against fixed sets, so a hand-edited value can never
// reach the page as markup. A value with no '|' is all caption, which is how
// every block written before this option looked.
typedef struct {
    char caption[512];
    const char *width; // "100" | "50" | "30"
    const char *align; // "left" | "center" | "right"
} ImageOptions;

static ImageOptions parse_image_options(const char *extra_data) {
    ImageOptions o = { .caption = "", .width = "100", .align = "center" };
    if (!extra_data || !extra_data[0]) return o;

    const char *first = strchr(extra_data, '|');
    size_t caption_len = first ? (size_t)(first - extra_data) : strlen(extra_data);
    if (caption_len >= sizeof(o.caption)) caption_len = sizeof(o.caption) - 1;
    memcpy(o.caption, extra_data, caption_len);
    o.caption[caption_len] = '\0';
    if (!first) return o;

    char rest[64] = "";
    strncpy(rest, first + 1, sizeof(rest) - 1);
    char *second = strchr(rest, '|');
    if (second) *second++ = '\0';

    static const char *widths[] = { "100", "50", "30", NULL };
    for (int i = 0; widths[i]; i++)
        if (strcmp(rest, widths[i]) == 0) o.width = widths[i];

    static const char *aligns[] = { "left", "center", "right", NULL };
    for (int i = 0; second && aligns[i]; i++)
        if (strcmp(second, aligns[i]) == 0) o.align = aligns[i];

    return o;
}

static char *render_image(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/image/image_epoch%d.html", epoch);
    if (!tpl) return NULL;

    ImageOptions o = parse_image_options(block->extra_data);

    char *result;
    if (epoch <= EPOCH_PRESTANDARD) {
        // Text-only and WML render the caption alone, with no image at all.
        result = render_template(tpl, o.caption);
        free(tpl);
        return result;
    }

    char *src = image_for_epoch(block->text, epoch);
    // Clicking the image opens it at full size: epoch 3 hands it to the same
    // lightbox the galleries use, older epochs just link to the file, since
    // that viewer needs a media_galleries id an image block has no room for.
    char *full = image_url_variant(block->text, "_full");
    if (!src || !full) {
        free(src);
        free(full);
        free(tpl);
        return NULL;
    }

    if (epoch == EPOCH_EARLY) {
        // No CSS this far back: size and alignment go in HTML attributes.
        char width_attr[16];
        snprintf(width_attr, sizeof(width_attr), "%s%%", o.width);
        result = render_template(tpl, o.align, full, src, width_attr, o.caption, o.caption);
    } else {
        // Each epoch's stylesheet names the block differently, so the modifier
        // classes have to match the base class used by that epoch's template.
        const char *base = (epoch >= EPOCH_MODERN) ? "boat-rudder__entry-image"
                                                   : "br-entry-image";
        char mods[128];
        snprintf(mods, sizeof(mods), " %s--w%s %s--%s", base, o.width, base, o.align);
        // Epoch 3 puts the full-size URL in data-full for the lightbox;
        // epoch 2 wraps the image in a plain link to it.
        result = (epoch >= EPOCH_MODERN)
            ? render_template(tpl, mods, src, full, o.caption, o.caption)
            : render_template(tpl, mods, full, src, o.caption, o.caption);
    }

    free(src);
    free(full);
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

    // Epochs -1/0 emit a bare <br/>/<hr> with no class to modify.
    char *result;
    if (epoch <= EPOCH_PRESTANDARD) {
        result = render_template(tpl);
    } else {
        char mod[64];
        modifier_class(block->extra_data, "boat-rudder__separator", mod, sizeof(mod));
        result = render_template(tpl, mod);
    }
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

// Epoch 3 embeds an iframe. No earlier browser can play the video inline, so
// retro epochs render a QR code the reader scans with a phone, plus a text
// link - the QR asset differs per epoch (GIF / WBMP / Unicode half-blocks) but
// all three templates take the same (asset, short_url, label) arguments.
static char *render_youtube_embed(const CmsContentBlock *block, int epoch) {
    if (!block->extra_data || !block->extra_data[0]) return strdup("");
    char *tpl = load_template("elements/youtube-embed/youtube-embed_epoch%d.html", epoch);
    if (!tpl) return NULL;

    if (epoch >= EPOCH_MODERN) {
        char *embed = youtube_to_embed(block->extra_data);
        char *result = embed ? render_template(tpl, embed) : NULL;
        free(embed);
        free(tpl);
        return result ? result : strdup("");
    }

    char video_id[32] = {0};
    if (extract_youtube_id(block->extra_data, video_id, sizeof(video_id)) != 0) {
        free(tpl);
        return strdup("");
    }

    char short_url[128];
    youtube_short_url(video_id, short_url, sizeof(short_url));
    const char *label = (block->text && block->text[0]) ? block->text : short_url;

    char *result = NULL;
    if (epoch == EPOCH_WML) {
        char wbmp_path[256];
        if (generate_youtube_qr_wbmp(block->extra_data, HTML_ROOT_DIR) == 0) {
            youtube_qr_wbmp_web_path(video_id, wbmp_path, sizeof(wbmp_path));
            result = render_template(tpl, wbmp_path, short_url, label);
        }
    } else if (epoch == EPOCH_PRESTANDARD) {
        // Text-only browsers get the QR drawn with Unicode half-blocks.
        char *qr_text = generate_youtube_qr_text(block->extra_data);
        if (qr_text) {
            result = render_template(tpl, qr_text, short_url, label);
            free(qr_text);
        }
    } else {
        char qr_path[256];
        if (generate_youtube_qr(block->extra_data, HTML_ROOT_DIR) == 0) {
            youtube_qr_web_path(video_id, qr_path, sizeof(qr_path));
            result = render_template(tpl, qr_path, short_url, label);
        }
    }

    free(tpl);
    return result ? result : strdup("");
}

static char *render_code_text(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/code-text/code-text_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // Every epoch's template wraps the code in <pre>, which keeps newlines -
    // except WML, which has no <pre> and needs explicit breaks.
    char *text = (epoch == EPOCH_WML) ? expand_newlines(block->text, epoch)
                                      : strdup(block->text);
    char *result = text ? render_template(tpl,
        block->extra_data ? block->extra_data : "", text) : NULL;
    free(text);
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

    // Icon format follows the epoch: SVG only for modern browsers, GIF for the
    // ones that predate it (a small variant for epoch 1), and no icon at all
    // for text-only/WML clients, whose templates keep it in a comment anyway.
    char icon_path[256] = "";
    if (epoch >= EPOCH_MODERN)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s.svg", icon);
    else if (epoch == EPOCH_MIDDLE)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s.gif", icon);
    else if (epoch == EPOCH_EARLY)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s-s.gif", icon);

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
    const char *author = entry->header_author ? entry->header_author : "";
    // With hide_author set (or no author at all) the byline block is dropped
    // entirely rather than emitted empty, so the entry starts at its content.
    if (epoch >= 3 && !entry->header_hide_author && author[0]) {
        char *meta_tpl = load_template("entry/entry-meta_epoch%d.html", epoch);
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
