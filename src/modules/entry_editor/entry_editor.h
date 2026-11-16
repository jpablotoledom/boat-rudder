#ifndef ENTRY_EDITOR_H
#define ENTRY_EDITOR_H

#include "../../db/cms_categories.h"
#include "../../db/cms_entries_admin.h"
#include "../../db/cms_languages.h"
#include "../../db/cms_site_settings.h"
#include <stddef.h>

// Renders the full /dashboard/entries/<id>/edit page content (everything
// inside <section class="boat-rudder-dashboard boat-rudder-entry-editor">):
// the global language-tab row, the entry-settings sidebar, the post-header
// sidebar, the content-blocks section (see entry_editor_blocks.h), and the
// editor's inline <script>. `categories` is every entry_category (for the
// settings sidebar's multi-select, "selected" if its id is in
// entry->category_ids[]); `langs` is cms_get_languages()'s result (for the
// language tabs, header fields and the EDITOR_LANGS JS array). `translate`
// sets the Translate button (none with no provider): its provider and, for
// a local translator, the address and key the browser calls it with - never
// the Azure key.
char *entry_editor_page(int epoch, const CmsEntryEdit *entry,
                         const CmsCategoryItem *categories, size_t category_count,
                         const CmsLanguageItem *langs, size_t lang_count,
                         const CmsTranslateSettings *translate);

#endif // ENTRY_EDITOR_H
