#include "menu.h"
#include "../../db/cms_languages.h"
#include "../../db/language_catalog.h"
#include "../../db/cms_menu.h"
#include "../../db/cms_site_settings.h"
#include "../../db/cms_themes.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_lang.h"
#include "../../utils/request_theme.h"
#include "../../utils/request_user.h"
#include "../../utils/template_utils.h"
#include "../../utils/theme_catalog.h"
#include <stdlib.h>
#include <string.h>

// Used when the `menu` collection is empty, unreachable, or mongodb is not
// ready, so the nav bar is never empty.
static const CmsMenuItem FALLBACK_ITEMS[] = {
    {.link = "/", .name = "Home"},
};

#define FALLBACK_ITEM_COUNT (sizeof(FALLBACK_ITEMS) / sizeof(FALLBACK_ITEMS[0]))

// Builds the language selector appended to the nav bar. Epoch 3 renders a
// drop-down whose entries link straight to /language/set; older epochs, where
// a drop-down cannot be relied on, link to the /language page instead. Both
// paths end at the same place - a plain GET that sets the cookie and bounces
// back - so switching language never needs JavaScript.
//
// Epoch 2's link (menu-lang_epoch2.html) is colored the same as the nav
// menu's own items (see the EPOCH_EARLY/EPOCH_MIDDLE block below), through
// the same {{COLOR_MENU_NORMAL}}/{{COLOR_MENU_HOVER}} tokens in
// layout_epoch2.html - but by `#id`, not `td a.class`: unlike the menu
// items, this link showed a *different* resting color depending on whether
// the reader had already visited /language before, meaning <body vlink=...>
// (visited) apparently outranks <body link=...> (unvisited) in whatever
// specificity this engine grants them, higher than the `td a.class`
// (0,1,2) that was already enough for the unvisited case. Rather than
// chase that number too, an ID selector sidesteps the whole question - it
// outranks any class/type-based rule unconditionally, so it doesn't matter
// what <body link>/<body vlink> are actually worth here. The theme
// selector (menu-theme_epoch2.html) shares the same fix, its own id.
//
// Returns a malloc'd string ("" when there is nothing to offer: fewer than two
// languages, or the templates are missing), never NULL unless allocation fails.
static char *language_selector(int epoch) {
    CmsLanguageItem *langs = NULL;
    size_t count = 0;
    if (mongodb_manager_is_ready()) cms_get_languages(&langs, &count);

    // A single language offers no choice; showing the control would be noise.
    if (count < 2) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    // The button is tight on space, so it shows the short form ("Esp"); the
    // list has room for the language's own name ("Espanol"), which is what a
    // reader looking for their language actually scans for.
    const char *active = request_lang();
    const char *active_abbr = language_catalog_abbr(active);

    // Come back to the page actually being read. `current_url` is the menu's
    // section (always "/blog" for an article), which would drop the reader on
    // the listing instead.
    char return_enc[1024];
    url_encode(return_enc, request_path(), sizeof(return_enc));

    char *tpl_path = generate_url_theme("menu/menu-lang_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    char *result = NULL;
    if (epoch >= EPOCH_MODERN) {
        char *item_path = generate_url_theme("menu/menu-lang-item_epoch%d.html", epoch);
        char *item_tpl = item_path ? read_file_to_string(item_path) : NULL;
        free(item_path);

        if (item_tpl) {
            char *items = strdup("");
            for (size_t i = 0; items && i < count; i++) {
                if (!langs[i].code) continue;
                const char *is_active = strcmp(langs[i].code, active) == 0
                    ? " boat-rudder-navbar-lang__item--active" : "";
                char *item = render_template(item_tpl, is_active, langs[i].code, return_enc,
                                              language_catalog_native(langs[i].code));
                items = item ? str_append(items, item) : NULL;
                free(item);
            }
            if (items) result = render_template(tpl, active_abbr, items);
            free(items);
            free(item_tpl);
        }
    } else {
        result = render_template(tpl, return_enc, active_abbr);
    }

    free(tpl);
    cms_languages_free(langs, count);
    return result ? result : strdup("");
}

// A theme switcher for the nav bar, "similar a la lista de idiomas" -
// mirrors language_selector() exactly: epoch 3 renders a drop-down whose
// entries link straight to /theme/set (sets the `theme` cookie, bounces
// back); epoch 1/2, where a drop-down cannot be relied on, get a plain
// link to the /theme page instead (theme_page.c), which itself mirrors
// /language's own epoch 1 (direct "<page>?theme=xx" link, no redirect -
// see theme_page.c) vs. epoch 2 (/theme/set) split. Epoch -1/0 still don't
// offer this: their look barely differs between themes today (colors
// already come from the same DB-backed palette regardless of which
// theme's markup is loaded - see theme-system-plan.md), so a page for a
// control that changes almost nothing there isn't worth it yet.
//
// Returns a malloc'd string ("" when there is nothing to offer: fewer than
// two themes, epoch -1/0, or the templates are missing), never NULL
// unless allocation fails.
static char *theme_selector(int epoch) {
    if (epoch < EPOCH_EARLY) return strdup("");

    char **keys = NULL;
    size_t count = 0;
    list_theme_keys(&keys, &count);

    // A single theme offers no choice; showing the control would be noise.
    if (count < 2) {
        free_theme_keys(keys, count);
        return strdup("");
    }

    const char *active = request_theme();

    char return_enc[1024];
    url_encode(return_enc, request_path(), sizeof(return_enc));

    char *tpl_path = generate_url_theme("menu/menu-theme_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        free_theme_keys(keys, count);
        return strdup("");
    }

    char *result = NULL;
    if (epoch >= EPOCH_MODERN) {
        char *item_path = generate_url_theme("menu/menu-theme-item_epoch%d.html", epoch);
        char *item_tpl = item_path ? read_file_to_string(item_path) : NULL;
        free(item_path);

        if (item_tpl) {
            char *items = strdup("");
            for (size_t i = 0; items && i < count; i++) {
                const char *is_active = strcmp(keys[i], active) == 0
                    ? " boat-rudder-navbar-theme__item--active" : "";
                char *item = render_template(item_tpl, is_active, keys[i], return_enc, keys[i]);
                items = item ? str_append(items, item) : NULL;
                free(item);
            }
            if (items) result = render_template(tpl, active, items);
            free(items);
            free(item_tpl);
        }
    } else {
        // menu-theme_epoch{1,2}.html link to /theme?return=..., a full page
        // listing every theme (theme_page.c) - same shape as
        // menu-lang_epoch{1,2}.html linking to /language. No stylesheet
        // here to capitalize the name the way epoch 3's button does (see
        // styles_epoch3.css), so it takes a capitalized copy directly.
        char *display = capitalize_first(active);
        result = render_template(tpl, return_enc, display ? display : active);
        free(display);
    }

    free(tpl);
    free_theme_keys(keys, count);
    return result ? result : strdup("");
}

// Epoch 3 only: a link to /dashboard carrying the signed-in user's display
// name. request_user_name() (per-thread, resolved once per request in
// http_router.c) is "" whenever there is no active session, which is what
// keeps this link out of the navbar entirely for every visitor who isn't
// logged in.
//
// Rendered twice, from two different one-`%s` templates sharing the same
// name: menu-user_epoch3.html sits in the fixed top-left corner (desktop)
// and menu-user-mobile_epoch3.html rides inside the hamburger dropdown as
// one more boat-rudder-navbar__menu-item (mobile) - styles_epoch3.css
// swaps which one is visible at the 800px breakpoint, since a signed-in
// admin still needs a way to reach /dashboard once the corner link is
// hidden. See user_menu_item()/user_menu_item_mobile() below.
//
// Returns a malloc'd string ("" when logged out, older epochs, or the
// template is missing), never NULL unless allocation fails.
static char *user_link(int epoch, const char *subpath_fmt) {
    const char *name = request_user_name();
    if (epoch < EPOCH_MODERN || !name[0]) return strdup("");

    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    if (!tpl) return strdup("");

    char *result = render_template(tpl, name);
    free(tpl);
    return result ? result : strdup("");
}

static char *user_menu_item(int epoch) {
    return user_link(epoch, "menu/menu-user_epoch%d.html");
}

static char *user_menu_item_mobile(int epoch) {
    return user_link(epoch, "menu/menu-user-mobile_epoch%d.html");
}

char *menu(const char *current_url, int epoch) {
    // WML: every page's first screen is one ~860-byte packet (see
    // wap_gateway/wbxml.h), and the full menu - logo, every section, the
    // language link with its return URL - took over half of it, leaving the
    // page's own content for screen 2. So pages carry a single "[Menu]" link
    // instead, to /menu, which shows the full menu (below) and a way back.
    if (epoch == EPOCH_WML && strcmp(request_path(), "/menu") != 0) {
        char *path = generate_url_theme("menu/menu-compact_epoch%d.html", epoch);
        char *tpl  = path ? read_file_to_string(path) : NULL;
        free(path);
        if (tpl) {
            char ret[1024];
            url_encode(ret, request_path(), sizeof(ret));
            char *compact = render_template(tpl, ret);
            free(tpl);
            return compact;
        }
    }

    char *menu_item_path    = generate_url_theme("menu/menu-item_epoch%d.html", epoch);
    char *selected_item_path = generate_url_theme("menu/menu-item-selected_epoch%d.html", epoch);
    char *separator_path    = generate_url_theme("menu/menu-item-separator_epoch%d.html", epoch);
    char *menu_path         = generate_url_theme("menu/menu_epoch%d.html", epoch);

    char *menu_item_tpl     = menu_item_path     ? read_file_to_string(menu_item_path)     : NULL;
    char *selected_item_tpl = selected_item_path ? read_file_to_string(selected_item_path) : NULL;
    char *separator         = separator_path     ? read_file_to_string(separator_path)     : NULL;
    char *menu_tpl          = menu_path          ? read_file_to_string(menu_path)          : NULL;

    free(menu_item_path);
    free(selected_item_path);
    free(separator_path);
    free(menu_path);

    char *items  = NULL;
    char *result = NULL;

    if (!menu_item_tpl || !separator || !menu_tpl) goto cleanup;
    if (!selected_item_tpl) selected_item_tpl = strdup(menu_item_tpl);

    CmsMenuItem *db_items = NULL;
    size_t db_count = 0;
    if (mongodb_manager_is_ready())
        cms_get_menu_items(request_lang(), &db_items, &db_count);

    const CmsMenuItem *menu_items = db_count > 0 ? db_items : FALLBACK_ITEMS;
    size_t item_count = db_count > 0 ? db_count : FALLBACK_ITEM_COUNT;

    items = strdup("");
    if (!items) {
        cms_menu_free(db_items, db_count);
        goto cleanup;
    }

    // Epoch 1 has no stylesheet at all, so its color has to be a real <font
    // color> attribute, substituted straight in here - see
    // menu-item(-selected)_epoch1.html. That used to also cover epoch 2 (no
    // stylesheet *it* could lean on either, historically), but its own
    // inline <style> block (layout_epoch2.html) does exist, so epoch 2's two
    // states are colored from there instead - #boat-rudder-navbar-menu-item
    // (-selected), via splice_retro_colors()'s {{COLOR_MENU_NORMAL}}/
    // {{COLOR_MENU_ACTIVE}}/{{COLOR_MENU_HOVER}} tokens - not a per-item
    // inline attribute.
    //
    // Several rounds of real-browser testing (IE5/Windows 3.11, the one
    // that keeps disagreeing with every plausible-looking CSS-specificity
    // fix) got this to an id instead of a class - condensed, since the
    // dead ends matter less than the pattern: an inline `style="color:..."`
    // per item broke hover outright (this engine won't let even
    // `!important` override an element's own inline style, unlike
    // IE5/Windows 95); a bare menu-item class selector (`.item { color }`)
    // fixed hover but lost the *resting* color to <body link="..."> outright
    // (specificity 0,1,0 - a spec-compliant engine should never let a
    // zero-specificity presentational hint win here, so this one plainly
    // doesn't follow that part of the spec); grouping in `:link`/`:visited`
    // pseudo-classes changed nothing (both grouped selectors and pairing
    // `:link`/`:visited` with a class are separately documented as
    // unreliable on real mid-90s engines, so that rule likely just got
    // dropped whole); a plain `a.item` (type+class,
    // 0,1,1) and a `<td>`-qualified `td a.item` (0,1,2)
    // both *still* lost on unselected items, even though the same
    // technique's doubled-up selected-state version won outright - meaning
    // whatever specificity this engine effectively grants <body link>/
    // <body vlink> is real, non-zero, and apparently higher for vlink
    // (visited) than for link (unvisited), high enough that no class-based
    // number chased here has cleared it reliably. An id selector sidesteps
    // needing to know that number at all - it outranks any class/type
    // combination unconditionally. Every *unselected* item shares one id
    // (`#boat-rudder-navbar-menu-item`) rather than each getting its own -
    // invalid HTML (ids must be unique), but real engines match `#id` by
    // plain attribute equality, not by first confirming uniqueness, so
    // every item with that id still gets colored; the one currently-selected
    // item gets the other id instead.
    //
    // The hover rule had to move from class to id too, one step behind: a
    // bare `.item:hover` (class+pseudo) stopped winning
    // the moment the resting rule became an id, since an id selector
    // outranks any number of classes regardless of pseudo-classes among
    // them - so hover is `#boat-rudder-navbar-menu-item:hover` now, id+
    // pseudo, matching the shared unselected-item id exactly like the
    // resting rule does. There's deliberately no
    // `#boat-rudder-navbar-menu-item-selected:hover` counterpart - hovering
    // the current page's own item just keeps its selected color, which is
    // the one part of this whole saga confirmed as *wanted* rather than
    // merely tolerated.
    //
    // An unselected epoch 2 item still needs *a* color statement of its own,
    // rather than just inheriting the theme's <body link="...">, since that
    // attribute now carries the separate "generic links" color
    // (cms_themes.h's link_normal), not navbar-menu-normal.
    int needs_color = (epoch == EPOCH_EARLY);
    CmsThemeColors colors;
    if (needs_color) cms_get_theme_colors(request_theme(), &colors);

    for (size_t i = 0; i < item_count; i++) {
        const char *sep = (i + 1 < item_count) ? separator : "";

        int is_selected = current_url && strcmp(current_url, menu_items[i].link) == 0;
        const char *tpl = is_selected ? selected_item_tpl : menu_item_tpl;
        char *item;
        if (needs_color) {
            item = render_template(tpl, menu_items[i].link,
                                    is_selected ? colors.navbar_menu_active : colors.navbar_menu_normal,
                                    menu_items[i].name, sep);
        } else {
            item = render_template(tpl, menu_items[i].link, menu_items[i].name, sep);
        }
        if (!item) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }

        items = str_append(items, item);
        free(item);
        if (!items) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }
    }

    cms_menu_free(db_items, db_count);

    // Home banner stands in for the logo on the home page, so the menu only
    // carries it elsewhere - the reader still needs a way back. Mirrors the
    // previous site. Decided from the request path, not `current_url`: the
    // latter is the menu *section* and is hardcoded to "/" by
    // buildPageWebSite(), which every login/dashboard/language page goes
    // through - they would all look like home.
    // cms_get_theme_logo_config() carries the structured per-epoch config
    // (site_settings_logo_page()); LOGO_MODE_UNSET (nothing saved through
    // that panel yet) falls back to cms_get_theme_logo()'s raw-markup field,
    // which itself falls back to the theme's own on-disk
    // menu-logo_epoch<N>.html (same convention as mainbanner()'s
    // cms_get_theme_banner()) - so a theme untouched by either panel, or
    // only ever edited through the old one, renders exactly as before.
    char *logo = strdup("");
    int at_home = strcmp(request_path(), "/") == 0;
    if ((epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE || epoch == EPOCH_WML ||
         epoch == EPOCH_PRESTANDARD) && !at_home) {
        CmsLogoConfig cfg;
        cms_get_theme_logo_config(request_theme(), epoch, &cfg);

        if (cfg.mode == LOGO_MODE_IMAGE && cfg.navbar_image[0]) {
            char *site_name = cms_get_site_name();
            char encoded_alt[512];
            html_encode(encoded_alt, site_name ? site_name : "", sizeof(encoded_alt));
            free(site_name);
            char *img = render_template("<img src=\"/themes/%s/assets/menu/epoch%d/%s\" alt=\"%s\">",
                                         request_theme(), epoch, cfg.navbar_image, encoded_alt);
            if (img) { free(logo); logo = img; }
        } else if (cfg.mode == LOGO_MODE_TEXT) {
            char *site_name = cms_get_site_name();
            const char *text = cfg.text[0] ? cfg.text : (site_name ? site_name : "");
            char encoded[1600];
            html_encode(encoded, text, sizeof(encoded));
            free(logo);
            logo = strdup(encoded);
            free(site_name);
        } else {
            char *logo_html = cms_get_theme_logo(request_theme(), epoch);
            if (logo_html && logo_html[0]) {
                free(logo);
                logo = logo_html;
            } else {
                free(logo_html);
            }
        }
    }

    char *lang_html = language_selector(epoch);
    // Epoch 3 draws its own title in the nav bar; by default that's the
    // personalizable site name, but the logo panel
    // (site_settings_logo_page()) lets an admin switch it to either custom
    // text (in a chosen font - see page_layout.c's build_logo_font_css())
    // or an uploaded image, same LOGO_MODE_TEXT/LOGO_MODE_IMAGE choice as
    // every other epoch above. LOGO_MODE_UNSET falls back to the pre-config
    // raw-markup override (cms_get_theme_logo()), then to site_name - same
    // chain as before this struct existed.
    if (lang_html) {
        if (epoch >= EPOCH_MODERN) {
            CmsLogoConfig cfg3;
            cms_get_theme_logo_config(request_theme(), epoch, &cfg3);

            char *site_name = cms_get_site_name();
            char *custom_logo = NULL;
            char *title_html = NULL;

            if (cfg3.mode == LOGO_MODE_IMAGE && cfg3.navbar_image[0]) {
                char encoded_alt[512];
                html_encode(encoded_alt, site_name ? site_name : "", sizeof(encoded_alt));
                title_html = render_template("<img src=\"/themes/%s/assets/menu/epoch%d/%s\" alt=\"%s\">",
                                              request_theme(), epoch, cfg3.navbar_image, encoded_alt);
            } else if (cfg3.mode == LOGO_MODE_TEXT) {
                const char *text = cfg3.text[0] ? cfg3.text : (site_name ? site_name : "");
                char encoded[1600];
                html_encode(encoded, text, sizeof(encoded));
                title_html = strdup(encoded);
            } else {
                custom_logo = cms_get_theme_logo(request_theme(), epoch);
                title_html = (custom_logo && custom_logo[0]) ? strdup(custom_logo) : strdup(site_name ? site_name : "");
            }

            char *user_html = user_menu_item(epoch);
            char *user_html_mobile = user_menu_item_mobile(epoch);

            if (user_html_mobile) items = str_append(items, user_html_mobile);
            else { free(items); items = NULL; }
            free(user_html_mobile);

            char *theme_html = theme_selector(epoch);

            result = (title_html && user_html && items && theme_html)
                ? render_template(menu_tpl, user_html, title_html, items, theme_html, lang_html)
                : NULL;
            free(site_name);
            free(custom_logo);
            free(title_html);
            free(user_html);
            free(theme_html);
        } else {
            // No new %s slot on menu_epoch{1,2}.html's own container for
            // this - menu-theme_epoch{1,2}.html is a self-contained,
            // already-spaced snippet (mirrors menu-lang_epoch{1,2}.html's
            // own leading spacer/cell), so it is simply appended onto the
            // same lang_html argument the container already takes,
            // reassigning lang_html so the one free() below still covers
            // whichever buffer ends up here.
            char *theme_html = theme_selector(epoch);
            lang_html = theme_html ? str_append(lang_html, theme_html) : NULL;
            free(theme_html);

            if (epoch == EPOCH_MIDDLE) {
                // menu_epoch2.html's own <table> takes the theme's navbar
                // background directly (no CSS var to lean on, same as every
                // other epoch 1/2 color) - it was rendering unstyled before,
                // just inheriting the page's own body background regardless
                // of what the "Navbar > Background" setting said.
                // navbar-background may carry an alpha byte for epoch 3's
                // CSS background-color, which a plain HTML bgcolor attribute
                // has no notion of (same fix as the table block and the
                // blog-list item card), so this strips it down first.
                CmsThemeColors navbar_colors;
                cms_get_theme_colors(request_theme(), &navbar_colors);
                char navbar_bg[8];
                int unused_alpha;
                cms_split_hex_alpha(navbar_colors.navbar_background, navbar_bg, &unused_alpha);
                result = lang_html ? render_template(menu_tpl, navbar_bg, logo, items, lang_html) : NULL;
            } else {
                result = lang_html ? render_template(menu_tpl, logo, items, lang_html) : NULL;
            }
        }
    }
    free(lang_html);
    free(logo);

cleanup:
    free(menu_item_tpl);
    free(selected_item_tpl);
    free(separator);
    free(menu_tpl);
    free(items);
    return result;
}
