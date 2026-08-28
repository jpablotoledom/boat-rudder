#ifndef CMS_THEMES_H
#define CMS_THEMES_H

// A theme's small set of DB-editable color tokens - one shared palette for
// every epoch that has a concept of color at all (1, 2 and 3), not split
// per epoch: an admin picks one "Accent" color and it applies everywhere
// that reads it, not just epoch 3. How each value actually reaches the
// page differs by epoch, since epoch 1/2 have no CSS custom properties
// (epoch 1: no CSS at all; epoch 2: a tiny inline <style> for typography
// only, predating CSS3 variables) - epoch 3 gets these as
// --br-color-<name> CSS variables, epoch 1/2 get them substituted straight
// into HTML attributes (bgcolor/text/link/vlink/bordercolor/<font color>).
// Unlike site_settings, `themes` holds one *sparse* document per theme key
// that an admin has actually customized from /dashboard/settings/themes -
// a theme with no document just keeps every epoch's own hardcoded colors,
// same as before this feature existed.
typedef struct {
    char background[8]; // "#rrggbb" - body background
    char text[8];        // body text
    char accent[8];       // links
    char author[8];        // home-blog byline author name
    char date[8];            // home-blog byline date
    char category[8];         // category tag color/background
    char border[8];            // home-blog item border (epoch 1 has no box to border)
} CmsThemeColors;

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

#endif // CMS_THEMES_H
