#include "orchestrator.h"
#include "page_layout.h"
#include "../utils/detect_epoch.h"
#include "../modules/blog_list/blog_list.h"
#include "../modules/home_content/home_content.h"
#include "../modules/menu/menu.h"
#include "../modules/mainbanner/mainbanner.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

// Home and the blog listing lay their content over the tiled backdrop; the
// content pages leave the body plain. Epoch 2 is the only layout with the slot.
#define BODY_BACKDROP " background=\"/themes/dark/assets/home-content/matrix-grid_epoch1-xl.gif\""

char *buildHomeWebSite(int epoch, const char *lang) {
    // Home's fragment, alongside the other three under page/: it is the one
    // with four regions, which is why it cannot be the same file as the rest.
    char *path = generate_url_theme("page/page-home_epoch%d.html", epoch);
    char *raw  = path ? read_file_to_string(path) : NULL;
    free(path);

    char *html_menu         = menu("/", epoch);  // home is always "/"
    char *html_mainbanner   = mainbanner(epoch);
    char *html_home_content = home_content(epoch, lang);
    char *html_home_blog    = home_blog(epoch, lang);

    char *result = NULL;
    if (raw && html_menu && html_mainbanner && html_home_content && html_home_blog) {
        char *fragment = render_template(raw, html_menu, html_mainbanner,
                                          html_home_content, html_home_blog);
        result = page_layout_wrap(fragment, "Boat Rudder - Home", epoch, BODY_BACKDROP);
    }

    free(raw);
    free(html_menu);
    free(html_mainbanner);
    free(html_home_content);
    free(html_home_blog);

    return result;
}

char *buildPageWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                             const char *current_url) {
    char *path = generate_url_theme("page/page_epoch%d.html", epoch);
    char *raw  = path ? read_file_to_string(path) : NULL;
    free(path);

    char *html_menu = menu(current_url ? current_url : "/", epoch);

    char *result = NULL;
    if (raw && html_menu && html_content) {
        char *fragment = render_template(raw, html_menu, html_content);
        result = page_layout_wrap(fragment, page_title, epoch, NULL);
    }

    free(raw);
    free(html_menu);
    free(html_content);

    return result;
}

char *buildPageWebSite(int epoch, const char *page_title, char *html_content) {
    return buildPageWebSiteAtUrl(epoch, page_title, html_content, "/");
}

static char *build_blog_page_internal(const char *tpl_fmt, int epoch,
                                       const char *page_title, char *html_content,
                                       const char *current_url, char *category_menu_html,
                                       const char *body_background) {
    char *path = generate_url_theme(tpl_fmt, epoch);
    char *raw  = path ? read_file_to_string(path) : NULL;
    free(path);

    char *html_menu = menu(current_url ? current_url : "/", epoch);

    char *combined_nav;
    if (category_menu_html) {
        // WML has no block-level separation of its own between two chunks
        // of markup concatenated back to back (no div/section, and margins
        // are a CSS concept it predates) - without a blank-line spacer the
        // language selector and the category list ran straight into each
        // other with no visible gap at all in a real WAP emulator.
        if (epoch == EPOCH_WML) {
            char *spaced = str_append(html_menu, "<p><br/></p>");
            html_menu = spaced ? spaced : html_menu;
        }
        combined_nav = str_append(html_menu, category_menu_html);
        free(category_menu_html);
        if (!combined_nav) combined_nav = strdup("");
    } else {
        combined_nav = html_menu;
    }

    char *result = NULL;
    if (raw && combined_nav && html_content) {
        char *fragment = render_template(raw, combined_nav, html_content);
        result = page_layout_wrap(fragment, page_title, epoch, body_background);
    }

    free(raw);
    free(combined_nav);
    free(html_content);

    return result;
}

char *buildBlogListWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                                 const char *current_url, char *category_menu_html) {
    // Epochs 2-3 have a full-width listing wrapper; older ones fall back to the
    // generic page shell, which is all their layout supports.
    const char *tpl = (epoch >= EPOCH_MIDDLE) ? "page/page-blog_epoch%d.html"
                                              : "page/page_epoch%d.html";
    // The listing shares home's backdrop; an article does not.
    return build_blog_page_internal(tpl, epoch, page_title, html_content, current_url,
                                     category_menu_html, BODY_BACKDROP);
}

char *buildEntryWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                              const char *current_url, char *category_menu_html) {
    const char *tpl = (epoch >= 2) ? "page/page-entry_epoch%d.html" : "page/page_epoch%d.html";
    return build_blog_page_internal(tpl, epoch, page_title, html_content, current_url,
                                     category_menu_html, NULL);
}
