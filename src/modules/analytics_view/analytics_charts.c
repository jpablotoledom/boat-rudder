#include "analytics_charts.h"
#include "../../utils/http_utils.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_DATA "<p class=\"boat-rudder__dashboard__empty\">No data</p>"

// Ring geometry, in the donut's own 120x120 viewBox. pathLength="100" on each
// circle lets the dash lengths below be plain percentages of the total.
#define DONUT_SIZE   120
#define DONUT_RADIUS 45
#define DONUT_STROKE 18
// Surface-colored gap between adjacent segments, in pathLength units (~1.5px).
#define DONUT_GAP    0.6

// hbars: one row = a label line, then the bar.
#define HBAR_ROW_HEIGHT 34
#define HBAR_TEXT_Y     13
#define HBAR_BAR_Y      19
#define HBAR_BAR_HEIGHT 8
// Labels longer than this many characters are cut with an ellipsis (the
// <title> keeps the full text): SVG text can't wrap or ellipsize itself.
#define HBAR_LABEL_MAX  26

// columns: value labels above, the plot, then the axis labels below.
#define COL_TOP         18
#define COL_PLOT_HEIGHT 100
#define COL_HEIGHT      (COL_TOP + COL_PLOT_HEIGHT + 22)
// Horizontal gap on each side of a column, in % of its slot.
#define COL_GAP_PCT     0.18

static char *encode(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *dst = malloc(cap);
    if (dst) html_encode(dst, src, cap);
    return dst;
}

// `src` cut to at most `max_chars` UTF-8 characters, with "…" appended when
// cut - never splitting a multi-byte character.
static char *truncate_utf8(const char *src, size_t max_chars) {
    size_t bytes = 0, chars = 0;
    while (src[bytes] && chars < max_chars) {
        bytes++;
        while ((src[bytes] & 0xC0) == 0x80) bytes++;
        chars++;
    }
    if (!src[bytes]) return strdup(src);
    char *out = malloc(bytes + sizeof("\xE2\x80\xA6"));
    if (!out) return NULL;
    memcpy(out, src, bytes);
    memcpy(out + bytes, "\xE2\x80\xA6", sizeof("\xE2\x80\xA6"));
    return out;
}

// "45%", "<1%" for a non-zero share that rounds to 0, "0%" for zero.
static void format_share(char *out, size_t size, int value, int total) {
    if (value <= 0 || total <= 0) { snprintf(out, size, "0%%"); return; }
    int pct = (int)((100.0 * value) / total + 0.5);
    if (pct == 0) snprintf(out, size, "<1%%");
    else          snprintf(out, size, "%d%%", pct);
}

// Appends `piece` (taking ownership) to `*acc`; on any failure frees both and
// leaves `*acc` NULL, so callers can chain appends and check once at the end.
static void append_owned(char **acc, char *piece) {
    if (!*acc || !piece) { free(*acc); *acc = NULL; free(piece); return; }
    *acc = str_append(*acc, piece);
    free(piece);
}

char *analytics_chart_donut(const ChartSlice *slices, size_t count,
                            const char *center_label, const char *aria_label) {
    int total = 0;
    for (size_t i = 0; i < count; i++) if (slices[i].value > 0) total += slices[i].value;
    if (total == 0) return strdup(NO_DATA);

    int nonzero = 0;
    for (size_t i = 0; i < count; i++) if (slices[i].value > 0) nonzero++;

    char *aria_enc = encode(aria_label), *center_enc = encode(center_label);
    char *out = (aria_enc && center_enc) ? render_template(
        "<div class=\"boat-rudder__chart boat-rudder__chart--donut\">"
        "<svg class=\"boat-rudder__chart__svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
        "role=\"img\" aria-label=\"%s\">",
        DONUT_SIZE, DONUT_SIZE, DONUT_SIZE, DONUT_SIZE, aria_enc) : NULL;
    free(aria_enc);

    // Each slice is a dashed circle: a dash of its share, offset by the
    // shares before it, starting at 12 o'clock.
    double offset = 0;
    for (size_t i = 0; out && i < count; i++) {
        if (slices[i].value <= 0) continue;
        double share = 100.0 * slices[i].value / total;
        double dash  = (nonzero > 1 && share > 2 * DONUT_GAP) ? share - DONUT_GAP : share;
        char pct[8];
        format_share(pct, sizeof(pct), slices[i].value, total);
        char *label_enc = encode(slices[i].label);
        append_owned(&out, label_enc ? render_template(
            "<circle class=\"boat-rudder__chart__slice boat-rudder__chart__series--%d\" "
            "cx=\"%d\" cy=\"%d\" r=\"%d\" fill=\"none\" stroke-width=\"%d\" pathLength=\"100\" "
            "stroke-dasharray=\"%.2f %.2f\" stroke-dashoffset=\"%.2f\" "
            "transform=\"rotate(-90 %d %d)\"><title>%s: %d (%s)</title></circle>",
            slices[i].series, DONUT_SIZE / 2, DONUT_SIZE / 2, DONUT_RADIUS, DONUT_STROKE,
            dash, 100.0 - dash, -offset, DONUT_SIZE / 2, DONUT_SIZE / 2,
            label_enc, slices[i].value, pct) : NULL);
        free(label_enc);
        offset += share;
    }

    append_owned(&out, center_enc ? render_template(
        "<text class=\"boat-rudder__chart__center-value\" x=\"%d\" y=\"%d\" text-anchor=\"middle\">%d</text>"
        "<text class=\"boat-rudder__chart__center-label\" x=\"%d\" y=\"%d\" text-anchor=\"middle\">%s</text>"
        "</svg><ul class=\"boat-rudder__chart__legend\">",
        DONUT_SIZE / 2, DONUT_SIZE / 2 + 2, total, DONUT_SIZE / 2, DONUT_SIZE / 2 + 17, center_enc) : NULL);
    free(center_enc);

    for (size_t i = 0; out && i < count; i++) {
        char pct[8];
        format_share(pct, sizeof(pct), slices[i].value, total);
        char *label_enc = encode(slices[i].label);
        append_owned(&out, label_enc ? render_template(
            "<li><span class=\"boat-rudder__chart__swatch boat-rudder__chart__series--%d\"></span>"
            "<span class=\"boat-rudder__chart__legend-label\">%s</span>"
            "<span class=\"boat-rudder__chart__legend-value\">%d &middot; %s</span></li>",
            slices[i].series, label_enc, slices[i].value, pct) : NULL);
        free(label_enc);
    }

    append_owned(&out, strdup("</ul></div>"));
    return out;
}

char *analytics_chart_hbars(const ChartBar *bars, size_t count, const char *aria_label) {
    int max = 0;
    for (size_t i = 0; i < count; i++) if (bars[i].value > max) max = bars[i].value;
    if (count == 0 || max == 0) return strdup(NO_DATA);

    // No viewBox: x/width in % of the block's width, y in px - so the bars
    // stretch with the block while the text keeps its real font size.
    char *aria_enc = encode(aria_label);
    char *out = aria_enc ? render_template(
        "<svg class=\"boat-rudder__chart__svg boat-rudder__chart--hbars\" width=\"100%%\" height=\"%d\" "
        "role=\"img\" aria-label=\"%s\">",
        (int)count * HBAR_ROW_HEIGHT, aria_enc) : NULL;
    free(aria_enc);

    for (size_t i = 0; out && i < count; i++) {
        int y = (int)i * HBAR_ROW_HEIGHT;
        double width = 100.0 * bars[i].value / max;

        char *full_enc  = encode(bars[i].label);
        char *short_raw = truncate_utf8(bars[i].label, HBAR_LABEL_MAX);
        char *short_enc = short_raw ? encode(short_raw) : NULL;
        free(short_raw);
        char *detail_enc = encode(bars[i].detail ? bars[i].detail : "");
        char *href_enc   = encode(bars[i].href ? bars[i].href : "");

        char *title = (full_enc && detail_enc)
            ? render_template("<title>%s: %d%s%s</title>", full_enc, bars[i].value,
                              detail_enc[0] ? ", " : "", detail_enc)
            : NULL;
        char *text = (short_enc && href_enc)
            ? (href_enc[0]
                ? render_template("<a href=\"%s\"><text class=\"boat-rudder__chart__label\" x=\"0\" y=\"%d\">%s</text></a>",
                                  href_enc, y + HBAR_TEXT_Y, short_enc)
                : render_template("<text class=\"boat-rudder__chart__label\" x=\"0\" y=\"%d\">%s</text>",
                                  y + HBAR_TEXT_Y, short_enc))
            : NULL;

        append_owned(&out, (title && text) ? render_template(
            "<g class=\"boat-rudder__chart__row\">%s"
            "<rect class=\"boat-rudder__chart__hit\" x=\"0\" y=\"%d\" width=\"100%%\" height=\"%d\"></rect>"
            "%s"
            "<text class=\"boat-rudder__chart__value\" x=\"100%%\" y=\"%d\" text-anchor=\"end\">%d</text>"
            "<rect class=\"boat-rudder__chart__track\" x=\"0\" y=\"%d\" width=\"100%%\" height=\"%d\" rx=\"2\"></rect>"
            "<rect class=\"boat-rudder__chart__bar boat-rudder__chart__series--accent\" x=\"0\" y=\"%d\" "
            "width=\"%.2f%%\" height=\"%d\" rx=\"2\"></rect></g>",
            title, y, HBAR_ROW_HEIGHT, text, y + HBAR_TEXT_Y, bars[i].value,
            y + HBAR_BAR_Y, HBAR_BAR_HEIGHT, y + HBAR_BAR_Y, width, HBAR_BAR_HEIGHT) : NULL);

        free(title);
        free(text);
        free(full_enc);
        free(short_enc);
        free(detail_enc);
        free(href_enc);
    }

    append_owned(&out, strdup("</svg>"));
    return out;
}

char *analytics_chart_columns(const ChartColumn *columns, size_t count, const char *aria_label) {
    int max = 0;
    size_t max_i = 0;
    for (size_t i = 0; i < count; i++)
        if (columns[i].value > max) { max = columns[i].value; max_i = i; }
    if (count == 0 || max == 0) return strdup(NO_DATA);

    int baseline = COL_TOP + COL_PLOT_HEIGHT;
    char *aria_enc = encode(aria_label);
    char *out = aria_enc ? render_template(
        "<svg class=\"boat-rudder__chart__svg boat-rudder__chart--columns\" width=\"100%%\" height=\"%d\" "
        "role=\"img\" aria-label=\"%s\">",
        COL_HEIGHT, aria_enc) : NULL;
    free(aria_enc);

    double slot = 100.0 / (double)count;
    for (size_t i = 0; out && i < count; i++) {
        const ChartColumn *c = &columns[i];
        // At least 2px for a non-zero value, so it never vanishes.
        int h = c->value > 0 ? (int)((double)COL_PLOT_HEIGHT * c->value / max + 0.5) : 0;
        if (c->value > 0 && h < 2) h = 2;
        double x = slot * i + slot * COL_GAP_PCT;
        double w = slot * (1.0 - 2 * COL_GAP_PCT);
        double center = slot * i + slot / 2;

        char *tooltip_enc = encode(c->tooltip);
        char *label_enc   = encode(c->label);
        char *value_label = (c->highlight || i == max_i)
            ? render_template("<text class=\"boat-rudder__chart__value\" x=\"%.2f%%\" y=\"%d\" "
                              "text-anchor=\"middle\">%d</text>", center, baseline - h - 5, c->value)
            : strdup("");

        append_owned(&out, (tooltip_enc && label_enc && value_label) ? render_template(
            "<g class=\"boat-rudder__chart__row\"><title>%s</title>"
            "<rect class=\"boat-rudder__chart__hit\" x=\"%.2f%%\" y=\"0\" width=\"%.2f%%\" height=\"%d\"></rect>"
            "<rect class=\"boat-rudder__chart__bar boat-rudder__chart__series--%s\" x=\"%.2f%%\" y=\"%d\" "
            "width=\"%.2f%%\" height=\"%d\" rx=\"2\"></rect>%s"
            "<text class=\"boat-rudder__chart__axis-label%s\" x=\"%.2f%%\" y=\"%d\" text-anchor=\"middle\">%s</text></g>",
            tooltip_enc, slot * i, slot, COL_HEIGHT,
            c->highlight ? "accent" : "muted", x, baseline - h, w, h, value_label,
            c->highlight ? " boat-rudder__chart__axis-label--strong" : "",
            center, baseline + 16, label_enc) : NULL);

        free(tooltip_enc);
        free(label_enc);
        free(value_label);
    }

    append_owned(&out, render_template(
        "<line class=\"boat-rudder__chart__baseline\" x1=\"0\" y1=\"%d\" x2=\"100%%\" y2=\"%d\"></line></svg>",
        baseline, baseline));
    return out;
}
