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
// see theme-scoped-personalization-plan.md §4); `settings_segment` is the
// /dashboard/settings/themes/<key>/<segment>/<epoch> route
// ("banner"/"footer"); `asset_component` is the theme-assets directory name
// under html/themes/<key>/assets/ ("mainbanner"/"footer") - differs from its
// own segment name because the on-disk directory predates this feature and
// keeps its name; `epochs`/`epoch_count` is which epochs get a panel at all.
// The logo editor no longer uses this - see site_settings_logo_page(), which
// needs a different panel shape per epoch (text/image/radio) that this
// one-textarea-plus-upload shape can't express.
static char *asset_page(int epoch, const char *title, const char *key, const char *settings_segment,
                         const char *asset_component, char *const values[EPOCH_COUNT],
                         const int *epochs, size_t epoch_count) {
    char *page_tpl  = load_template("dashboard/settings/settings-asset_epoch%d.html", epoch);
    char *panel_tpl = load_template("dashboard/settings/settings-asset-panel_epoch%d.html", epoch);

    char *panels = NULL;
    char *result = NULL;

    if (!page_tpl || !panel_tpl) goto cleanup;

    panels = strdup("");
    for (size_t ei = 0; panels && ei < epoch_count; ei++) {
        int e = epochs[ei];
        int i = epoch_to_index(e);
        char epoch_str[4];
        snprintf(epoch_str, sizeof(epoch_str), "%d", e);

        char *encoded = html_encode_alloc(values[i] ? values[i] : "");
        if (!encoded) {
            free(panels);
            panels = NULL;
            break;
        }

        char *panel = render_template(panel_tpl, asset_component, epoch_str, key, EPOCH_LABELS[i],
                                       key, settings_segment, epoch_str, encoded);
        free(encoded);
        if (!panel) {
            free(panels);
            panels = NULL;
            break;
        }

        panels = str_append(panels, panel);
        free(panel);
    }
    if (!panels) goto cleanup;

    result = render_template(page_tpl, title, key, panels);

cleanup:
    free(page_tpl);
    free(panel_tpl);
    free(panels);
    return result;
}

char *site_settings_banner_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Home banner", key, "banner", "mainbanner", values,
                       ALL_ASSET_EPOCHS, ALL_ASSET_EPOCHS_COUNT);
}

char *site_settings_footer_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Footer", key, "footer", "footer", values,
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
            result = render_template(tpl, "menu", epoch_str, key, EPOCH_LABELS[i],
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
                result = render_template(tpl, "menu", epoch_str, key, EPOCH_LABELS[i],
                                          key, "logo", epoch_str,
                                          is_image ? "" : "checked",
                                          is_image ? "checked" : "",
                                          is_image ? "hidden disabled" : "",
                                          epoch_str, epoch_str, encoded_text,
                                          epoch_str, epoch_str, font_options,
                                          is_image ? "" : "hidden disabled",
                                          encoded_navbar, encoded_navbar,
                                          encoded_footer, encoded_footer);
                free(font_options);
            }
        }
        free(tpl);
    } else {
        char *tpl = load_template("dashboard/settings/settings-logo-panel_image-only_epoch%d.html", page_epoch);
        if (tpl) {
            result = render_template(tpl, "menu", epoch_str, key, EPOCH_LABELS[i],
                                      key, "logo", epoch_str,
                                      encoded_navbar, encoded_navbar,
                                      encoded_footer, encoded_footer);
        }
        free(tpl);
    }

cleanup:
    free(encoded_text);
    free(encoded_navbar);
    free(encoded_footer);
    return result;
}

char *site_settings_logo_page(int epoch, const char *key, const CmsLogoConfig configs[EPOCH_COUNT]) {
    char *page_tpl = load_template("dashboard/settings/settings-logo_epoch%d.html", epoch);
    if (!page_tpl) return NULL;

    static const int LOGO_EPOCHS[] = { -1, 0, 1, 2, 3 };
    char *panels = strdup("");
    for (size_t ei = 0; panels && ei < sizeof(LOGO_EPOCHS) / sizeof(LOGO_EPOCHS[0]); ei++) {
        int e = LOGO_EPOCHS[ei];
        char *panel = logo_panel(epoch, e, key, &configs[epoch_to_index(e)]);
        if (!panel) {
            free(panels);
            panels = NULL;
            break;
        }
        panels = str_append(panels, panel);
        free(panel);
    }

    char *result = panels ? render_template(page_tpl, key, panels) : NULL;
    free(page_tpl);
    free(panels);
    return result;
}

char *site_settings_preview_page(int epoch) {
    return load_template("dashboard/settings/preview_epoch%d.html", epoch);
}

char *site_settings_css_page(int epoch, const char *key, const char *value) {
    char *page_tpl = load_template("dashboard/settings/settings-css_epoch%d.html", epoch);
    if (!page_tpl) return NULL;

    char *stored = cms_get_theme_css_value(key);
    const char *status = stored[0]
        ? "Customized - showing your saved override below. \"Restore original\" discards it."
        : "Showing the theme's original stylesheet - nothing customized yet.";
    int is_customized = stored[0] != '\0';
    free(stored);

    char *encoded_css = html_encode_alloc(value ? value : "");
    if (!encoded_css) {
        free(page_tpl);
        return NULL;
    }

    char *result = render_template(page_tpl, key, status, key, encoded_css,
                                    key, is_customized ? "" : " hidden");
    free(encoded_css);
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

char *site_settings_themes_page(int epoch, const ThemeEntry *themes, size_t count) {
    char *page_tpl     = load_template("dashboard/settings/settings-themes_epoch%d.html", epoch);
    char *panel_tpl    = load_template("dashboard/settings/settings-themes-panel_epoch%d.html", epoch);
    char *activate_tpl = load_template("dashboard/settings/settings-themes-activate_epoch%d.html", epoch);

    char *panels = NULL;
    char *result = NULL;

    if (!page_tpl || !panel_tpl || !activate_tpl) goto cleanup;

    panels = strdup("");
    for (size_t i = 0; panels && i < count; i++) {
        char *activate = themes[i].active ? strdup("") : render_template(activate_tpl, themes[i].key);
        if (!activate) {
            free(panels);
            panels = NULL;
            break;
        }

        const CmsThemeColors *c = &themes[i].colors;
        BgColorForm navbar_bg     = split_bg(c->navbar_background);
        BgColorForm body_bg       = split_bg(c->body_background);
        BgColorForm home_bg       = split_bg(c->home_content_background);
        BgColorForm blog_item_bg  = split_bg(c->blog_list_item_background);
        BgColorForm footer_bg     = split_bg(c->footer_logo_background);

        char *logo_font = cms_get_theme_logo_font(themes[i].key);
        char *font_options = build_font_options(logo_font);
        free(logo_font);
        if (!font_options) { free(activate); free(panels); panels = NULL; break; }

        char *panel = render_template(panel_tpl, themes[i].key,
                                       themes[i].active ? " (active)" : "", activate,
                                       themes[i].key,
                                       navbar_bg.rgb, navbar_bg.alpha, c->navbar_menu_normal,
                                       c->navbar_menu_hover, c->navbar_menu_active,
                                       c->navbar_logo, font_options,
                                       body_bg.rgb, body_bg.alpha, c->body_background_epoch1,
                                       home_bg.rgb, home_bg.alpha, c->home_content_text,
                                       blog_item_bg.rgb, blog_item_bg.alpha, c->blog_list_item_border,
                                       c->blog_list_item_author, c->blog_list_item_categories,
                                       c->blog_list_item_date,
                                       c->footer_logo, footer_bg.rgb, footer_bg.alpha,
                                       themes[i].key, themes[i].key, themes[i].key, themes[i].key);
        free(font_options);
        free(activate);
        if (!panel) {
            free(panels);
            panels = NULL;
            break;
        }

        panels = str_append(panels, panel);
        free(panel);
    }
    if (!panels) goto cleanup;

    result = render_template(page_tpl, panels);

cleanup:
    free(page_tpl);
    free(panel_tpl);
    free(activate_tpl);
    free(panels);
    return result;
}
