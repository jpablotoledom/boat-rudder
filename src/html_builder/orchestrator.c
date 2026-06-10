#include "orchestrator.h"
#include "../modules/container/container.h"
#include "../modules/home_blog/home_blog.h"
#include "../modules/home_content/home_content.h"
#include "../modules/menu/menu.h"
#include "../modules/slider/slider.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>

char *buildHomeWebSite(int epoch, const char *lang) {
    char *html_container    = container(epoch);
    char *html_menu         = menu("/", epoch);
    char *html_slider       = slider(epoch);
    char *html_home_content = home_content(epoch, lang);
    char *html_home_blog    = home_blog(epoch);

    char *result = NULL;
    if (html_container && html_menu && html_slider && html_home_content && html_home_blog) {
        result = render_template(html_container, html_menu, html_slider, html_home_content, html_home_blog);
    }

    free(html_container);
    free(html_menu);
    free(html_slider);
    free(html_home_content);
    free(html_home_blog);

    return result;
}

char *buildPageWebSite(int epoch, const char *page_title, char *html_content) {
    char *path = generate_url_theme("page/page_epoch%d.html", epoch);
    char *raw  = path ? read_file_to_string(path) : NULL;
    free(path);

    char title_tag[256];
    snprintf(title_tag, sizeof(title_tag), "<title>%s</title>", page_title);

    char *titled    = raw ? str_replace_first(raw, "{{PAGE_TITLE}}", title_tag) : NULL;
    char *html_menu = menu("/", epoch);

    char *result = NULL;
    if (titled && html_menu && html_content) {
        result = render_template(titled, html_menu, html_content);
    }

    free(raw);
    free(titled);
    free(html_menu);
    free(html_content);

    return result;
}
