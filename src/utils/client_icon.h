#ifndef CLIENT_ICON_H
#define CLIENT_ICON_H

#include <stddef.h>

// Which icon (html/assets/icons/clients/<name>.gif, drawn by
// scripts/generate-client-icons.py) and which words stand for the browser and
// operating system a blog comment was written from. Input: what the comment
// stored - ua_parser.h's keys ("MSIE/5", "Windows/2000") and the User-Agent
// itself, read where the keys say nothing ("Macintosh" before Mac OS X,
// "X11"...).

#define CLIENT_ICON_DIR "/assets/icons/clients/"

typedef struct {
    const char *browser_icon;   // icon name, never NULL ("browser" when unknown)
    char        browser_label[64];
    const char *os_icon;        // icon name, or NULL when the system is unknown (text browsers rarely say)
    char        os_label[64];
} ClientIcons;

// Fills *out. Any argument may be NULL or "" (a comment older than this record).
// Returns 1 when there is anything to show (a browser was recorded), else 0.
int client_icons(const char *browser_key, const char *os_key, const char *user_agent, ClientIcons *out);

#endif // CLIENT_ICON_H
