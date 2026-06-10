#include "detect_epoch.h"
#include <stdlib.h>
#include <string.h>

static int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int detect_epoch(const char *user_agent) {
    if (!user_agent || !*user_agent) {
        return EPOCH_EARLY;
    }

    // WAP / WML devices.
    if (contains(user_agent, "Wap") || contains(user_agent, "WAP") ||
        contains(user_agent, "Nokia") || contains(user_agent, "UP.Browser") ||
        contains(user_agent, "UP/")) {
        return EPOCH_WML;
    }

    // Pre-standard text-mode browsers.
    if (contains(user_agent, "Lynx") || contains(user_agent, "Cello") ||
        contains(user_agent, "Line Mode Browser") ||
        contains(user_agent, "ELinks") || contains(user_agent, "w3m")) {
        return EPOCH_PRESTANDARD;
    }

    // Internet Explorer: MSIE 5+ -> MIDDLE, MSIE <5 -> EARLY.
    const char *msie = strstr(user_agent, "MSIE ");
    if (msie) {
        int version = atoi(msie + 5);
        return (version >= 5) ? EPOCH_MIDDLE : EPOCH_EARLY;
    }

    // Internet Explorer 11 identifies itself via the Trident token.
    if (contains(user_agent, "Trident/")) {
        return EPOCH_MIDDLE;
    }

    // Firefox: version 4+ -> MODERN, older -> MIDDLE.
    const char *firefox = strstr(user_agent, "Firefox/");
    if (firefox) {
        int version = atoi(firefox + strlen("Firefox/"));
        return (version >= 4) ? EPOCH_MODERN : EPOCH_MIDDLE;
    }

    // Chrome/Chromium: version 10+ -> MODERN, older -> MIDDLE.
    const char *chrome = strstr(user_agent, "Chrome/");
    if (chrome) {
        int version = atoi(chrome + strlen("Chrome/"));
        return (version >= 10) ? EPOCH_MODERN : EPOCH_MIDDLE;
    }

    // Safari without a Chrome token is a modern WebKit browser.
    if (contains(user_agent, "Safari/")) {
        return EPOCH_MODERN;
    }

    // Netscape Navigator: version 4+ -> MIDDLE, older -> EARLY.
    const char *netscape = strstr(user_agent, "Netscape");
    if (netscape) {
        const char *slash = strchr(netscape, '/');
        if (slash) {
            int version = atoi(slash + 1);
            return (version >= 4) ? EPOCH_MIDDLE : EPOCH_EARLY;
        }
        return EPOCH_EARLY;
    }

    // Mosaic and other pre-Netscape browsers.
    if (contains(user_agent, "Mosaic")) {
        return EPOCH_EARLY;
    }

    // Generic "Mozilla/X.Y" token: bucket by major version.
    if (strncmp(user_agent, "Mozilla/", strlen("Mozilla/")) == 0) {
        int version = atoi(user_agent + strlen("Mozilla/"));
        if (version <= 2) return EPOCH_EARLY;
        if (version <= 4) return EPOCH_MIDDLE;
        return EPOCH_MODERN;
    }

    return EPOCH_EARLY;
}
