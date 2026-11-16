#ifndef CMS_CATEGORY_RANKING_H
#define CMS_CATEGORY_RANKING_H

#include "cms_categories.h"
#include <stddef.h>

// Which categories the /blog category bar shows first: each category's
// visits are the sum of its published blog posts' visits over the last
// CATEGORY_RANKING_DAYS days (entry_visits_daily, see analytics.c) - a post
// in two categories counts for both. A background thread works it out at
// startup and every CATEGORY_RANKING_INTERVAL_SECS after; requests only
// read the result, kept in memory.

#define CATEGORY_RANKING_DAYS          30
#define CATEGORY_RANKING_INTERVAL_SECS (3 * 60 * 60)

// Starts the thread (a no-op returning 0 if mongodb isn't ready). Returns
// -1 if it could not start.
int  category_ranking_start(void);
void category_ranking_stop(void);

// Works the ranking out now, in the calling thread - what the background
// thread runs.
void category_ranking_refresh(void);

// Reorders cats[0..count) most visited first; categories with equal visits
// (none, before the first refresh) keep their order.
void category_ranking_sort(CmsCategoryItem *cats, size_t count);

#endif // CMS_CATEGORY_RANKING_H
