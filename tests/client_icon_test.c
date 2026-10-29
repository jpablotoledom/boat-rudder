// Which icon and words stand for a comment's browser and system
// (src/utils/client_icon.c), from ua_parser.h's keys and the User-Agent - and
// that every icon it names exists in html/assets/icons/clients/.
//
// Run from the source directory: ctest --test-dir build/debug.

#include "../src/utils/client_icon.h"
#include "../src/utils/ua_parser.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect_str(const char *what, const char *got, const char *expected) {
    checks++;
    if ((got == NULL) != (expected == NULL) || (got && strcmp(got, expected) != 0)) {
        printf("FAIL %s: expected \"%s\", got \"%s\"\n", what, expected ? expected : "(null)", got ? got : "(null)");
        failures++;
    }
}

static void icon_exists(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "html/assets/icons/clients/%s.gif", name);
    FILE *f = fopen(path, "rb");
    checks++;
    if (!f) { printf("FAIL missing %s\n", path); failures++; return; }
    fclose(f);
}

// A User-Agent through ua_parser.h, as comments_post() does, then mapped.
static void expect_ua(const char *ua, const char *b_icon, const char *b_label, const char *o_icon, const char *o_label) {
    char browser[64], os[64];
    ua_parse_browser(ua, browser, sizeof(browser));
    ua_parse_os(ua, os, sizeof(os));
    ClientIcons c;
    client_icons(browser, os, ua, &c);
    expect_str(ua, c.browser_icon, b_icon);
    expect_str(ua, c.browser_label, b_label);
    expect_str(ua, c.os_icon, o_icon);
    expect_str(ua, c.os_label, o_label ? o_label : "");
    icon_exists(c.browser_icon);
    if (c.os_icon) icon_exists(c.os_icon);
}

int main(void) {
    expect_ua("Mozilla/4.0 (compatible; MSIE 5.01; Windows NT 5.0)", "ie", "Internet Explorer 5", "windows-classic", "Windows 2000");
    expect_ua("Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1)", "ie", "Internet Explorer 6", "windows-xp", "Windows XP");
    expect_ua("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
              "chrome", "Chrome 120", "windows", "Windows 10");
    expect_ua("Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0", "firefox", "Firefox 128", "linux", "Linux");
    expect_ua("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
              "safari", "Safari 17", "mac", "macOS");
    expect_ua("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Version/17.0 Mobile/15E148 Safari/604.1",
              "safari", "Safari 17", "ios", "iOS 17");
    expect_ua("Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 Chrome/120.0 Mobile Safari/537.36",
              "chrome", "Chrome 120", "android", "Android 14");
    expect_ua("Mozilla/3.0 (Macintosh; I; PPC)", "netscape", "Netscape 3", "mac-classic", "Mac OS");
    expect_ua("Mozilla/3.0 (Win95; I)", "netscape", "Netscape 3", "windows-classic", "Windows 95");
    expect_ua("Mozilla/3.01 (X11; I; IRIX 6.2 IP22)", "netscape", "Netscape 3", "unix", "IRIX");
    expect_ua("NCSA_Mosaic/2.7b5 (X11;Linux 2.6.7 i686) libwww/2.12 modified", "mosaic", "NCSA Mosaic 2", "linux", "Linux");
    expect_ua("NCSA Mosaic/2.0 (Windows x86)", "mosaic", "NCSA Mosaic 2", "windows-classic", "Windows");
    expect_ua("Lynx/2.9.0dev.12 libwww-FM/2.14 SSL-MM/1.4.1 GNUTLS/3.7.8", "text", "Lynx 2", NULL, NULL);
    expect_ua("Lynx/2.9.0dev.10 (DOS) libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/1.0.2u", "text", "Lynx 2", "dos", "MS-DOS");
    expect_ua("Lynx/2.8.5rel.1 libwww-FM/2.14 (FreeDOS; DJGPP)", "text", "Lynx 2", "dos", "FreeDOS");
    expect_ua("Links (2.29; DOS; text)", "text", "Links 2", "dos", "MS-DOS");
    expect_ua("xChaos_Arachne/5.1.89;GPL,386+", "arachne", "Arachne", "dos", "MS-DOS");
    expect_ua("Mozilla/5.0 (webOS/1.4.0; U; en-US) AppleWebKit/532.2 Version/1.0 Safari/532.2", "safari", "Safari 1", NULL, NULL);
    expect_ua("Nokia7110/1.0 (04.94)", "wap", "Nokia", NULL, NULL);
    expect_ua("BoatRudder-WAP-Gateway", "wap", "WAP phone", NULL, NULL);
    expect_ua("Rover 1.5 (Palm; IP; OS v. 3.5.3)", "wap", "Rover", "palm", "Palm OS 3");
    expect_ua("Mozilla/4.0 (compatible; MSIE 6.0; Windows 98; PalmSource/Palm-D050; Blazer/4.3) 16;320x320",
              "ie", "Internet Explorer 6", "palm", "Palm OS");
    expect_ua("Mozilla/4.61 [en] (OS/2; U)", "netscape", "Netscape 4", "os2", "OS/2");
    expect_ua("IBM WebExplorer /v0.94 (OS/2)", "browser", "IBM WebExplorer", "os2", "OS/2");
    expect_ua("IBM WebExplorer DLL /v960311", "browser", "IBM WebExplorer", "os2", "OS/2");
    expect_ua("w3m/0.5.3", "text", "w3m", NULL, NULL);
    expect_ua("Mozilla/5.0 (FreeBSD 14.0; amd64) Firefox/120.0", "firefox", "Firefox 120", "bsd", "FreeBSD 14");

    // Nothing recorded (comments older than the record): nothing to show.
    ClientIcons c;
    checks++;
    if (client_icons("", "", "", &c) != 0 || client_icons(NULL, NULL, NULL, &c) != 0) { printf("FAIL empty\n"); failures++; }

    // Every icon the generator draws is there.
    static const char *ALL[] = { "chrome", "firefox", "edge", "safari", "opera", "ie", "netscape", "mosaic", "text",
                                 "wap", "browser", "windows", "windows-xp", "windows-classic", "mac", "mac-classic",
                                 "linux", "android", "ios", "bsd", "unix", "dos", "os2", "palm", "os", "arachne", NULL };
    for (int i = 0; ALL[i]; i++) icon_exists(ALL[i]);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
