#include "home_blog.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *title;
    const char *date;
    const char *author;
    const char *image;
    const char *summary;
    const char *url;
} blog_post_t;

// MVP: static "blog posts" list, no database/CMS backing yet.
static const blog_post_t BLOG_POSTS[] = {
    {"Sailing into HTML 3.2", "2026", "Boat Rudder Crew",
     "/themes/dark/assets/home-content/bg_epoch3.png",
     "A look back at table layouts and <font> tags, and how Boat Rudder "
     "renders them for HTML 3.2 browsers.",
     "/"},
    {"WAP phones welcome aboard", "2026", "Boat Rudder Crew",
     "/assets/slide/background/floor.jpg",
     "Even the smallest WML decks get a tailored response - no images, no "
     "CSS, just the essentials.",
     "/"},
    {"CSS3 hits the deck", "2026", "Boat Rudder Crew",
     "/assets/slide/background/background-wb.jpg",
     "The modern epoch gets a full mainbanner, animated navbar and a "
     "gallery for the latest posts.",
     "/"},
};

#define BLOG_POST_COUNT (sizeof(BLOG_POSTS) / sizeof(BLOG_POSTS[0]))

char *home_blog(int epoch) {
    char *item_path    = generate_url_theme("home-blog/home-blog-item_epoch%d.html", epoch);
    char *content_path = generate_url_theme("home-blog/home-blog_epoch%d.html", epoch);

    char *item_tpl    = item_path    ? read_file_to_string(item_path)    : NULL;
    char *content_tpl = content_path ? read_file_to_string(content_path) : NULL;

    free(item_path);
    free(content_path);

    char *items  = NULL;
    char *result = NULL;

    if (!item_tpl || !content_tpl) goto cleanup;

    items = strdup("");
    if (!items) goto cleanup;

    for (size_t i = 0; i < BLOG_POST_COUNT; i++) {
        const blog_post_t *post = &BLOG_POSTS[i];
        char *item;

        if (epoch >= 1) {
            // Image-card layout: image, link, title, summary, author, date.
            item = render_template(item_tpl, post->image, post->url, post->title,
                                    post->summary, post->author, post->date);
        } else {
            // Text-only layout: title, date, summary.
            item = render_template(item_tpl, post->title, post->date, post->summary);
        }
        if (!item) goto cleanup;

        items = str_append(items, item);
        free(item);
        if (!items) goto cleanup;
    }

    result = render_template(content_tpl, items);

cleanup:
    free(item_tpl);
    free(content_tpl);
    free(items);
    return result;
}
