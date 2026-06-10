#include "container.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>

char *container(int epoch) {
    char *path = generate_url_theme("container/container_epoch%d.html", epoch);
    if (!path) return NULL;

    char *raw = read_file_to_string(path);
    free(path);
    if (!raw) return NULL;

    char *result = str_replace_first(raw, "{{PAGE_TITLE}}", "<title>Boat Rudder - Home</title>");
    free(raw);
    return result;
}
