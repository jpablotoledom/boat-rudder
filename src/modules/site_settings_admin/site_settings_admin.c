#include "site_settings_admin.h"
#include "../../db/cms_fonts.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// html_encode() needs a caller-sized buffer; worst case every byte of `src`
// expands to "&quot;" (6 bytes), so 6x + 1 is always enough.
static char *html_encode_alloc(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    html_encode(out, src, cap);
    return out;
}

char *site_settings_general_page(int epoch, const char *site_name, const char *error_message) {
    char *page_tpl  = load_template("dashboard/settings/settings_epoch%d.html", epoch);
    char *error_tpl = load_template("dashboard/settings/settings-error_epoch%d.html", epoch);

    char *error_html = NULL;
    char *encoded_name = NULL;
    char *result = NULL;

    if (!page_tpl) goto cleanup;

    if (error_message && error_message[0] && error_tpl)
        error_html = render_template(error_tpl, error_message);

    encoded_name = html_encode_alloc(site_name ? site_name : "");
    if (!encoded_name) goto cleanup;

    result = render_template(page_tpl, error_html ? error_html : "", encoded_name);

cleanup:
    free(page_tpl);
    free(error_tpl);
    free(error_html);
    free(encoded_name);
    return result;
}

// Epoch (-1..3) -> a short human label for the admin form's <h2>. Index via
// epoch_to_index().
static const char *EPOCH_LABELS[EPOCH_COUNT] = {
    "Epoch -1 (WAP / WML)",
    "Epoch 0 (text browsers)",
    "Epoch 1 (early HTML, tables/font)",
    "Epoch 2 (HTML 3.2 tables)",
    "Epoch 3 (modern)",
};

// Every epoch, for site_settings_banner_page()/site_settings_footer_page():
// those exist everywhere from WML to modern.
static const int ALL_ASSET_EPOCHS[] = { -1, 0, 1, 2, 3 };
#define ALL_ASSET_EPOCHS_COUNT (sizeof(ALL_ASSET_EPOCHS) / sizeof(ALL_ASSET_EPOCHS[0]))

// Shared by site_settings_banner_page()/site_settings_footer_page(): `key`
// is the theme being edited (not necessarily the admin's own active theme -
// see theme-scoped-personalization-plan.md §4); `segment` is both the
// /dashboard/settings/themes/<key>/<segment>/<epoch> route and the
// theme-assets component the panel's upload widget uses ("banner"/"footer",
// see http_router.c's theme_assets_dir()); `epochs`/`epoch_count` is which
// epochs get a panel at all.
// The logo editor no longer uses this - see site_settings_logo_page(), which
// needs a different panel shape per epoch (text/image/radio) that this
// one-textarea-plus-upload shape can't express.
// One panel per epoch in `epochs` (a textarea plus the upload widget), for
// asset_page() and the theme customizer. Returns a malloc'd string, or NULL
// on a missing template / allocation failure.
static char *asset_panels(int epoch, const char *key, const char *segment,
                          char *const values[EPOCH_COUNT], const int *epochs, size_t epoch_count) {
    char *panel_tpl = load_template("dashboard/settings/settings-asset-panel_epoch%d.html", epoch);
    if (!panel_tpl) return NULL;

    char *panels = strdup("");
    for (size_t ei = 0; panels && ei < epoch_count; ei++) {
        int e = epochs[ei];
        int i = epoch_to_index(e);
        char epoch_str[4];
        snprintf(epoch_str, sizeof(epoch_str), "%d", e);

        char *encoded = html_encode_alloc(values[i] ? values[i] : "");
        char *panel = encoded ? render_template(panel_tpl, segment, epoch_str, key, EPOCH_LABELS[i],
                                                 key, segment, epoch_str, encoded)
                              : NULL;
        free(encoded);
        panels = panel ? str_append(panels, panel) : (free(panels), NULL);
        free(panel);
    }

    free(panel_tpl);
    return panels;
}

static char *asset_page(int epoch, const char *title, const char *key, const char *segment,
                         char *const values[EPOCH_COUNT], const int *epochs, size_t epoch_count) {
    char *page_tpl = load_template("dashboard/settings/settings-asset_epoch%d.html", epoch);
    char *panels   = asset_panels(epoch, key, segment, values, epochs, epoch_count);
    char *result   = page_tpl && panels ? render_template(page_tpl, title, key, panels) : NULL;
    free(page_tpl);
    free(panels);
    return result;
}

char *site_settings_banner_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Home banner", key, "banner", values,
                       ALL_ASSET_EPOCHS, ALL_ASSET_EPOCHS_COUNT);
}

char *site_settings_footer_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Footer", key, "footer", values,
                       ALL_ASSET_EPOCHS, ALL_ASSET_EPOCHS_COUNT);
}

// <option> list for the epoch-3 "Logo en fuente" font picker
// (settings-logo-panel_radio_epoch3.html): a hardcoded classic web-safe
// font set ("system:<name>"), followed by every font uploaded via
// /dashboard/settings/fonts ("uploaded:<name>") - the prefix is how the
// stored CmsLogoConfig.font value (and the <select>'s own submitted value)
// tells the two kinds apart, since both are plain font-family names on
// their own. Kept separate from build_font_options() (the unrelated
// Themes/Colors default-font picker - uploaded fonts only, no prefix),
// since mixing their option sets would be confusing if the two ever
// diverge; see cms_themes.h's CmsLogoConfig doc comment for how this
// picker's choice overrides that one when an epoch-3 logo is in
// LOGO_MODE_TEXT.
static const char *SYSTEM_FONTS[] = {
    "Arial", "Times New Roman", "Courier New", "Georgia",
    "Verdana", "Comic Sans MS", "Impact", "Trebuchet MS",
};
#define SYSTEM_FONTS_COUNT (sizeof(SYSTEM_FONTS) / sizeof(SYSTEM_FONTS[0]))

static char *build_logo_font_options(const char *selected) {
    char *options = strdup("");

    for (size_t i = 0; options && i < SYSTEM_FONTS_COUNT; i++) {
        char value[128];
        snprintf(value, sizeof(value), "system:%s", SYSTEM_FONTS[i]);
        const char *is_selected = (selected && strcmp(selected, value) == 0) ? " selected" : "";
        char *option = render_template("            <option value=\"%s\"%s>%s</option>\n",
                                        value, is_selected, SYSTEM_FONTS[i]);
        options = option ? str_append(options, option) : NULL;
        free(option);
    }

    CmsFont *fonts = NULL;
    size_t count = 0;
    cms_get_fonts(&fonts, &count);

    for (size_t i = 0; options && i < count; i++) {
        char value[320];
        snprintf(value, sizeof(value), "uploaded:%s", fonts[i].name);
        char *encoded_value = html_encode_alloc(value);
        char *encoded_name  = html_encode_alloc(fonts[i].name);
        if (!encoded_value || !encoded_name) {
            free(encoded_value);
            free(encoded_name);
            free(options);
            options = NULL;
            break;
        }

        const char *is_selected = (selected && strcmp(selected, value) == 0) ? " selected" : "";
        char *option = render_template("            <option value=\"%s\"%s>%s</option>\n",
                                        encoded_value, is_selected, encoded_name);
        free(encoded_value);
        free(encoded_name);
        options = option ? str_append(options, option) : NULL;
        free(option);
    }

    cms_fonts_free(fonts, count);
    return options ? options : strdup("");
}

// One epoch's panel for site_settings_logo_page(), picking the template
// variant that matches what that epoch offers - see CmsLogoConfig's doc
// comment in cms_themes.h for exactly which: epoch 0 -> text only,
// epoch -1/1/2 -> image only (navbar + footer), epoch 3 -> both, admin's
// choice via radio. `page_epoch` (always EPOCH_MODERN - the dashboard
// itself is always rendered modern, see http_router.c's require_admin_session
// gate) picks which on-disk template set to load; `logo_epoch` is the epoch
// this specific panel configures.
static char *logo_panel(int page_epoch, int logo_epoch, const char *key, const CmsLogoConfig *cfg) {
    int i = epoch_to_index(logo_epoch);
    char epoch_str[4];
    snprintf(epoch_str, sizeof(epoch_str), "%d", logo_epoch);

    char *encoded_text   = html_encode_alloc(cfg->text);
    char *encoded_navbar = html_encode_alloc(cfg->navbar_image);
    char *encoded_footer = html_encode_alloc(cfg->footer_image);

    char *result = NULL;
    if (!encoded_text || !encoded_navbar || !encoded_footer) goto cleanup;

    if (logo_epoch == 0) {
        char *tpl = load_template("dashboard/settings/settings-logo-panel_text-only_epoch%d.html", page_epoch);
        if (tpl) {
            result = render_template(tpl, "logo", epoch_str, key, EPOCH_LABELS[i],
                                      key, "logo", epoch_str,
                                      epoch_str, epoch_str, encoded_text);
        }
        free(tpl);
    } else if (logo_epoch == 3) {
        char *tpl = load_template("dashboard/settings/settings-logo-panel_radio_epoch%d.html", page_epoch);
        if (tpl) {
            int is_image = cfg->mode == LOGO_MODE_IMAGE;
            char *font_options = build_logo_font_options(cfg->font);
            if (font_options) {
                result = render_template(tpl, "logo", epoch_str, key, EPOCH_LABELS[i],
                                          key, "logo", epoch_str,
                                          is_image ? "" : "checked",
                                          is_image ? "checked" : "",
                                          is_image ? "hidden disabled" : "",
                                          epoch_str, epoch_str, encoded_text,
                                          epoch_str, epoch_str, font_options,
                                          is_image ? "" : "hidden disabled",
                                          encoded_navbar, encoded_footer);
                free(font_options);
            }
        }
        free(tpl);
    } else {
        char *tpl = load_template("dashboard/settings/settings-logo-panel_image-only_epoch%d.html", page_epoch);
        if (tpl) {
            result = render_template(tpl, "logo", epoch_str, key, EPOCH_LABELS[i],
                                      key, "logo", epoch_str,
                                      encoded_navbar, encoded_footer);
        }
        free(tpl);
    }

cleanup:
    free(encoded_text);
    free(encoded_navbar);
    free(encoded_footer);
    return result;
}

// Every epoch's logo panel (-1..3), for site_settings_logo_page() and the
// theme customizer. Returns a malloc'd string, or NULL on a missing template
// / allocation failure.
static char *logo_panels(int epoch, const char *key, const CmsLogoConfig configs[EPOCH_COUNT]) {
    char *panels = strdup("");
    for (int e = -1; panels && e <= 3; e++) {
        char *panel = logo_panel(epoch, e, key, &configs[epoch_to_index(e)]);
        panels = panel ? str_append(panels, panel) : (free(panels), NULL);
        free(panel);
    }
    return panels;
}

char *site_settings_logo_page(int epoch, const char *key, const CmsLogoConfig configs[EPOCH_COUNT]) {
    char *page_tpl = load_template("dashboard/settings/settings-logo_epoch%d.html", epoch);
    char *panels   = logo_panels(epoch, key, configs);
    char *result   = page_tpl && panels ? render_template(page_tpl, key, panels) : NULL;
    free(page_tpl);
    free(panels);
    return result;
}

char *site_settings_preview_page(int epoch) {
    return load_template("dashboard/settings/preview_epoch%d.html", epoch);
}

char *site_settings_css_page(int epoch, const char *key, ThemeCssSheet sheet, const char *custom,
                             const char *original) {
    char *page_tpl = load_template("dashboard/settings/settings-css_epoch%d.html", epoch);
    if (!page_tpl) return NULL;

    int is_customized = custom && custom[0];
    const char *status = is_customized
        ? "Customized - the rules below are appended after the theme's original stylesheet, "
          "so they win over it. \"Discard my changes\" removes them."
        : "Nothing customized yet - the site uses the theme's original stylesheet. Add only "
          "the rules you want to change; they are appended after the original, so they win "
          "over it.";

    char *encoded_custom   = html_encode_alloc(custom ? custom : "");
    char *encoded_original = html_encode_alloc(original ? original : "");
    char *result = NULL;
    if (encoded_custom && encoded_original) {
        static const char *ACTIVE_TAB = " boat-rudder-tabs__tab--active";
        int is_admin = sheet == THEME_CSS_ADMIN;
        result = render_template(page_tpl, key,
                                 is_admin ? "" : ACTIVE_TAB, is_admin ? ACTIVE_TAB : "",
                                 status, is_admin ? "admin-css" : "css", encoded_custom,
                                 is_customized ? "" : " hidden", encoded_original,
                                 cms_theme_css_file(sheet));
    }
    free(encoded_custom);
    free(encoded_original);
    free(page_tpl);
    return result;
}

// Splits a stored background value into the two form fields it renders as:
// an <input type="color"> value (opaque "#rrggbb") and an opacity-percentage
// <input type="range"> value, using cms_split_hex_alpha().
typedef struct {
    char rgb[8];
    char alpha[4];
} BgColorForm;

static BgColorForm split_bg(const char *stored) {
    BgColorForm f;
    int alpha_pct;
    cms_split_hex_alpha(stored, f.rgb, &alpha_pct);
    snprintf(f.alpha, sizeof(f.alpha), "%d", alpha_pct);
    return f;
}

// <option> list for the "Logo font" <select> (settings-themes-panel_epoch3.
// html), one per font uploaded via /dashboard/settings/fonts, `selected`
// (the theme's own cms_get_theme_logo_font()) marked - the "Default
// (Milonga)" option itself is a hardcoded literal in the template, not
// built here.
static char *build_font_options(const char *selected) {
    CmsFont *fonts = NULL;
    size_t count = 0;
    cms_get_fonts(&fonts, &count);

    char *options = strdup("");
    for (size_t i = 0; options && i < count; i++) {
        char *encoded = html_encode_alloc(fonts[i].name);
        if (!encoded) { free(options); options = NULL; break; }

        const char *is_selected = (selected && selected[0] && strcmp(selected, fonts[i].name) == 0)
            ? " selected" : "";
        char *option = render_template("            <option value=\"%s\"%s>%s</option>\n",
                                        encoded, is_selected, encoded);
        free(encoded);
        options = option ? str_append(options, option) : NULL;
        free(option);
    }

    cms_fonts_free(fonts, count);
    return options ? options : strdup("");
}

// "Set active" for a theme that isn't the active one, "" for the one that
// is; `return_to` is where the activate route sends the admin back.
static char *activate_control(const char *key, bool active, const char *return_to, int epoch) {
    if (active) return strdup("");
    char *tpl = load_template("dashboard/settings/settings-themes-activate_epoch%d.html", epoch);
    char *html = tpl ? render_template(tpl, key, return_to) : NULL;
    free(tpl);
    return html;
}

char *site_settings_themes_page(int epoch, const ThemeEntry *themes, size_t count) {
    char *page_tpl   = load_template("dashboard/settings/settings-themes_epoch%d.html", epoch);
    char *row_tpl    = load_template("dashboard/settings/settings-themes-row_epoch%d.html", epoch);
    char *active_tpl = load_template("dashboard/settings/settings-themes-active_epoch%d.html", epoch);

    char *rows = NULL;
    char *result = NULL;

    if (!page_tpl || !row_tpl || !active_tpl) goto cleanup;

    rows = strdup("");
    for (size_t i = 0; rows && i < count; i++) {
        char *status = themes[i].active ? strdup(active_tpl)
                                        : activate_control(themes[i].key, false, "/dashboard/settings/themes", epoch);
        char *row = status ? render_template(row_tpl, themes[i].key, status) : NULL;
        free(status);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) goto cleanup;

    result = render_template(page_tpl, rows);

cleanup:
    free(page_tpl);
    free(row_tpl);
    free(active_tpl);
    free(rows);
    return result;
}

// The theme's color form (settings-themes-panel_epoch3.html): one palette
// for every epoch, each field tagged with the epochs that use it. Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
static char *colors_panel(int epoch, const ThemeEntry *theme) {
    char *panel_tpl = load_template("dashboard/settings/settings-themes-panel_epoch%d.html", epoch);
    if (!panel_tpl) return NULL;

    const CmsThemeColors *c = &theme->colors;
    BgColorForm navbar_bg     = split_bg(c->navbar_background);
    BgColorForm body_bg       = split_bg(c->body_background);
    BgColorForm page_bg       = split_bg(c->page_content_background);
    BgColorForm home_bg       = split_bg(c->home_content_background);
    BgColorForm blog_item_bg  = split_bg(c->blog_list_item_background);
    BgColorForm footer_bg     = split_bg(c->footer_logo_background);

    char *logo_font = cms_get_theme_logo_font(theme->key);
    char *font_options = build_font_options(logo_font);
    free(logo_font);

    char *panel = font_options ? render_template(panel_tpl, theme->key,
                             navbar_bg.rgb, navbar_bg.alpha, c->navbar_menu_normal,
                             c->navbar_menu_hover, c->navbar_menu_active,
                             c->navbar_logo, font_options,
                             body_bg.rgb, body_bg.alpha, c->body_background_epoch1,
                             page_bg.rgb, page_bg.alpha,
                             c->body_text, c->body_text_muted, c->body_surface,
                             c->body_surface_raised, c->body_border,
                             c->link_normal, c->link_hover, c->link_visited, c->link_active,
                             home_bg.rgb, home_bg.alpha, c->home_content_text,
                             blog_item_bg.rgb, blog_item_bg.alpha, c->blog_list_item_border,
                             c->blog_list_item_author, c->blog_list_item_categories,
                             c->blog_list_item_categories_hover, c->blog_list_item_date,
                             c->table_header, c->table_border, c->table_row_a, c->table_row_b,
                             c->code_background, c->code_text, c->code_keyword,
                             c->code_string, c->code_comment, c->code_number,
                             c->code_variable, c->code_tag, c->code_line_number,
                             c->footer_logo, footer_bg.rgb, footer_bg.alpha)
                       : NULL;
    free(font_options);
    free(panel_tpl);
    return panel;
}

// The home blog's background images panel for epoch 2 and 3 (the only ones
// with them, see cms_themes.h): the list's and each item's. Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
static char *home_blog_panels(int epoch, const char *key, char *const backgrounds[EPOCH_COUNT],
                              char *const item_backgrounds[EPOCH_COUNT]) {
    char *panel_tpl = load_template("dashboard/settings/settings-home-blog-panel_epoch%d.html", epoch);
    if (!panel_tpl) return NULL;

    static const int EPOCHS[] = { EPOCH_MIDDLE, EPOCH_MODERN };
    char *panels = strdup("");
    for (size_t ei = 0; panels && ei < sizeof(EPOCHS) / sizeof(EPOCHS[0]); ei++) {
        int i = epoch_to_index(EPOCHS[ei]);
        char epoch_str[4];
        snprintf(epoch_str, sizeof(epoch_str), "%d", EPOCHS[ei]);

        char *list_bg = html_encode_alloc(backgrounds[i] ? backgrounds[i] : "");
        char *item_bg = html_encode_alloc(item_backgrounds[i] ? item_backgrounds[i] : "");
        char *panel = list_bg && item_bg ? render_template(panel_tpl, epoch_str, key, list_bg, item_bg)
                                         : NULL;
        free(list_bg);
        free(item_bg);
        panels = panel ? str_append(panels, panel) : (free(panels), NULL);
        free(panel);
    }

    free(panel_tpl);
    return panels;
}

char *site_settings_customize_page(int epoch, const ThemeCustomizeData *data) {
    const char *key = data->theme.key;
    char *page_tpl   = load_template("dashboard/settings/settings-customize_epoch%d.html", epoch);
    char *active_tpl = load_template("dashboard/settings/settings-themes-active_epoch%d.html", epoch);

    char return_to[128];
    snprintf(return_to, sizeof(return_to), "/dashboard/settings/themes/%s/customize", key);
    char *status  = data->theme.active ? (active_tpl ? strdup(active_tpl) : NULL)
                                       : activate_control(key, false, return_to, epoch);
    char *logo    = logo_panels(epoch, key, data->logo);
    char *banner  = asset_panels(epoch, key, "banner", data->banner, ALL_ASSET_EPOCHS,
                                 ALL_ASSET_EPOCHS_COUNT);
    char *blog    = home_blog_panels(epoch, key, data->home_blog_background,
                                     data->home_blog_item_background);
    char *footer  = asset_panels(epoch, key, "footer", data->footer, ALL_ASSET_EPOCHS,
                                 ALL_ASSET_EPOCHS_COUNT);
    char *colors  = colors_panel(epoch, &data->theme);
    char *custom   = html_encode_alloc(data->css_custom ? data->css_custom : "");
    char *original = html_encode_alloc(data->css_original ? data->css_original : "");

    char *result = NULL;
    if (page_tpl && status && logo && banner && blog && footer && colors && custom && original)
        result = render_template(page_tpl, key, status, logo, banner, blog, footer, colors,
                                 custom, original);

    free(page_tpl);
    free(active_tpl);
    free(status);
    free(logo);
    free(banner);
    free(blog);
    free(footer);
    free(colors);
    free(custom);
    free(original);
    return result;
}
