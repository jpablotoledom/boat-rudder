#ifndef ANALYTICS_VIEW_H
#define ANALYTICS_VIEW_H

// Builds the /dashboard/analytics report fragment: a period selector, total/
// per-epoch visit counts, browser/OS/country/route breakdowns, and a top-20
// articles table, all aggregated from analytics.c's page_visits_daily/
// entry_visits_daily day-buckets.
//
// `period` is one of "day"/"week"/"month"/"year"/"all"/"range" ("month" if
// NULL/empty/unrecognized); the other params narrow it: `date` for "day"
// ("YYYY-MM-DD", defaults to today UTC), `year`/`week` for "week",
// `year`/`month` for "month", `year` alone for "year", `from`/`to`
// ("YYYY-MM-DD" each) for "range". Ignored where not relevant to `period`.
//
// Epoch 3 only - the report itself is admin tooling, gated the same way as
// every other /dashboard/settings page (see http_router.c's route), so
// there is exactly one template (analytics_epoch3.html) instead of one per
// epoch. Returns a malloc'd string, or NULL on a missing template or
// allocation failure.
char *analytics_view(int epoch, const char *period, int year, int month, int week,
                      const char *date, const char *from, const char *to);

// Builds the /dashboard home's analytics summary (analytics/summary_epoch3.html):
// visits today and over the last 7 days (today plus the 6 days before, UTC),
// visits per epoch, and the top 5 countries, browser families and blog
// articles, each ranked by its last-7-days count and shown with today's count
// next to it - each drawn as an SVG chart (analytics_charts.h: visits per day
// as columns, epochs as a donut, the top-5 lists as horizontal bars) with its
// table folded underneath. Same data and gating as analytics_view(); epoch 3
// only. Returns a malloc'd string, or NULL on a missing template or
// allocation failure.
char *analytics_summary(int epoch);

#endif // ANALYTICS_VIEW_H
