#include "entry_editor_blocks.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// <textarea> row count for blocks/lang-field_epoch%d.html.
#define BLOCK_TEXT_ROWS 3

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
        char *field = render_template(field_tpl, langs[i].code, BLOCK_TEXT_ROWS, block->text_values[i]);
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

char *entry_editor_render_block(const CmsContentBlockEdit *block,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    if (strcmp(block->type, "tittle") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/tittle_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "paragraph") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/paragraph_epoch%d.html",
                                   langs, lang_count, epoch);
    if (strcmp(block->type, "image") == 0)
        return render_extra_block(block, "dashboard/entries/editor/blocks/image_epoch%d.html",
                                   langs, lang_count, epoch);
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
