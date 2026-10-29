#include "client_icon.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// "Name/12" -> 12, "Name" -> 0.
static int key_version(const char *key) {
    const char *slash = strchr(key, '/');
    return slash ? atoi(slash + 1) : 0;
}

static int key_is(const char *key, const char *name) {
    size_t n = strlen(name);
    return strncmp(key, name, n) == 0 && (key[n] == '\0' || key[n] == '/');
}

static void label(char *out, size_t size, const char *name, int version) {
    if (version > 0) snprintf(out, size, "%s %d", name, version);
    else snprintf(out, size, "%s", name);
}

static const struct { const char *key; const char *icon; const char *name; } BROWSERS[] = {
    { "Chrome",     "chrome",   "Chrome" },
    { "Firefox",    "firefox",  "Firefox" },
    { "Edge",       "edge",     "Edge" },
    { "Safari",     "safari",   "Safari" },
    { "Opera",      "opera",    "Opera" },
    { "MSIE",       "ie",       "Internet Explorer" },
    { "Netscape",   "netscape", "Netscape" },
    { "Mosaic",     "mosaic",   "NCSA Mosaic" },
    { "Lynx",       "text",     "Lynx" },
    { "Links",      "text",     "Links" },
    { "w3m",        "text",     "w3m" },
    { "Cello",      "browser",  "Cello" },
    { "WinWAP",     "wap",      "WinWAP" },
    { "UP_Browser", "wap",      "UP.Browser" },
    { "Obigo",      "wap",      "Obigo" },
};

static void browser_of(const char *key, const char *ua, ClientIcons *out) {
    for (size_t i = 0; i < sizeof(BROWSERS) / sizeof(BROWSERS[0]); i++) {
        if (key_is(key, BROWSERS[i].key)) {
            out->browser_icon = BROWSERS[i].icon;
            label(out->browser_label, sizeof(out->browser_label), BROWSERS[i].name, key_version(key));
            return;
        }
    }
    // Browsers the parser has no key for.
    static const struct { const char *token; const char *icon; const char *name; } OTHERS[] = {
        { "ELinks", "text", "ELinks" }, { "Line Mode", "text", "Line Mode Browser" },
        { "LineMode", "text", "Line Mode Browser" }, { "Arachne", "arachne", "Arachne" },
        // WAP phones, and the WAP gateway when a phone didn't say what it is.
        { "BoatRudder-WAP-Gateway", "wap", "WAP phone" }, { "Nokia", "wap", "Nokia" },
        { "Ericsson", "wap", "Ericsson" }, { "MOT-", "wap", "Motorola" },
    };
    for (size_t i = 0; ua && i < sizeof(OTHERS) / sizeof(OTHERS[0]); i++) {
        if (strstr(ua, OTHERS[i].token)) {
            out->browser_icon = OTHERS[i].icon;
            snprintf(out->browser_label, sizeof(out->browser_label), "%s", OTHERS[i].name);
            return;
        }
    }
    out->browser_icon = "browser";
    snprintf(out->browser_label, sizeof(out->browser_label), "Other browser");
}

// "DOS" as a word of its own ("(DOS)", "MS-DOS", "DOS/V"), not inside a longer one.
static int says_dos(const char *ua) {
    if (strstr(ua, "FreeDOS") || strstr(ua, "DJGPP")) return 1;
    for (const char *p = strstr(ua, "DOS"); p; p = strstr(p + 1, "DOS")) {
        int before = p > ua && isalnum((unsigned char)p[-1]);
        int after = isalpha((unsigned char)p[3]);
        if (!before && !after) return 1;
    }
    return 0;
}

static void os_of(const char *key, const char *ua, ClientIcons *out) {
    out->os_icon = NULL;
    out->os_label[0] = '\0';
    char *l = out->os_label;
    size_t n = sizeof(out->os_label);

    if (key_is(key, "Windows")) {
        const char *v = strchr(key, '/') ? strchr(key, '/') + 1 : "";
        static const struct { const char *v; const char *icon; const char *name; } WIN[] = {
            { "10", "windows", "Windows 10" },   { "8_1", "windows", "Windows 8.1" }, { "8", "windows", "Windows 8" },
            { "7", "windows-xp", "Windows 7" },  { "Vista", "windows-xp", "Windows Vista" },
            { "XP", "windows-xp", "Windows XP" }, { "Server2003", "windows-xp", "Windows Server 2003" },
            { "2000", "windows-classic", "Windows 2000" }, { "NT4", "windows-classic", "Windows NT 4" },
            { "ME", "windows-classic", "Windows Me" }, { "98", "windows-classic", "Windows 98" },
            { "95", "windows-classic", "Windows 95" }, { "3x", "windows-classic", "Windows 3.x" },
        };
        for (size_t i = 0; i < sizeof(WIN) / sizeof(WIN[0]); i++) {
            if (strcmp(v, WIN[i].v) == 0) { out->os_icon = WIN[i].icon; snprintf(l, n, "%s", WIN[i].name); return; }
        }
        out->os_icon = "windows-classic";   // "x86", "NT5_2"...: an older Windows
        snprintf(l, n, "Windows");
        return;
    }
    if (key_is(key, "Mac"))     { out->os_icon = "mac";     snprintf(l, n, "macOS"); return; }
    if (key_is(key, "iOS"))     { out->os_icon = "ios";     label(l, n, "iOS", key_version(key)); return; }
    if (key_is(key, "Android")) { out->os_icon = "android"; label(l, n, "Android", key_version(key)); return; }
    if (key_is(key, "Linux"))   { out->os_icon = "linux";   snprintf(l, n, "Linux"); return; }
    if (key_is(key, "FreeBSD")) { out->os_icon = "bsd";     label(l, n, "FreeBSD", key_version(key)); return; }
    if (key_is(key, "OpenBSD")) { out->os_icon = "bsd";     snprintf(l, n, "OpenBSD"); return; }
    if (key_is(key, "NetBSD"))  { out->os_icon = "bsd";     snprintf(l, n, "NetBSD"); return; }
    if (key_is(key, "BeOS"))    { out->os_icon = "unix";    snprintf(l, n, "BeOS"); return; }
    if (key_is(key, "OS2"))     { out->os_icon = "unix";    snprintf(l, n, "OS/2"); return; }
    if (key_is(key, "Solaris")) { out->os_icon = "unix";    snprintf(l, n, "Solaris"); return; }

    // What the parser has no key for, read from the User-Agent itself.
    if (!ua) return;
    // DOS builds (Lynx, Links) don't say so unless their User-Agent is set to; Arachne only runs on DOS.
    if (says_dos(ua) || (strstr(ua, "Arachne") && !strstr(ua, "Linux"))) {
        out->os_icon = "dos";
        snprintf(l, n, strstr(ua, "FreeDOS") ? "FreeDOS" : "MS-DOS");
        return;
    }
    if (strstr(ua, "Macintosh") || strstr(ua, "Mac_PowerPC") || strstr(ua, "Mac_68") || strstr(ua, "Mac_PPC")) {
        out->os_icon = "mac-classic";
        snprintf(l, n, "Mac OS");
        return;
    }
    static const struct { const char *token; const char *name; } UNIXES[] = {
        { "IRIX", "IRIX" }, { "HP-UX", "HP-UX" }, { "AIX", "AIX" }, { "OSF1", "Digital UNIX" },
        { "X11", "Unix" },
    };
    for (size_t i = 0; i < sizeof(UNIXES) / sizeof(UNIXES[0]); i++) {
        if (strstr(ua, UNIXES[i].token)) { out->os_icon = "unix"; snprintf(l, n, "%s", UNIXES[i].name); return; }
    }
    if (strstr(ua, "Win")) { out->os_icon = "windows-classic"; snprintf(l, n, "Windows"); return; }
}

int client_icons(const char *browser_key, const char *os_key, const char *user_agent, ClientIcons *out) {
    memset(out, 0, sizeof(*out));
    if (!browser_key || !browser_key[0]) {
        out->browser_icon = "browser";
        return 0;
    }
    browser_of(browser_key, user_agent, out);
    os_of(os_key ? os_key : "", user_agent, out);
    return 1;
}
