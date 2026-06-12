#include "entry_editor.h"
#include "entry_editor_blocks.h"
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

static int is_category_selected(const CmsEntryEdit *entry, const char *category_id) {
    for (size_t i = 0; i < entry->category_count; i++)
        if (strcmp(entry->category_ids[i], category_id) == 0) return 1;
    return 0;
}

// Renders meta_epoch%d.html: entry link, type <select>, categories
// <select multiple> (one category-option_epoch%d.html per category, "selected"
// if its id is in entry->category_ids[]), and the "Published" checkbox.
static char *render_meta_sidebar(const CmsEntryEdit *entry, const CmsCategoryItem *categories,
                                  size_t category_count, int epoch) {
    char *tpl = load_template("dashboard/entries/editor/meta_epoch%d.html", epoch);
    char *option_tpl = load_template("dashboard/entries/editor/category-option_epoch%d.html", epoch);
    if (!tpl || !option_tpl) {
        free(tpl);
        free(option_tpl);
        return NULL;
    }

    char *options = strdup("");
    for (size_t i = 0; options && i < category_count; i++) {
        const char *selected = is_category_selected(entry, categories[i].id) ? " selected" : "";
        char *option = render_template(option_tpl, categories[i].id, selected, categories[i].name);
        options = option ? str_append(options, option) : NULL;
        free(option);
    }

    char *result = NULL;
    if (options) {
        const char *page_selected = strcmp(entry->type, "page") == 0 ? " selected" : "";
        const char *blog_selected = strcmp(entry->type, "blog") == 0 ? " selected" : "";
        const char *enabled_checked = entry->enabled ? " checked" : "";
        result = render_template(tpl, entry->link, page_selected, blog_selected, options, enabled_checked);
    }

    free(options);
    free(tpl);
    free(option_tpl);
    return result;
}

// Renders header_epoch%d.html: image_url/date inputs plus one
// header-lang-tab_epoch%d.html per language (title/summary/author for
// entry->header_*_values[i]), wrapped as language panels.
static char *render_header_sidebar(const CmsEntryEdit *entry, const CmsLanguageItem *langs,
                                    size_t lang_count, int epoch) {
    char *tpl = load_template("dashboard/entries/editor/header_epoch%d.html", epoch);
    char *panel_tpl = load_template("dashboard/entries/editor/header-lang-tab_epoch%d.html", epoch);
    if (!tpl || !panel_tpl) {
        free(tpl);
        free(panel_tpl);
        return NULL;
    }

    char *panels = strdup("");
    for (size_t i = 0; panels && i < lang_count; i++) {
        char *panel = render_template(panel_tpl, langs[i].code, entry->header_title_values[i],
                                       entry->header_summary_values[i], entry->header_author_values[i]);
        panels = panel ? str_append(panels, panel) : NULL;
        free(panel);
    }

    char *result = panels
        ? render_template(tpl, entry->header_image_url, entry->header_date, panels)
        : NULL;

    free(panels);
    free(tpl);
    free(panel_tpl);
    return result;
}

// Renders one lang-tab-button_epoch%d.html per language - the global tab row
// that drives setLang() for both the header sidebar and every content block.
static char *render_lang_tab_buttons(const CmsLanguageItem *langs, size_t lang_count, int epoch) {
    char *tab_tpl = load_template("dashboard/entries/editor/lang-tab-button_epoch%d.html", epoch);
    if (!tab_tpl) return NULL;

    char *result = strdup("");
    for (size_t i = 0; result && i < lang_count; i++) {
        char *tab = render_template(tab_tpl, langs[i].code, langs[i].code, langs[i].name);
        result = tab ? str_append(result, tab) : NULL;
        free(tab);
    }

    free(tab_tpl);
    return result;
}

// Builds a JSON array literal of language codes, e.g. ["en","es"], for the
// editor's data-langs attribute / EDITOR_LANGS JS constant.
static char *build_editor_langs_json(const CmsLanguageItem *langs, size_t lang_count) {
    char *result = strdup("[");
    for (size_t i = 0; result && i < lang_count; i++) {
        char *item = render_template(i == 0 ? "\"%s\"" : ",\"%s\"", langs[i].code);
        result = item ? str_append(result, item) : NULL;
        free(item);
    }

    if (result) {
        char *closed = str_append(result, "]");
        result = closed;
    }

    return result;
}

char *entry_editor_page(int epoch, const CmsEntryEdit *entry,
                         const CmsCategoryItem *categories, size_t category_count,
                         const CmsLanguageItem *langs, size_t lang_count) {
    char *container_tpl = load_template("dashboard/entries/editor/container_epoch%d.html", epoch);
    if (!container_tpl) return NULL;

    char *langs_json   = build_editor_langs_json(langs, lang_count);
    char *lang_tabs    = render_lang_tab_buttons(langs, lang_count, epoch);
    char *meta_html    = render_meta_sidebar(entry, categories, category_count, epoch);
    char *header_html  = render_header_sidebar(entry, langs, lang_count, epoch);
    char *blocks_html  = entry_editor_render_blocks(entry, langs, lang_count, epoch);

    char *result = NULL;
    if (langs_json && lang_tabs && meta_html && header_html && blocks_html) {
        result = render_template(container_tpl, entry->id, langs_json, lang_tabs,
                                  meta_html, header_html, blocks_html);
    }

    free(langs_json);
    free(lang_tabs);
    free(meta_html);
    free(header_html);
    free(blocks_html);
    free(container_tpl);
    return result;
}
