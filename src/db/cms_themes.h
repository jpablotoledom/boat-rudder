#ifndef CMS_THEMES_H
#define CMS_THEMES_H

#include "../utils/detect_epoch.h"
#include <stddef.h>

// A theme's DB-editable color tokens - the 13 variables defined in the
// project's Figma file ("Color palette Dark"/"Color palette Light"), plus
// footer_logo/footer_logo_background (epoch 3 only - the ".boat-rudder__
// footer-title" bar has no epoch 1/2 equivalent to substitute into, those
// epochs render the footer as plain images), one
// shared palette for every epoch that has a concept of color at all (1, 2
// and 3), not split per epoch. How each value actually reaches the page
// differs by epoch, since epoch 1/2 have no CSS custom properties (epoch 1:
// no CSS at all; epoch 2: a tiny inline <style> for typography only,
// predating CSS3 variables) - epoch 3 gets these as --br-color-<name> CSS
// variables (names hyphenated to match the Figma variable exactly, e.g.
// --br-color-navbar-background), epoch 1/2 get a handful of them (there is
// no epoch 1/2 concept of "hover", for instance) substituted straight into
// HTML attributes (bgcolor/text/link/vlink/bordercolor/<font color>) - see
// page_layout.c's splice_retro_colors() and blog_list.c/category_tags.c for
// exactly which. Unlike site_settings, `themes` holds one *sparse* document
// per theme key that an admin has actually customized from
// /dashboard/settings/themes - a theme with no document just keeps its own
// Figma-defined defaults (see cms_themes.c's THEME_DEFAULTS).
// The 5 *_background fields below carry an optional alpha channel -
// "#rrggbb" (opaque, the historical format) or "#rrggbbaa" - since epoch 3's
// CSS custom properties happily accept 8-digit hex and every background use
// site is a CSS background-color; epoch 1/2 have no alpha concept (plain
// bgcolor attributes), so this only ever matters for epoch 3. The extra 2
// bytes in these fields' size (10 vs 8) is exactly room for "aa". The admin
// form (settings-themes-panel_epoch3.html) splits/joins this with a
// separate opacity slider - see site_settings_admin.c's hex helpers.
typedef struct {
    char navbar_background[10];         // "#rrggbb[aa]" - navbar container background
    char navbar_menu_normal[8];         // menu item link color, unvisited/no interaction
    char navbar_menu_hover[8];          // menu item link color, :hover
    char navbar_menu_active[8];         // menu item link color, current page
    char navbar_logo[8];                // site name/logo text color
    char body_background[10];           // "#rrggbb[aa]" - page body background
    char home_content_background[10];   // "#rrggbb[aa]" - "Welcome" home-content section background
    char home_content_text[8];          // home-content section text
    char blog_list_item_background[10]; // "#rrggbb[aa]" - each blog listing card's background
    char blog_list_item_border[8];      // blog listing card border (epoch 1 has no box to border)
    char blog_list_item_author[8];      // blog listing card byline author name
    char blog_list_item_categories[8];  // category tag color/background
    char blog_list_item_date[8];        // blog listing card byline date
    char footer_logo[8];                // footer bar "Boat Rudder" text color (epoch 3 only)
    char footer_logo_background[10];    // "#rrggbb[aa]" - footer bar background behind that text (epoch 3 only)
    char body_background_epoch1[8];     // page body background, epoch 1 only - see below
} CmsThemeColors;

// body_background_epoch1 is the one deliberate exception to "one shared
// palette, not a separate one per epoch": a color picked freely can fall
// outside what a real epoch-1-era display can show as a flat fill - shown
// on an indexed-color (e.g. 8-bit/256-color) screen, an unmatched color
// gets dithered into a pattern of pixels instead of rendering solid (seen
// live comparing NCSA Mosaic against Internet Explorer 3 on the same
// theme). This lets epoch 1's <body bgcolor> take an independent value -
// normally one of the 16 VGA-safe colors the admin form's palette offers,
// guaranteed flat on any indexed display - without touching body-background
// itself, which epoch 2/3 keep using unchanged. Defaults to the same value
// as body_background (see cms_themes.c's THEME_DEFAULTS), so an unedited
// theme renders identically to before this field existed.

// db.themes.findOne({key}). Fills `out` with the stored colors, or with
// epoch 3's own hardcoded styles_epoch3.css values (see cms_themes.c) if
// no document exists for `key` or mongodb is unreachable - a theme with no
// saved colors must render exactly as it does today. Every value applies
// identically to epoch 1/2/3 rendering (see the type's doc above), so
// there is only ever one default set, not one per epoch: epoch 1/2's
// fresh-install appearance already differs slightly from their own prior
// hardcoded colors as a result (they previously had their own distinct
// muted palette; unifying the settings means adopting epoch 3's) - the
// disclosed, accepted trade-off of "one control affects every epoch it
// can" instead of duplicating the settings per epoch. Always returns 1.
int cms_get_theme_colors(const char *key, CmsThemeColors *out);

// db.themes.updateOne({key}, {$set: {colors}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_theme_colors(const char *key, const CmsThemeColors *colors);

// Home banner and footer, one raw-markup value per epoch, now scoped to
// the theme they visually belong with instead of site-wide - see
// theme-scoped-personalization-plan.md. "" (unset) falls back to that same
// theme's own on-disk file (mainbanner/mainbanner_epoch<N>.html /
// layout/footer_epoch<N>.html via generate_url_theme()), so a theme with
// no saved banner/footer renders exactly as it does today.

// Returns a malloc'd string, never NULL: the DB value for `epoch` if
// non-empty, otherwise that theme's on-disk fallback. Used by
// mainbanner()/page_layout_wrap() with key = request_theme(), so a
// banner/footer follows the same per-visitor precedence colors already do.
char *cms_get_theme_banner(const char *key, int epoch);
char *cms_get_theme_footer(const char *key, int epoch);

// The navbar logo, same "" (unset) falls back to that theme's own on-disk
// file (menu/menu-logo_epoch<N>.html via generate_url_theme()) convention -
// but only meaningful for epoch -1/1/2, the epochs that actually render an
// <img> logo (epoch 0 has no logo at all; epoch 3's logo is text with its
// own font picker instead - see cms_get_theme_logo_font()). Callers outside
// that range still work (an empty on-disk fallback), there's just nothing
// to show.
//
// This raw-markup field predates the structured per-epoch logo config below
// (cms_get_theme_logo_config()) - kept as-is, unused by new callers, purely
// as that function's backward-compat fallback for a theme that saved a logo
// before the structured config existed.
char *cms_get_theme_logo(const char *key, int epoch);

// The *stored* value only ("" if unset - not file-resolved), one per
// epoch (index via epoch_to_index()), for the admin form: it needs to
// tell "nothing saved" apart from "saved text that happens to equal the
// file". out_values is caller-allocated with EPOCH_COUNT entries, each
// filled with a malloc'd string the caller must free.
void cms_get_theme_banner_values(const char *key, char *out_values[EPOCH_COUNT]);
void cms_get_theme_footer_values(const char *key, char *out_values[EPOCH_COUNT]);

// db.themes.updateOne({key}, {$set: {"banner_html.<field for epoch>": html}},
// {upsert: true}). Returns 0 on success, -1 if epoch is outside -1..3, on
// a DB error, or if mongodb is not ready. An empty `html` clears that
// epoch back to the theme's on-disk default.
int cms_update_theme_banner(const char *key, int epoch, const char *html);
int cms_update_theme_footer(const char *key, int epoch, const char *html);
int cms_update_theme_logo(const char *key, int epoch, const char *html);

// The theme's chosen font-family for the epoch 3 navbar logo text (the
// site name) - "" (unset) keeps the hardcoded default (Milonga - see
// styles_epoch3.css). Fonts themselves are uploaded/managed globally via
// /dashboard/settings/fonts (see cms_fonts.h), not scoped per theme; each
// theme only stores the *name* of the one it picked. Always returns a
// malloc'd string, "" on a DB error or if mongodb is not ready.
char *cms_get_theme_logo_font(const char *key);

// db.themes.updateOne({key}, {$set: {logo_font: font_name}}, {upsert: true}).
// An empty `font_name` clears the override back to the hardcoded default.
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_theme_logo_font(const char *key, const char *font_name);

// Structured per-epoch logo configuration - the "Logo" panel under
// /dashboard/settings/themes/<key>/logo, one CmsLogoConfig per epoch
// (-1..3), replacing the old single raw-markup logo_html field above for
// any epoch that has been saved through the new panel:
//   epoch -1 (WAP/WML): LOGO_MODE_IMAGE only - navbar_image/footer_image
//     are WBMP filenames (see image_convert_to_wbmp(), called at upload
//     time by the theme-assets upload route before this struct is ever
//     saved - by the time it lands here the file is already WBMP).
//   epoch 0 (text browsers): LOGO_MODE_TEXT only - `text` is shown as
//     plain text where the nav bar has no room for an image at all.
//   epoch 1/2: LOGO_MODE_IMAGE only - navbar_image/footer_image are
//     filenames under html/themes/<key>/assets/menu/epoch<N>/.
//   epoch 3: either mode, admin's choice (a radio button in the panel).
//     LOGO_MODE_TEXT: `text` (falls back to the site name if empty) shown
//     in the font named by `font` - "system:<name>" for a hardcoded
//     web-safe font (see site_settings_admin.c's SYSTEM_FONTS) or
//     "uploaded:<name>" for one from /dashboard/settings/fonts
//     (cms_get_fonts()); this *overrides*, for this theme, the separate
//     default font picker on the Themes/Colors panel
//     (cms_get_theme_logo_font()) whenever mode is TEXT - that picker only
//     still applies when this one is unset/IMAGE. LOGO_MODE_IMAGE: same
//     navbar_image/footer_image fields as epoch 1/2.
typedef enum {
    LOGO_MODE_UNSET = 0, // nothing saved via the new panel - caller falls
                         // back to cms_get_theme_logo()'s raw-markup field
    LOGO_MODE_TEXT,
    LOGO_MODE_IMAGE,
} CmsLogoMode;

typedef struct {
    CmsLogoMode mode;
    char text[256];          // epoch 0, or epoch 3 in LOGO_MODE_TEXT
    char font[128];          // epoch 3 in LOGO_MODE_TEXT only: "system:<name>" / "uploaded:<name>"
    char navbar_image[256];  // filename under assets/menu/epoch<N>/ (LOGO_MODE_IMAGE)
    char footer_image[256];  // filename under assets/menu/epoch<N>/ (LOGO_MODE_IMAGE)
} CmsLogoConfig;

// db.themes.findOne({key}).logo.<epoch field>. Fills `out` with the stored
// structured config for `epoch`, or `out->mode = LOGO_MODE_UNSET` (every
// other field "") if nothing has been saved there yet - callers should then
// fall back to cms_get_theme_logo(key, epoch)'s raw-markup value (which
// itself falls back further, to the theme's on-disk menu-logo_epoch<N>.html
// - see that function's own doc comment), so a theme that predates this
// struct, or one edited through the old panel, keeps rendering exactly as
// before. Returns 1 if epoch is in -1..3, 0 (out untouched) otherwise.
int cms_get_theme_logo_config(const char *key, int epoch, CmsLogoConfig *out);

// db.themes.updateOne({key}, {$set: {"logo.<field for epoch>": {...}}},
// {upsert: true}). Returns 0 on success, -1 if epoch is outside -1..3, on a
// DB error, or if mongodb is not ready.
int cms_update_theme_logo_config(const char *key, int epoch, const CmsLogoConfig *cfg);

// The theme's epoch 3 stylesheet (styles_epoch3.css) - "" (DB unset) falls
// back to that theme's own on-disk html/themes/<key>/styles_epoch3.css, the
// file every theme ships with. Lets an admin fully rewrite a theme's CSS
// from /dashboard/settings/themes/<key>/css while the shipped file stays
// the one-click "Restore original" target (see cms_update_theme_css()).
// Returns a malloc'd string, never NULL unless allocation fails.
char *cms_get_theme_css(const char *key);

// The *stored* override only ("" if unset - not file-resolved), for the
// editor form: tells "nothing saved" apart from "saved text matching the
// file".
char *cms_get_theme_css_value(const char *key);

// db.themes.updateOne({key}, {$set: {css_epoch3: css}}, {upsert: true}). An
// empty `css` clears the override back to the on-disk original - this is
// what the editor's "Restore original" button submits. Returns 0 on
// success, -1 on a DB error or if mongodb is not ready.
int cms_update_theme_css(const char *key, const char *css);

// Splits a stored "#rrggbb" or "#rrggbbaa" background value into its opaque
// 7-char hex ("#rrggbb", for an <input type="color"> value - that control
// has no alpha concept) and an opacity percentage 0-100 (100 when `stored`
// carries no alpha byte, or isn't a recognizable hex color at all).
// rgb_out must be >= 8 bytes.
void cms_split_hex_alpha(const char *stored, char *rgb_out, int *alpha_pct_out);

// The inverse of cms_split_hex_alpha(): joins a "#rrggbb" color and an
// opacity percentage (clamped to 0-100) back into a stored background
// value. Emits plain "#rrggbb" for alpha_pct >= 100 (keeps fully-opaque
// values in the historical 7-char format) or "#rrggbbaa" otherwise.
// out_size must be >= 10.
void cms_join_hex_alpha(const char *rgb_hex, int alpha_pct, char *out, size_t out_size);

#endif // CMS_THEMES_H
