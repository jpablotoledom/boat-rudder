#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

// Builds the full Home page (HTML or WML, depending on `epoch`) by
// assembling the container, menu, slider and home_content modules.
//
// Returns a malloc'd string, or NULL if any component could not be built
// (e.g. a template file is missing) - the caller should respond with a
// 500 Internal Server Error in that case. The caller must free() the
// returned buffer.
char *buildHomeWebSite(int epoch, const char *lang);

// Builds a generic, non-home page (login, dashboard, error, ...) by wrapping
// `html_content` in the page_epoch<N>.html shell (head + menu + footer) and
// resolving {{PAGE_TITLE}} as "<title>page_title</title>".
//
// Takes ownership of `html_content` (always frees it, even on failure).
// Returns a malloc'd string, or NULL if the shell template or menu could not
// be built. The caller must free() the returned buffer.
char *buildPageWebSite(int epoch, const char *page_title, char *html_content);

// Same as buildPageWebSite() but highlights the menu item matching current_url.
char *buildPageWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                             const char *current_url);

// Like buildPageWebSiteAtUrl() but also renders a category sub-menu bar below
// the main navbar. Used for all blog pages (/blog, /blog/<slug>,
// /blog/category/<slug>). Takes ownership of both `html_content` and
// `category_menu_html` (frees them in all paths). If `category_menu_html` is
// NULL the page renders identically to buildPageWebSiteAtUrl().
char *buildBlogWebSiteAtUrl(int epoch, const char *page_title, char *html_content,
                             const char *current_url, char *category_menu_html);

#endif // ORCHESTRATOR_H
