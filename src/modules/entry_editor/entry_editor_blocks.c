#include "entry_editor_blocks.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// <textarea> row bounds for blocks/lang-field_epoch%d.html. The box opens tall
// enough for its content - a six-item list in three rows left half of it out of
// sight - and is capped so a long block does not push the rest of the editor
// off screen. Past the cap the textarea scrolls, and it can be resized by hand.
#define BLOCK_TEXT_ROWS_MIN 3
#define BLOCK_TEXT_ROWS_MAX 20

// Rows needed to show `text` without scrolling, clamped to the bounds above.
static int text_rows(const char *text) {
    if (!text) return BLOCK_TEXT_ROWS_MIN;

    int lines = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') lines++;

    if (lines < BLOCK_TEXT_ROWS_MIN) return BLOCK_TEXT_ROWS_MIN;
    if (lines > BLOCK_TEXT_ROWS_MAX) return BLOCK_TEXT_ROWS_MAX;
    return lines;
}

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// Renders blocks/lang-field_epoch%d.html for each of langs[], one
// data-lang panel per language wrapping block->text_values[i] - shared by
// all 4 block types' templates.
static char *render_lang_fields(const CmsContentBlockEdit *block,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    char *field_tpl = load_template("dashboard/entries/editor/blocks/lang-field_epoch%d.html", epoch);
    if (!field_tpl) return NULL;

    char *result = strdup("");
    for (size_t i = 0; result && i < lang_count; i++) {
        char *field = render_template(field_tpl, langs[i].code,
                                       text_rows(block->text_values[i]),
                                       block->text_values[i]);
        result = field ? str_append(result, field) : NULL;
        free(field);
    }

    free(field_tpl);
    return result;
}

// block id + extra_data only - for blocks with no per-language text.
static char *render_notext_block(const CmsContentBlockEdit *block, const char *tpl_path, int epoch) {
    char *tpl = load_template(tpl_path, epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->id, block->extra_data ? block->extra_data : "");
    free(tpl);
    return result;
}

// block id + per-language text fields + one untranslated extra_data field.
static char *render_extra_block(const CmsContentBlockEdit *block, const char *tpl_path,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    char *tpl = load_template(tpl_path, epoch);
    if (!tpl) return NULL;

    char *fields = render_lang_fields(block, langs, lang_count, epoch);
    char *result = fields ? render_template(tpl, block->id, fields, block->extra_data) : NULL;

    free(fields);
    free(tpl);
    return result;
}

// Like render_extra_block(), but feeds extra_data before the language fields.
// The image block shows a picture instead of its path, so the path inputs live
// at the bottom inside the Advanced panel while the caption stays above them -
// which reverses the order the two placeholders appear in the template.
static char *render_image_block(const CmsContentBlockEdit *block,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    char *tpl = load_template("dashboard/entries/editor/blocks/image_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *fields = render_lang_fields(block, langs, lang_count, epoch);
    char *result = fields ? render_template(tpl, block->id, block->extra_data, fields) : NULL;

    free(fields);
    free(tpl);
    return result;
}

char *entry_editor_render_block(const CmsContentBlockEdit *block,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    if (strcmp(block->type, "tittle") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/tittle_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "paragraph") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/paragraph_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "image") == 0)
        return render_image_block(block, langs, lang_count, epoch);
    if (strcmp(block->type, "byline") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/byline_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "gallery") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/gallery_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "separator") == 0)
        return render_notext_block(block, "dashboard/entries/editor/blocks/separator_epoch%d.html", epoch);
    if (strcmp(block->type, "youtube-embed") == 0)
        return render_notext_block(block, "dashboard/entries/editor/blocks/youtube-embed_epoch%d.html", epoch);
    if (strcmp(block->type, "link") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/link_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "list") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/list_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "code-text") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/code-text_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "generic") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/generic_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "image-paragraph") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/image-paragraph_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "table") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/table_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "social-networks") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/social-networks_epoch%d.html",
                                   langs, lang_count, epoch);
    return strdup("");
}

char *entry_editor_render_blocks(const CmsEntryEdit *entry,
                                  const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    char *wrap_tpl = load_template("dashboard/entries/editor/blocks_epoch%d.html", epoch);
    if (!wrap_tpl) return NULL;

    char *blocks_html = strdup("");
    for (size_t i = 0; blocks_html && i < entry->content_count; i++) {
        char *block_html = entry_editor_render_block(&entry->content[i], langs, lang_count, epoch);
        blocks_html = block_html ? str_append(blocks_html, block_html) : NULL;
        free(block_html);
    }

    char *result = blocks_html ? render_template(wrap_tpl, blocks_html) : NULL;
    free(blocks_html);
    free(wrap_tpl);
    return result;
}
