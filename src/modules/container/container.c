#include "container.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>

char *container(int epoch, const char *page_title) {
    char *path = generate_url_theme("container/container_epoch%d.html", epoch);
    if (!path) return NULL;

    char *raw = read_file_to_string(path);
    free(path);
    if (!raw) return NULL;

    char *title_tag = build_title_tag(page_title);
    char *result = title_tag ? str_replace_first(raw, "{{PAGE_TITLE}}", title_tag) : NULL;
    free(title_tag);
    free(raw);
    return result;
}
