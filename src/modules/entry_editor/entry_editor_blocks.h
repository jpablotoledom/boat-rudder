#ifndef ENTRY_EDITOR_BLOCKS_H
#define ENTRY_EDITOR_BLOCKS_H

#include "../../db/cms_entries_admin.h"
#include "../../db/cms_languages.h"
#include <stddef.h>

// Renders one content block's edit form: a toolbar (move/remove buttons), one
// "blocks/lang-field_epoch%d.html" per language for block->text_values[i], a
// type-specific extra field for "image"/"byline" (bound to block->extra_data),
// and an empty ".boat-rudder__entry-editor__block-preview" div filled by the
// editor's JS. Used both for the full editor page (entry_editor_render_blocks())
// and for the POST /dashboard/api/entries/<id>/blocks "add block" response
// (same HTML either way). Unknown types render "" (defensive, mirrors
// entry_page.c's render_block()).
char *entry_editor_render_block(const CmsContentBlockEdit *block,
                                 const CmsLanguageItem *langs, size_t lang_count, int epoch);

// Renders entry->content[] (each via entry_editor_render_block()), wrapped in
// dashboard/entries/editor/blocks_epoch%d.html (which also contains the
// "+ Add block" dropdown and the "Save content"/autosave controls).
char *entry_editor_render_blocks(const CmsEntryEdit *entry,
                                  const CmsLanguageItem *langs, size_t lang_count, int epoch);

#endif // ENTRY_EDITOR_BLOCKS_H
