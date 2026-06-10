#include "orchestrator.h"
#include "../modules/container/container.h"
#include "../modules/home_content/home_content.h"
#include "../modules/menu/menu.h"
#include "../modules/slider/slider.h"
#include "../utils/template_utils.h"
#include <stdlib.h>

char *buildHomeWebSite(int epoch, const char *lang) {
    char *html_container    = container(epoch);
    char *html_menu         = menu("/", epoch);
    char *html_slider       = slider(epoch);
    char *html_home_content = home_content(epoch, lang);

    char *result = NULL;
    if (html_container && html_menu && html_slider && html_home_content) {
        result = render_template(html_container, html_menu, html_slider, html_home_content);
    }

    free(html_container);
    free(html_menu);
    free(html_slider);
    free(html_home_content);

    return result;
}
