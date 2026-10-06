#ifndef ANALYTICS_CHARTS_H
#define ANALYTICS_CHARTS_H

#include <stddef.h>

// Server-side SVG charts for the dashboard's analytics (no JavaScript, no
// chart library). Every builder returns a malloc'd HTML fragment - an inline
// <svg> plus whatever HTML it needs around it - or NULL on allocation
// failure; with nothing to plot it returns the same "No data" paragraph the
// tables use. Labels are HTML-encoded here, so callers pass raw strings.
//
// Colors never appear in the markup: marks carry classes
// (boat-rudder__chart__series--<n>, --accent, --muted) that each theme's
// styles_epoch3.css colors, so light and dark get their own validated steps.
// Hover detail is each mark's native SVG <title> tooltip.

// One part of a whole, for analytics_chart_donut(). `series` (1-5) picks the
// categorical color class and should follow the entity, not its rank (the
// epochs always use 1 = WML ... 5 = epoch 3).
typedef struct {
    const char *label;
    int value;
    int series;
} ChartSlice;

// A donut of `slices` (zero-valued ones are skipped in the ring but kept in
// the legend) with the total and `center_label` in the hole, followed by a
// legend listing every slice's value and share - the direct labels that
// carry identity without relying on color alone.
char *analytics_chart_donut(const ChartSlice *slices, size_t count,
                            const char *center_label, const char *aria_label);

// One ranked row for analytics_chart_hbars().
typedef struct {
    const char *label;
    int value;
    const char *href;   // optional link for the label (NULL for none)
    const char *detail; // optional extra tooltip text, e.g. "3 today" (NULL for none)
} ChartBar;

// Horizontal bars, one per row in the given order (callers pass them
// already ranked), each scaled to the largest value: label on the left,
// value on the right, bar underneath. Single hue - it compares magnitudes.
char *analytics_chart_hbars(const ChartBar *bars, size_t count, const char *aria_label);

// One column for analytics_chart_columns().
typedef struct {
    const char *label;   // axis label under the column ("Mon 05", "Today")
    const char *tooltip; // full description for the <title> ("2026-10-05: 12 visits")
    int value;
    int highlight;       // 1 = the accent color (and always value-labeled), 0 = muted
} ChartColumn;

// Vertical columns in the given order sharing a zero baseline, scaled to the
// largest value. Only highlighted columns and the maximum carry a value
// label; the rest show theirs on hover.
char *analytics_chart_columns(const ChartColumn *columns, size_t count, const char *aria_label);

#endif // ANALYTICS_CHARTS_H
