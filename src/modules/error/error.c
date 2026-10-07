#include "error.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

typedef struct {
    int code;
    const char *message;
} error_message_t;

static const error_message_t DEFAULT_MESSAGES[] = {
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {503, "Service Unavailable"},
};

#define DEFAULT_MESSAGE_COUNT (sizeof(DEFAULT_MESSAGES) / sizeof(DEFAULT_MESSAGES[0]))

static const char *default_message_for(int status_code) {
    for (size_t i = 0; i < DEFAULT_MESSAGE_COUNT; i++) {
        if (DEFAULT_MESSAGES[i].code == status_code) return DEFAULT_MESSAGES[i].message;
    }
    return "Error";
}

char *error_content(int epoch, int status_code, const char *message) {
    char *path = generate_url_theme("error/error_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    if (!message) message = default_message_for(status_code);

    char code_str[16];
    snprintf(code_str, sizeof(code_str), "%d", status_code);

    char *result = render_template(tpl, code_str, message);
    free(tpl);
    return result;
}
