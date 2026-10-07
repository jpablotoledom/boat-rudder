#ifndef REQUEST_CODE_LINES_H
#define REQUEST_CODE_LINES_H

// Whether this request asked for code-text blocks without line numbers
// (`?code_lines=off`). Epoch 2 writes its line numbers into the code itself
// (see code_highlight_html_fonts()), and its readers' JavaScript can't
// rewrite a drawn page - so hiding them is a reload with this parameter,
// set per request from http_router.c like request_theme_set().
void request_code_lines_set(const char *query_value);
int  request_code_lines_hidden(void);

#endif
