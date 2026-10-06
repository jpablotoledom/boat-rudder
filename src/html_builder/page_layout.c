#include "page_layout.h"
#include "../db/cms_fonts.h"
#include "../db/cms_site_settings.h"
#include "../db/cms_themes.h"
#include "../utils/detect_epoch.h"
#include "../utils/generate_url_theme.h"
#include "../utils/http_utils.h"
#include "../utils/read_file.h"
#include "../utils/request_theme.h"
#include "../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Replaces `marker` with the contents of layout/<part>_epoch<N>.html. A part
// that has no file for this epoch resolves to nothing, which is how the retro
// epochs drop the epoch-3 viewers without needing an empty file each.
static char *splice_part(char *html, const char *marker, const char *part, int epoch) {
    if (!html || !strstr(html, marker)) return html;

    char subpath[128];
    snprintf(subpath, sizeof(subpath), "layout/%s_epoch%%d.html", part);

    char *path = generate_url_theme(subpath, epoch);
    char *body = path ? read_file_to_string(path) : NULL;
    free(path);

    char *result = str_replace_first(html, marker, body ? body : "");
    free(body);
    free(html);
    return result;
}

// Like splice_part(), but for the footer specifically: its content is
// personalizable per theme from /dashboard/settings/themes/<key>/footer,
// so it comes from cms_get_theme_footer() (DB override for the active
// theme, falling back to that same theme's on-disk
// layout/footer_epoch<N>.html) instead of a direct file read.
//
// Epoch 3's footer (dark and light themes) carries a "{{SITE_NAME}}" token
// where it shows the site's own name - str_replace_first(), not
// render_template(), because this body is raw admin-editable markup that
// can contain arbitrary '%' (CSS percentages, etc.) and must never be run
// through printf-style substitution. Older epochs have no such token in
// their on-disk footer, so this is a no-op for them.
static char *splice_footer(char *html, int epoch) {
    if (!html || !strstr(html, "{{FOOTER}}")) return html;

    char *body = cms_get_theme_footer(request_theme(), epoch);
    if (!body) body = strdup("");

    if (epoch == EPOCH_MODERN && strstr(body, "{{SITE_NAME}}")) {
        char *site_name = cms_get_site_name();

        // The epoch-3 logo panel's own text (LOGO_MODE_TEXT) stands in for
        // the site name here too, same as the navbar (menu.c) - so the
        // footer title matches whatever the admin configured as "Logo en
        // fuente" (text + font, see the --br-font-navbar-logo var this
        // shares with the navbar via .boat-rudder__footer-title's own
        // font-family rule), not just the site's own name.
        CmsLogoConfig logo_cfg;
        cms_get_theme_logo_config(request_theme(), epoch, &logo_cfg);
        const char *title = (logo_cfg.mode == LOGO_MODE_TEXT && logo_cfg.text[0])
            ? logo_cfg.text : (site_name ? site_name : "");

        char *replaced = str_replace_first(body, "{{SITE_NAME}}", title);
        free(site_name);
        free(body);
        body = replaced ? replaced : strdup("");
    }

    char *result = str_replace_first(html, "{{FOOTER}}", body);
    free(body);
    free(html);
    return result;
}

// The footer logo image, "{{FOOTER_LOGO}}" in a theme's own on-disk
// layout/footer_epoch<N>.html (dark/light, epoch -1/1/2/3) - a no-op
// wherever the marker is absent, same convention as splice_part(). Reads
// straight from cms_get_theme_logo_config() (not cms_get_theme_footer(),
// which is unrelated raw footer markup): LOGO_MODE_IMAGE with a
// footer_image set renders that <img>. Text is epoch 0's only mode (see
// CmsLogoConfig's own doc comment), rendered here as the panel's own text
// or the site's name - even when nothing has ever been saved for it
// (cfg.mode staying LOGO_MODE_UNSET), since epoch 0 has no on-disk logo
// file to fall back to (this replaced its hardcoded "Boat Rudder" text).
// Epoch 3's text mode is deliberately not rendered here: its footer shows
// that text in its own {{SITE_NAME}} title beside this logo (see
// splice_footer()), so it would appear twice. LOGO_MODE_UNSET or IMAGE
// with no footer_image saved, on every other epoch, falls through to the
// on-disk default below.
static char *splice_footer_logo(char *html, int epoch) {
    if (!html || !strstr(html, "{{FOOTER_LOGO}}")) return html;

    CmsLogoConfig cfg;
    cms_get_theme_logo_config(request_theme(), epoch, &cfg);

    char *img = NULL;
    if (cfg.mode == LOGO_MODE_IMAGE && cfg.footer_image[0]) {
        char *site_name = cms_get_site_name();
        char encoded_alt[512];
        html_encode(encoded_alt, site_name ? site_name : "", sizeof(encoded_alt));
        free(site_name);
        img = render_template("<img src=\"/themes/%s/assets/menu/epoch%d/%s\" alt=\"%s\">",
                               request_theme(), epoch, cfg.footer_image, encoded_alt);
    } else if (epoch == EPOCH_PRESTANDARD) {
        char *site_name = cms_get_site_name();
        const char *text = cfg.text[0] ? cfg.text : (site_name ? site_name : "");
        char encoded[1600];
        html_encode(encoded, text, sizeof(encoded));
        free(site_name);
        img = strdup(encoded);
    }
    if (!img) {
        // Nothing saved via the new panel (or the admin picked epoch 3's
        // text mode instead) - fall back to the theme's own on-disk
        // default, same convention as splice_part()/cms_get_theme_logo():
        // an untouched theme keeps the footer logo it always had.
        char *path = generate_url_theme("layout/footer-logo_epoch%d.html", epoch);
        img = path ? read_file_to_string(path) : NULL;
        free(path);
        if (!img) img = strdup("");
    }

    char *result = str_replace_first(html, "{{FOOTER_LOGO}}", img);
    free(img);
    free(html);
    return result;
}

// Builds the @font-face + --br-font-navbar-logo override for the active
// theme's chosen logo font, or "" if none applies (styles_epoch3.css's own
// hardcoded Milonga @font-face, and var(--br-font-navbar-logo, Milonga)'s
// own fallback, cover that case with no override needed here at all).
// `font_name` is either the Themes/Colors default-font picker's plain
// uploaded-font name (cms_get_theme_logo_font()), or - when the epoch-3
// logo panel itself is in LOGO_MODE_TEXT (see splice_theme_colors() below) -
// that panel's own CmsLogoConfig.font, "system:<name>" or "uploaded:<name>":
// a "system:" font is a plain CSS font-family the visitor's own OS/browser
// is expected to already have, so it skips @font-face entirely and only
// sets the CSS var; "uploaded:" (and the bare, prefix-less form the
// Colors picker still stores) resolve to an uploaded file the same way as
// before.
static char *build_logo_font_css(const char *font_name) {
    if (!font_name || !font_name[0]) return strdup("");

    if (strncmp(font_name, "system:", 7) == 0) {
        const char *system_name = font_name + 7;
        if (!system_name[0]) return strdup("");
        return render_template(":root{--br-font-navbar-logo:'%s';}", system_name);
    }

    const char *lookup_name = (strncmp(font_name, "uploaded:", 9) == 0) ? font_name + 9 : font_name;
    if (!lookup_name[0]) return strdup("");

    char *filename = cms_get_font_filename_by_name(lookup_name);
    if (!filename || !filename[0]) {
        free(filename);
        return strdup("");
    }

    char *css = render_template(
        "@font-face{font-family:'%s';src:url('/assets/fonts/%s') format('%s');}"
        ":root{--br-font-navbar-logo:'%s';}",
        lookup_name, filename, cms_font_format_for_filename(filename), lookup_name);
    free(filename);
    return css ? css : strdup("");
}

// Replaces {{THEME_COLORS}} (epoch 3's layout only - older epochs have no
// color model, and simply lack the marker, so this is a no-op there) with a
// small inline <style>:root{...}</style> fragment carrying the active
// theme's DB-editable color tokens (/dashboard/settings/themes) - the same
// one shared palette every epoch reads from, per cms_themes.h - plus the
// theme's chosen logo font, if any (see build_logo_font_css()). Every
// styles_epoch3.css rule that opts in reads its value via
// var(--br-color-x, <hardcoded-default>), so a theme with no saved colors
// renders identically whether or not this marker even exists.
static char *splice_theme_colors(char *html) {
    if (!html || !strstr(html, "{{THEME_COLORS}}")) return html;

    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    // The epoch-3 logo panel's own font choice (site_settings_logo_page(),
    // LOGO_MODE_TEXT only) overrides the Themes/Colors default-font picker
    // for this theme whenever it's set - see build_logo_font_css()'s doc
    // comment and CmsLogoConfig's in cms_themes.h.
    CmsLogoConfig logo_cfg;
    cms_get_theme_logo_config(request_theme(), EPOCH_MODERN, &logo_cfg);
    char *logo_font = (logo_cfg.mode == LOGO_MODE_TEXT && logo_cfg.font[0])
        ? strdup(logo_cfg.font)
        : cms_get_theme_logo_font(request_theme());
    char *font_css = build_logo_font_css(logo_font);
    free(logo_font);

    char *style = render_template(
        "<style>:root{"
        "--br-color-navbar-background:%s;--br-color-navbar-menu-normal:%s;"
        "--br-color-navbar-menu-hover:%s;--br-color-navbar-menu-active:%s;"
        "--br-color-navbar-logo:%s;--br-color-body-background:%s;"
        "--br-color-home-content-background:%s;--br-color-home-content-text:%s;"
        "--br-color-blog-list-item-background:%s;--br-color-blog-list-item-border:%s;"
        "--br-color-blog-list-item-author:%s;--br-color-blog-list-item-categories:%s;"
        "--br-color-blog-list-item-categories-hover:%s;"
        "--br-color-blog-list-item-date:%s;"
        "--br-color-footer-logo:%s;--br-color-footer-logo-background:%s;"
        "--br-color-link-normal:%s;--br-color-link-hover:%s;"
        "--br-color-link-visited:%s;--br-color-link-active:%s;"
        "--br-color-table-header:%s;--br-color-table-border:%s;"
        "--br-color-table-row-a:%s;--br-color-table-row-b:%s;"
        "--br-color-code-background:%s;--br-color-code-text:%s;"
        "--br-color-code-keyword:%s;--br-color-code-string:%s;"
        "--br-color-code-comment:%s;--br-color-code-number:%s;"
        "--br-color-code-variable:%s;--br-color-code-tag:%s;"
        "--br-color-code-line-number:%s;"
        "}%s</style>",
        colors.navbar_background, colors.navbar_menu_normal,
        colors.navbar_menu_hover, colors.navbar_menu_active,
        colors.navbar_logo, colors.body_background,
        colors.home_content_background, colors.home_content_text,
        colors.blog_list_item_background, colors.blog_list_item_border,
        colors.blog_list_item_author, colors.blog_list_item_categories,
        colors.blog_list_item_categories_hover,
        colors.blog_list_item_date,
        colors.footer_logo, colors.footer_logo_background,
        colors.link_normal, colors.link_hover, colors.link_visited, colors.link_active,
        colors.table_header, colors.table_border, colors.table_row_a, colors.table_row_b,
        colors.code_background, colors.code_text,
        colors.code_keyword, colors.code_string,
        colors.code_comment, colors.code_number,
        colors.code_variable, colors.code_tag,
        colors.code_line_number,
        font_css ? font_css : "");
    free(font_css);

    char *result = str_replace_first(html, "{{THEME_COLORS}}", style ? style : "");
    free(style);
    free(html);
    return result;
}

// Replaces {{COLOR_BACKGROUND}}/{{COLOR_TEXT}}/{{COLOR_LINK}}/{{COLOR_VLINK}}/
// {{COLOR_ALINK}}/{{COLOR_LINK_HOVER}}/{{COLOR_MENU_NORMAL}}/
// {{COLOR_MENU_ACTIVE}}/{{COLOR_MENU_HOVER}}/{{COLOR_CATEGORIES_HOVER}}
// (epoch 1/2's layout only -
// epoch 3 has its own {{THEME_COLORS}} CSS block above, and epoch -1/0 have
// no color model, so those layouts simply lack these markers and this is a
// no-op) with plain hex values from the *same* cms_get_theme_colors() epoch
// 3 reads - one shared palette, not a separate one per epoch (see
// cms_themes.h), with exactly one exception: {{COLOR_BACKGROUND}} on epoch 1
// takes body_background_epoch1 instead of body_background, since a
// freely-picked color can render dithered rather than solid on the
// indexed-color displays epoch 1's real browsers predate (see
// cms_themes.h's own doc comment on that field). Epoch 1 has no CSS at all,
// so none of the {{COLOR_LINK_HOVER}}/{{COLOR_MENU_*}} tokens ever appear in
// its layout - link/vlink/alink go straight into <body> attributes there
// (those predate CSS entirely, HTML 2.0/Netscape 1.1, so real epoch-1
// browsers still honor them), and the menu's own colors go straight into a
// <font color> attribute per item instead (see menu.c). Epoch 2's range
// (Netscape 4 through IE10/Firefox 3/Chrome 9) does have CSS1, so its
// layout carries real `a:hover`/`#boat-rudder-navbar-menu-item(-selected)`/
// `.boat-rudder__menu__item:hover` rules instead of an inline attribute -
// see menu.c's own comment for the several rounds of real-browser testing
// (IE5/Windows 3.11 vs. 95) that shaped exactly which selector forms those
// are (and which plausible-looking ones turned out not to work).
// {{COLOR_LINK}}/
// {{COLOR_VLINK}}/{{COLOR_ALINK}}/{{COLOR_LINK_HOVER}} take link-normal/
// link-visited/link-active/link-hover - the generic (non-menu) link colors,
// kept independent of the nav bar's own menu-normal/hover/active so an
// admin can style one without moving the other; {{COLOR_MENU_NORMAL}}/
// {{COLOR_MENU_ACTIVE}}/{{COLOR_MENU_HOVER}} take navbar-menu-normal/
// active/hover themselves, coloring the menu, not generic content links.
// {{COLOR_TEXT}} uses home-content-text (body text has no dedicated token
// of its own in the Figma palette - home-content-text is the closest
// match, being the main visible text color on these epochs' pages).
// {{COLOR_CATEGORIES}}/{{COLOR_CATEGORIES_HOVER}} take blog-list-item-
// categories/-hover, for #boat-rudder-entry-category(:hover) in epoch 2's
// layout (see category_epoch2.html and category_tags.c's own comment) - an
// id, not the boat-rudder__entry-category class epoch 3 uses, and no inline
// color attribute at all: real-browser testing (IE5/Windows 3.11) showed an
// inline `style="color:..."` there can never be overridden on :hover, not
// even with `!important`, same as the navbar's own menu items (see menu.c).
// Epoch 1 predates CSS entirely, so neither token is used there.
static char *splice_retro_colors(char *html, int epoch) {
    if (!html) return NULL;
    if (!strstr(html, "{{COLOR_BACKGROUND}}") && !strstr(html, "{{COLOR_TEXT}}") &&
        !strstr(html, "{{COLOR_LINK}}") && !strstr(html, "{{COLOR_VLINK}}") &&
        !strstr(html, "{{COLOR_ALINK}}") && !strstr(html, "{{COLOR_LINK_HOVER}}") &&
        !strstr(html, "{{COLOR_MENU_NORMAL}}") && !strstr(html, "{{COLOR_MENU_ACTIVE}}") &&
        !strstr(html, "{{COLOR_MENU_HOVER}}") && !strstr(html, "{{COLOR_CATEGORIES}}") &&
        !strstr(html, "{{COLOR_CATEGORIES_HOVER}}"))
        return html;

    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    const char *background = (epoch == EPOCH_EARLY) ? colors.body_background_epoch1
                                                      : colors.body_background;

    char *step = str_replace_all(html, "{{COLOR_BACKGROUND}}", background);
    free(html);
    if (!step) return NULL;

    char *next = str_replace_all(step, "{{COLOR_TEXT}}", colors.home_content_text);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_LINK}}", colors.link_normal);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_VLINK}}", colors.link_visited);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_ALINK}}", colors.link_active);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_LINK_HOVER}}", colors.link_hover);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_MENU_NORMAL}}", colors.navbar_menu_normal);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_MENU_ACTIVE}}", colors.navbar_menu_active);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_MENU_HOVER}}", colors.navbar_menu_hover);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_CATEGORIES}}", colors.blog_list_item_categories);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_all(step, "{{COLOR_CATEGORIES_HOVER}}", colors.blog_list_item_categories_hover);
    free(step);
    return next;
}

// Every remaining "{{SITE_NAME}}" in the page - a WML card's title, an
// image's alt text - becomes the configured site name (site_settings),
// HTML-escaped. After splice_footer(), which fills epoch 3's footer title
// with its logo text instead.
static char *splice_site_name(char *html) {
    if (!html || !strstr(html, "{{SITE_NAME}}")) return html;
    char *site_name = cms_get_site_name();
    char encoded[512];
    html_encode(encoded, site_name ? site_name : "", sizeof(encoded));
    free(site_name);
    char *result = str_replace_all(html, "{{SITE_NAME}}", encoded);
    free(html);
    return result;
}

char *page_layout_wrap(char *fragment_html, const char *page_title, int epoch,
                       const char *body_background) {
    if (!fragment_html) return NULL;

    fragment_html = splice_footer(fragment_html, epoch);
    fragment_html = splice_footer_logo(fragment_html, epoch);
    fragment_html = splice_part(fragment_html, "{{LIGHTBOX}}",   "lightbox",   epoch);
    fragment_html = splice_part(fragment_html, "{{HOME-MODAL}}", "home-modal", epoch);
    fragment_html = splice_site_name(fragment_html);
    if (!fragment_html) return NULL;

    char *path   = generate_url_theme("layout/layout_epoch%d.html", epoch);
    char *layout = path ? read_file_to_string(path) : NULL;
    free(path);
    if (!layout) {
        free(fragment_html);
        return NULL;
    }

    // A title may name the site as "{{SITE_NAME}} - Blog" (http_router.c's
    // routes do) rather than hardcode it - resolved here, for every epoch.
    char *resolved_title = NULL;
    if (page_title && strstr(page_title, "{{SITE_NAME}}")) {
        char *site_name = cms_get_site_name();
        resolved_title = str_replace_all(page_title, "{{SITE_NAME}}", site_name ? site_name : "");
        free(site_name);
        if (resolved_title) page_title = resolved_title;
    }

    // {{PAGE_TITLE}} is the title as text, escaped - each layout puts it in
    // its own element: <title> for HTML, the <card>'s title attribute for WML.
    char encoded_title[512];
    html_encode(encoded_title, page_title ? page_title : "", sizeof(encoded_title));
    free(resolved_title);
    char *titled = str_replace_first(layout, "{{PAGE_TITLE}}", encoded_title);
    free(layout);
    if (!titled) {
        free(fragment_html);
        return NULL;
    }

    char *with_bg = str_replace_first(titled, "{{BODY_BACKGROUND}}",
                                      body_background ? body_background : "");
    free(titled);
    if (!with_bg) {
        free(fragment_html);
        return NULL;
    }

    char *with_retro_colors = splice_retro_colors(with_bg, epoch);
    if (!with_retro_colors) {
        free(fragment_html);
        return NULL;
    }

    char *with_colors = splice_theme_colors(with_retro_colors);
    if (!with_colors) {
        free(fragment_html);
        return NULL;
    }

    char *result = str_replace_first(with_colors, "{{CONTENT}}", fragment_html);
    free(with_colors);
    free(fragment_html);
    return result;
}
