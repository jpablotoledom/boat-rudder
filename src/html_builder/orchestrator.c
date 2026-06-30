#include "orchestrator.h"
#include "../modules/container/container.h"
#include "../modules/home_blog/home_blog.h"
#include "../modules/home_content/home_content.h"
#include "../modules/menu/menu.h"
#include "../modules/slider/slider.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/template_utils.h"
#include <stdlib.h>

char *buildHomeWebSite(int epoch, const char *lang) {
    char *html_container    = container(epoch, "Boat Rudder - Home");
    char *html_menu         = menu("/", epoch);  // home is always "/"
    char *html_slider       = slider(epoch);
    char *html_home_content = home_content(epoch, lang);
    char *html_home_blog    = home_blog(epoch, lang);

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

char *buildPageWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                             const char *current_url) {
    char *path = generate_url_theme("page/page_epoch%d.html", epoch);
    char *raw  = path ? read_file_to_string(path) : NULL;
    free(path);

    char *title_tag = build_title_tag(page_title);
    char *titled    = (raw && title_tag) ? str_replace_first(raw, "{{PAGE_TITLE}}", title_tag) : NULL;
    char *html_menu = menu(current_url ? current_url : "/", epoch);

    char *result = NULL;
    if (titled && html_menu && html_content) {
        result = render_template(titled, html_menu, html_content);
    }

    free(raw);
    free(title_tag);
    free(titled);
    free(html_menu);
    free(html_content);

    return result;
}

char *buildPageWebSite(int epoch, const char *page_title, char *html_content) {
    return buildPageWebSiteAtUrl(epoch, page_title, html_content, "/");
}
