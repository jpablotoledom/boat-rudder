#include "entry_page.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *render_header(const CmsEntry *entry, int epoch) {
    char *tpl = load_template("entry/entry-header_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *result = (epoch >= 1)
        ? render_template(tpl, entry->header_image_url, entry->header_title,
                           entry->header_summary, entry->header_author, entry->header_date)
        : render_template(tpl, entry->header_title, entry->header_summary,
                           entry->header_author, entry->header_date);
    free(tpl);
    return result;
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

// Renders one content block. Unknown types render as an empty string, so the
// rest of the page still renders.
static char *render_block(const CmsContentBlock *block, int epoch) {
    if (strcmp(block->type, "tittle") == 0)    return render_tittle(block, epoch);
    if (strcmp(block->type, "paragraph") == 0) return render_paragraph(block, epoch);
    if (strcmp(block->type, "byline") == 0)    return render_byline(block, epoch);
    if (strcmp(block->type, "image") == 0)     return render_image(block, epoch);
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
    char *result = render_header(entry, epoch);
    if (!result) return NULL;

    char *categories_html = render_categories(entry, epoch);
    if (!categories_html) {
        free(result);
        return NULL;
    }
    result = str_append(result, categories_html);
    free(categories_html);
    if (!result) return NULL;

    char *content_html = entry_page_render_content(entry, epoch);
    if (!content_html) {
        free(result);
        return NULL;
    }
    result = str_append(result, content_html);
    free(content_html);

    return result;
}
