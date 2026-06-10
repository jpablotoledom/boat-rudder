#include "slider.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include <stdlib.h>

char *slider(int epoch) {
    char *path = generate_url_theme("slider/slider_epoch%d.html", epoch);
    if (!path) return NULL;

    char *result = read_file_to_string(path);
    free(path);
    return result;
}
