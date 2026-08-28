#include "slider.h"
#include "../../db/cms_site_settings.h"

char *slider(int epoch) {
    // cms_get_site_banner() falls back to the on-disk
    // slider/slider_epoch<N>.html itself when the DB has no override, so a
    // fresh install renders exactly as before this became DB-backed.
    return cms_get_site_banner(epoch);
}
