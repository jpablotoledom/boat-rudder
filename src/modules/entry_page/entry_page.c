#include "entry_page.h"
#include "../../utils/category_tags.h"
#include "../../utils/image_size.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/qr_generator/qr_generator.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Widest an image may be drawn on the retro epochs, in pixels.
//
// Their browsers ran on 640x480 screens, and there is no reflow to rescue a
// layout that overshoots: the page simply grows a horizontal scrollbar and the
// text runs off the right edge. The budget at 640 is roughly 620 usable once
// the vertical scrollbar is taken out, 95% of that for the content table, less
// its 10px cellpadding on each side and the image's own 5px hspace - about
// 560. Capping a little under that keeps a full-width image inside the column.
#define RETRO_MAX_IMAGE_WIDTH 550

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

    const char *suffix = (epoch >= EPOCH_MODERN) ? "_full" : "_medium";
    char *variant = image_url_variant(url, suffix);

    // image-optimizer.sh writes _medium and _micro as GIF (only _full/_half/
    // _small stay JPEG), so the extension has to follow the variant or the
    // file is a 404.
    if (variant && epoch < EPOCH_MODERN) {
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
    // Clicking the image opens a bigger copy: epoch 3 hands it to the same
    // lightbox the galleries use, older epochs just link to the file, since
    // that viewer needs a media_galleries id an image block has no room for.
    // They get `_half` (capped at 1024px) rather than the original: a machine
    // of that era has neither the memory nor the link for a full-resolution
    // photo, and 1024 already exceeds anything it can display.
    const char *open_suffix = (epoch >= EPOCH_MODERN) ? "_full" : "_half";
    char *full = image_url_variant(block->text, open_suffix);
    if (!src || !full) {
        free(src);
        free(full);
        free(tpl);
        return NULL;
    }

    if (epoch < EPOCH_MODERN) {
        // Netscape 4 and IE 5 apply stylesheets only in part - `margin: auto`
        // on a block is exactly the sort of thing they get wrong - so size and
        // alignment travel as HTML attributes instead: align on the cell,
        // width on the image. Epoch 1 lays it out with <p align>, epoch 2 with
        // a table.
        //
        // The width has to be in pixels. Percentages on <img width> came with
        // HTML 4.0, and a browser of this era meeting one draws the image zero
        // pixels wide - it loads, it just cannot be seen. So the author's
        // 100/50/30 is resolved against the real width of the file being
        // served. If that width cannot be read the attribute is dropped and
        // the image comes out at its natural size, which beats not at all.
        char width_attr[32] = "";
        int intrinsic = image_intrinsic_width(src);
        int percent = atoi(o.width);
        if (intrinsic > 0 && percent > 0) {
            int px = intrinsic * percent / 100;
            if (px > RETRO_MAX_IMAGE_WIDTH) px = RETRO_MAX_IMAGE_WIDTH;
            if (px < 1) px = 1;
            // Only the width is emitted, never the height, so the browser
            // scales the other axis and the picture keeps its proportions.
            snprintf(width_attr, sizeof(width_attr), "width=\"%d\"", px);
        }
        result = render_template(tpl, o.align, full, src, width_attr, o.caption, o.caption);
    } else {
        char mods[128];
        snprintf(mods, sizeof(mods), " boat-rudder__entry-image--w%s boat-rudder__entry-image--%s",
                 o.width, o.align);
        // The full-size URL rides along in data-full for the lightbox.
        result = render_template(tpl, mods, src, full, o.caption, o.caption);
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

    char *wrap_tpl = load_template("entry/entry-categories_epoch%d.html", epoch);
    if (!wrap_tpl) return strdup("");

    char *items = category_tags_render(entry->category_links, entry->category_names,
                                        entry->category_count, epoch);
    char *result = items ? render_template(wrap_tpl, items) : NULL;

    free(items);
    free(wrap_tpl);
    return result;
}

// One shape for every epoch: a container holding rows, each row holding items.
// Epochs that have no notion of a row - WML, text, and the epoch-3 grid - use a
// row template that is just its contents, so the loop below never branches.
// `per_row` is how many items a row holds before it is closed; 0 means one row.
static int gallery_columns(int epoch) {
    return (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE) ? 3 : 0;
}

// Thumbnail variant for the inline gallery. Epoch 1 predates progressive JPEG,
// so it gets the GIF the optimizer writes.
static char *gallery_thumb(const char *url, int epoch) {
    if (epoch == EPOCH_EARLY) {
        char *t = image_url_variant(url, "_micro");
        if (t) { char *dot = strrchr(t, '.'); if (dot) strcpy(dot, ".gif"); }
        return t;
    }
    return image_url_variant(url, "_small");
}

static char *render_gallery(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    const char *gallery_id = (block->extra_data && strlen(block->extra_data) == 24)
                              ? block->extra_data : NULL;

    char *wrap_tpl   = load_template("elements/gallery/gallery_epoch%d.html", epoch);
    char *row_tpl    = load_template("elements/gallery/gallery-row_epoch%d.html", epoch);
    char *item_tpl   = load_template("elements/gallery/gallery-item_epoch%d.html", epoch);
    char *more_tpl   = load_template("elements/gallery/gallery-item-more_epoch%d.html", epoch);
    char *hidden_tpl = load_template("elements/gallery/gallery-item-hidden_epoch%d.html", epoch);
    if (!wrap_tpl || !row_tpl || !item_tpl) {
        free(wrap_tpl); free(row_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
        return strdup("");
    }

    // Split the semicolon-separated list of image paths.
    int total = 1;
    for (const char *p = block->text; *p; p++) if (*p == ';') total++;
    char **urls = calloc((size_t)total, sizeof(char *));
    char *copy = strdup(block->text);
    int count = 0;
    if (urls && copy) {
        char *saveptr = NULL;
        for (char *tok = strtok_r(copy, ";", &saveptr); tok && count < total;
             tok = strtok_r(NULL, ";", &saveptr)) {
            while (*tok == ' ') tok++;
            if (*tok) urls[count++] = strdup(tok);
        }
    }
    free(copy);

    // Epoch 3 shows the first few and folds the rest behind a "+N" tile.
    // Epochs 1-2 cap harder and cut the rest entirely, with a "View all"
    // link/note after the gallery instead of a tile: a machine of that era is
    // the one actually paying for every image in the block, not just the
    // reader's patience, and an article can carry a gallery of 20+ photos.
    int cap = (epoch >= EPOCH_MODERN) ? 5
            : (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE) ? 3 : 0;
    int max_visible = (cap > 0 && count > cap) ? cap : count;
    int remaining   = count - max_visible;

    int per_row = gallery_columns(epoch);
    char *rows_html = strdup("");
    char *row_html  = strdup("");
    int col = 0;

    // Epochs 1-2 simply never render the images past the cap - not hidden
    // markup, nothing sent for them at all. Epoch 3's overflow tile is the
    // only case that renders every item and hides the rest with CSS.
    int render_count = (epoch >= EPOCH_MODERN) ? count : max_visible;

    for (int i = 0; i < render_count && rows_html && row_html; i++) {
        char *thumb = gallery_thumb(urls[i], epoch);
        char *full  = image_url_variant(urls[i], "_full");

        // Where the block names a gallery the item opens the viewer at this
        // image; otherwise it opens the picture itself, so the item is never
        // a dead end and no separate "no link" template is needed.
        char *href = gallery_id ? render_template("/gallery/%s?img=%d", gallery_id, i)
                                : (full ? strdup(full) : strdup(""));

        const char *tpl = item_tpl;
        char *item;
        if (epoch >= EPOCH_MODERN && remaining > 0 && i == max_visible - 1 && more_tpl)
            item = render_template(more_tpl, thumb ? thumb : "", full ? full : "", remaining + 1);
        else if (epoch >= EPOCH_MODERN && i >= max_visible && hidden_tpl)
            item = render_template(hidden_tpl, thumb ? thumb : "", full ? full : "");
        else
            item = render_template(tpl, href ? href : "", thumb ? thumb : "", full ? full : "");

        free(thumb); free(full); free(href);
        if (item) { row_html = str_append(row_html, item); free(item); }

        if (per_row > 0 && ++col >= per_row) {
            char *row = render_template(row_tpl, row_html);
            if (row) { rows_html = str_append(rows_html, row); free(row); }
            free(row_html); row_html = strdup(""); col = 0;
        }
    }

    // The last row, or the only one when the epoch does not use rows.
    if (rows_html && row_html && (col > 0 || per_row == 0)) {
        char *row = render_template(row_tpl, row_html);
        if (row) { rows_html = str_append(rows_html, row); free(row); }
    }
    free(row_html);

    for (int i = 0; i < count; i++) free(urls[i]);
    free(urls);

    char *result = rows_html ? render_template(wrap_tpl, rows_html) : NULL;
    free(rows_html);

    // "View all" goes after the closing table, not inside a row - it names
    // how many photos were left out, and where to see them.
    if (result && cap > 0 && count > cap) {
        char *view_all = NULL;
        if (gallery_id) {
            char *tpl = load_template("elements/gallery/gallery-view-all_epoch%d.html", epoch);
            if (tpl) view_all = render_template(tpl, gallery_id, count);
            free(tpl);
        } else {
            char *tpl = load_template("elements/gallery/gallery-view-all-count_epoch%d.html", epoch);
            if (tpl) view_all = render_template(tpl, count);
            free(tpl);
        }
        if (view_all) { result = str_append(result, view_all); free(view_all); }
    }

    free(wrap_tpl); free(row_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
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

    // One container template for both kinds; the tag itself is an argument,
    // and it comes last so that WML - which has no list element and prints the
    // items in a plain <p> - simply leaves it unused rather than skipping a
    // position, which printf does not define.
    const char *tag = (block->extra_data && strcmp(block->extra_data, "ol") == 0)
        ? "ol" : "ul";

    char *container_tpl = load_template("elements/list/list-container_epoch%d.html", epoch);
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

    char *result = items ? render_template(container_tpl, items, tag) : NULL;
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

// Unlike `image`/`gallery`, an image-paragraph block stores its path with the
// `_full` size suffix already baked in, not the bare path the renderer usually
// appends one to. Epochs 1-2 need the much lighter `_micro` instead, so the
// existing suffix has to be swapped out, not stacked under another one.
static char *image_paragraph_src(const char *stored_path, int epoch) {
    if (epoch != EPOCH_EARLY && epoch != EPOCH_MIDDLE) return strdup(stored_path);

    char *base = strdup(stored_path);
    if (!base) return NULL;
    char *marker = strstr(base, "_full.");
    if (marker) memmove(marker, marker + 5, strlen(marker + 5) + 1);

    char *variant = image_url_variant(base, "_micro");
    free(base);
    // _micro is always GIF (image-optimizer.sh), regardless of source format.
    if (variant) {
        char *dot = strrchr(variant, '.');
        if (dot) strcpy(dot, ".gif");
    }
    return variant;
}

static char *render_image_paragraph(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/image-paragraph/image-paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char align_attr[32] = "";
    const char *a = block->extra_data;
    if (a && (strcmp(a, "left") == 0 || strcmp(a, "right") == 0))
        snprintf(align_attr, sizeof(align_attr), "align=\"%s\"", a);
    char *src = image_paragraph_src(block->text, epoch);
    char *result = src ? render_template(tpl, src, align_attr) : NULL;
    free(src);
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
    // With hide_author set (or no author at all) the byline is dropped entirely
    // rather than emitted empty, so the entry starts at its content. Every
    // epoch has an entry-meta template; the older ones just say it plainly.
    if (!entry->header_hide_author && author[0]) {
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
