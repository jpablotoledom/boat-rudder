#include "mainbanner.h"
#include "../../db/cms_site_settings.h"

char *mainbanner(int epoch) {
    // cms_get_site_banner() falls back to the on-disk
    // mainbanner/mainbanner_epoch<N>.html itself when the DB has no
    // override, so a fresh install renders exactly as before this became
    // DB-backed.
    return cms_get_site_banner(epoch);
}
