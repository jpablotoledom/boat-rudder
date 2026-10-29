#define _XOPEN_SOURCE 700

#include "http_router.h"
#include "http_constants.h"
#include "connection.h"
#include "http_request_parser.h"
#include "utils/url_parser.h"
#include "utils/static_file_server.h"
#include "../db/auth.h"
#include "../db/cms_categories.h"
#include "../db/cms_entries.h"
#include "../db/cms_entries_admin.h"
#include "../db/cms_languages.h"
#include "../db/cms_menu.h"
#include "../db/cms_site_settings.h"
#include "../db/cms_fonts.h"
#include "../db/cms_comments.h"
#include "../db/cms_themes.h"
#include "../db/cms_users_admin.h"
#include "../db/mongodb_manager.h"
#include "../db/session_manager.h"
#include "../db/short_links.h"
#include "../html_builder/orchestrator.h"
#include "../html_builder/page_layout.h"
#include "../modules/analytics/analytics.h"
#include "../modules/analytics_view/analytics_view.h"
#include "../modules/blog_list/blog_list.h"
#include "../modules/category_menu/category_menu.h"
#include "../modules/categories_admin/categories_admin.h"
#include "../modules/dashboard/dashboard.h"
#include "../modules/entry_editor/entry_editor.h"
#include "../modules/entry_editor/entry_editor_blocks.h"
#include "../modules/entry_page/entry_page.h"
#include "../modules/home_content/home_content.h"
#include "../modules/error/error.h"
#include "../modules/fonts_admin/fonts_admin.h"
#include "../modules/languages_admin/languages_admin.h"
#include "../modules/language_page/language_page.h"
#include "../modules/site_page/site_page.h"
#include "../modules/search_page/search_page.h"
#include "../modules/comments/comments.h"
#include "../modules/comments_admin/comments_admin.h"
#include "../modules/theme_page/theme_page.h"
#include "../modules/login/login.h"
#include "../modules/media_admin/media_admin.h"
#include "../modules/menu_admin/menu_admin.h"
#include "../modules/site_settings_admin/site_settings_admin.h"
#include "../modules/browser_detection_admin/browser_detection_admin.h"
#include "../modules/updates_admin/updates_admin.h"
#include "../modules/extensions_admin/extensions_admin.h"
#include "../extensions/extensions.h"
#include "../db/cms_browser_detection.h"
#include "../modules/users_admin/users_admin.h"
#include "../db/cms_media.h"
#include "../db/cms_media_galleries.h"
#include "utils/multipart_parser.h"
#include "../utils/build_epoch_response.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/request_charset.h"
#include "../utils/request_code_lines.h"
#include "../utils/request_wml.h"
#include "../utils/captcha.h"
#include "../utils/request_lang.h"
#include "../utils/request_theme.h"
#include "../utils/request_site.h"
#include "../utils/mailer.h"
#include "../utils/theme_catalog.h"
#include "../utils/request_user.h"
#include "../utils/config_loader.h"
#include "../utils/qr_generator/qr_generator.h"
#include "../utils/image_convert/image_convert.h"
#include "../utils/detect_epoch.h"
#include "../utils/json_utils.h"
#include "../utils/log.h"
#include "../utils/http_utils.h"
#include "../utils/template_utils.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *get_header_value(HttpRequest *req, const char *key) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].key, key) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

// HTTP/1.0 (RFC 1945) requires a redirect's Location to be an absolute URI;
// a bare path like "/" is only legal from RFC 2616 onward. Some HTTP/1.0-era
// clients (NCSA Mosaic among them) take that literally and instead of
// resolving the path against the current page, prepend "http://" straight
// onto it - "/" becomes the schemeless, hostless "http:///", which then
// fails as a DNS lookup on an empty hostname. Building the absolute form
// ourselves, from the Host the client itself just connected with, sidesteps
// the ambiguity entirely. Falls back to the bare path if the request had no
// Host header (rare, but then there is no better address to build from).
static char *absolute_location(void *ctx, HttpRequest *req, const char *path) {
    const char *host = get_header_value(req, "Host");
    if (!host || !host[0]) return NULL;

    const char *scheme = ((connection_ctx_t *)ctx)->ssl ? "https" : "http";
    size_t len = strlen(scheme) + strlen(host) + strlen(path) + 4;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s://%s%s", scheme, host, path);
    return out;
}

// Media files on disk live at ./html/content/posts/<author>/<dir>/<name>,
// under whichever size-variant suffix scripts/image-optimizer.sh wrote
// (same-extension for _full/_half/_small, always .gif for _medium/_micro -
// see image_for_epoch() in entry_page.c). A dashboard upload's own bare
// name has no such file, so unlink()/rename() failing on it is routine and
// silently ignored, exactly like the ones for the size variants.
static void media_variant_path(char *out, size_t out_size, const char *dir_path,
                               const char *name, const char *suffix, int force_gif) {
    char stem[256] = {0};
    const char *ext = strrchr(name, '.');
    size_t stem_len = ext ? (size_t)(ext - name) : strlen(name);
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, name, stem_len);
    stem[stem_len] = '\0';
    snprintf(out, out_size, "%s/%s%s%s", dir_path, stem, suffix,
             force_gif ? ".gif" : (ext ? ext : ""));
}

// Uploads made before scripts/image-optimizer.sh stopped creating it have a
// base-name symlink to _half, under the stored name (photo.jpg) or the
// uploaded extension (photo.png), which the DB record doesn't carry; the
// upload allowlist bounds which extensions that can be, so each is tried,
// and only ever acted on when it is a symlink - another item's real file is
// never touched.
static const char *media_upload_link_exts[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp",
    ".JPG", ".JPEG", ".PNG", ".GIF", ".WEBP", NULL
};

static void media_link_path(char *out, size_t out_size, const char *dir_path,
                            const char *name, const char *ext) {
    char stem[256] = {0};
    const char *dot = strrchr(name, '.');
    size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, name, stem_len);
    snprintf(out, out_size, "%s/%s%s", dir_path, stem, ext);
}

static int is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static void media_item_dir_path(const CmsMediaItem *item, char *out, size_t out_size) {
    snprintf(out, out_size, "./html/content/posts/%s/%s", item->author_username, item->dir_name);
}

// Deletes every size variant of a media item from disk (the base file plus
// the optimizer's _full/_half/_small/_medium/_micro copies).
static void media_delete_variants(const CmsMediaItem *item) {
    char dir_path[512];
    media_item_dir_path(item, dir_path, sizeof(dir_path));

    static const char *same_ext_suffixes[] = { "", "_full", "_half", "_small", NULL };
    static const char *gif_suffixes[]      = { "_medium", "_micro", NULL };

    char path[1024];
    for (int i = 0; same_ext_suffixes[i]; i++) {
        media_variant_path(path, sizeof(path), dir_path, item->name, same_ext_suffixes[i], 0);
        unlink(path);
    }
    for (int i = 0; gif_suffixes[i]; i++) {
        media_variant_path(path, sizeof(path), dir_path, item->name, gif_suffixes[i], 1);
        unlink(path);
    }
    for (int i = 0; media_upload_link_exts[i]; i++) {
        media_link_path(path, sizeof(path), dir_path, item->name, media_upload_link_exts[i]);
        if (is_symlink(path)) unlink(path);
    }
}

// Moves every size variant of a media item to dest_dir_name (same author),
// creating that directory on disk if it doesn't exist yet. Returns 0 even
// when some variants had nothing to move - only a failure to prepare the
// destination directory is fatal, matching how the DB update after this
// call is the operation callers actually care about.
static int media_move_variants(const CmsMediaItem *item, const char *dest_dir_name) {
    char src_dir[512], dst_dir[512];
    media_item_dir_path(item, src_dir, sizeof(src_dir));
    snprintf(dst_dir, sizeof(dst_dir), "./html/content/posts/%s/%s", item->author_username, dest_dir_name);

    struct stat st;
    if (stat(dst_dir, &st) == -1 && mkdir(dst_dir, 0775) != 0) {
        LOG_ERROR("media_move_variants: could not create %s: %s", dst_dir, strerror(errno));
        return -1;
    }

    static const char *same_ext_suffixes[] = { "", "_full", "_half", "_small", NULL };
    static const char *gif_suffixes[]      = { "_medium", "_micro", NULL };

    char src_path[1024], dst_path[1024];
    for (int i = 0; same_ext_suffixes[i]; i++) {
        media_variant_path(src_path, sizeof(src_path), src_dir, item->name, same_ext_suffixes[i], 0);
        media_variant_path(dst_path, sizeof(dst_path), dst_dir, item->name, same_ext_suffixes[i], 0);
        rename(src_path, dst_path);
    }
    for (int i = 0; gif_suffixes[i]; i++) {
        media_variant_path(src_path, sizeof(src_path), src_dir, item->name, gif_suffixes[i], 1);
        media_variant_path(dst_path, sizeof(dst_path), dst_dir, item->name, gif_suffixes[i], 1);
        rename(src_path, dst_path);
    }
    // The link's target is relative ("photo_half.gif"), so it still
    // resolves once moved next to the variants.
    for (int i = 0; media_upload_link_exts[i]; i++) {
        media_link_path(src_path, sizeof(src_path), src_dir, item->name, media_upload_link_exts[i]);
        media_link_path(dst_path, sizeof(dst_path), dst_dir, item->name, media_upload_link_exts[i]);
        if (is_symlink(src_path)) rename(src_path, dst_path);
    }
    return 0;
}

// Uploads with no directory selected (e.g. from the entry editor's "Select
// photos" picker before any directory has been created) go into this
// author's "default" directory instead of failing - created on first use,
// physically and in the DB, exactly like any other directory. The caller
// can move the file elsewhere afterwards with the "Move to..." picker.
static int resolve_or_create_default_media_directory(const char *user_id, const char *username,
                                                      CmsMediaDirectory *out) {
    if (cms_get_media_directory_by_name("default", user_id, out)) return 0;

    // A new author has no folder under posts/ yet: create it first, as the
    // directory-create route does, or the default one can't be made.
    char author_path[512], dirpath[sizeof(author_path) + 16];
    snprintf(author_path, sizeof(author_path), "./html/content/posts/%s", username);
    snprintf(dirpath, sizeof(dirpath), "%s/default", author_path);
    struct stat st;
    if (stat(author_path, &st) == -1) mkdir(author_path, 0775);
    if (stat(dirpath, &st) == -1 && mkdir(dirpath, 0775) != 0) {
        LOG_ERROR("resolve_or_create_default_media_directory: could not create %s: %s",
                  dirpath, strerror(errno));
        return -1;
    }

    char new_id[25];
    if (cms_create_media_directory("default", "posts", user_id, new_id) != 0) {
        LOG_ERROR("resolve_or_create_default_media_directory: DB insert failed");
        return -1;
    }
    return cms_get_media_directory_by_id(new_id, out) ? 0 : -1;
}

// A media directory name becomes a path component under
// ./html/content/posts/<author>/ and reaches image-optimizer.sh's command
// line, so it is held to a plain charset: 3-60 of [A-Za-z0-9_-] - no '/',
// no '.', hence no "..". Enforced on creation and rename, and re-checked on
// names read back from the DB before they touch the filesystem.
static int media_dir_name_valid(const char *name) {
    size_t len = name ? strlen(name) : 0;
    if (len < 3 || len > 60) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

// cms_get_username_by_id() (and the media item's author_username) already
// map the email's local part onto [A-Za-z0-9._-]; what is still unsafe as a
// path component is "", "." and ".." - rejected along with any leading dot.
static int media_username_valid(const char *username) {
    if (!username || !username[0] || username[0] == '.') return 0;
    for (const char *p = username; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

// A stored media file name: the upload route's sanitized name (charset
// [A-Za-z0-9._-]), never empty or dot-led.
static int media_file_name_valid(const char *name) {
    return media_username_valid(name);
}

// Whether a media item's on-disk location, built from DB fields, is safe to
// unlink()/rename() under.
static int media_item_paths_valid(const CmsMediaItem *item) {
    return media_username_valid(item->author_username) &&
           media_dir_name_valid(item->dir_name) &&
           media_file_name_valid(item->name);
}

// Ownership rule for the media library: an Administrador manages everyone's
// media; an Autor only what `author_id` says is theirs.
static int media_can_manage(const char *role, const char *user_id, const char *owner_id) {
    return strcmp(role, "admin") == 0 || strcmp(user_id, owner_id) == 0;
}

// The part of the library an account sees: an Administrador and the
// read-only Observador every author's, an Autor only their own. Returns the
// author id to scope cms_get_media_*() to, or NULL for everything.
static const char *media_scope(const char *role, const char *user_id) {
    return strcmp(role, "admin") == 0 || strcmp(role, "viewer") == 0 ? NULL : user_id;
}

// Fills each directory's owner_label for a viewer who sees every author's
// (media_scope() NULL) - "Me" for their own, the owner's name (else
// username) for the rest - since two authors' "default" look alike. An
// Autor only sees their own: no labels.
static void media_label_owners(CmsMediaDirectory *dirs, size_t count, const char *role, const char *user_id) {
    if (media_scope(role, user_id)) return;
    for (size_t i = 0; i < count; i++) {
        CmsMediaDirectory *d = &dirs[i];
        if (strcmp(d->author_id, user_id) == 0) {
            snprintf(d->owner_label, sizeof(d->owner_label), "Me");
            continue;
        }
        size_t j = 0;   // the same owner as an earlier directory: its label
        while (j < i && strcmp(dirs[j].author_id, d->author_id) != 0) j++;
        if (j < i) {
            memcpy(d->owner_label, dirs[j].owner_label, sizeof(d->owner_label));
            continue;
        }
        char *name = cms_get_user_name_by_id(d->author_id);
        if (name && name[0]) snprintf(d->owner_label, sizeof(d->owner_label), "%s", name);
        else if (cms_get_username_by_id(d->author_id, d->owner_label, sizeof(d->owner_label)) != 0)
            snprintf(d->owner_label, sizeof(d->owner_label), "unknown");
        free(name);
    }
}

// Upload allowlist: the formats scripts/image-optimizer.sh turns into its
// JPEG/GIF variants, recognized by the file's magic bytes - its content, not
// its name. Returns the extension that format is stored under (".jpg",
// ".png", ".gif", ".webp"), or NULL for anything else: .html, .svg, .js...
// would otherwise be stored and served from the site's own origin.
static const char *media_image_extension(const unsigned char *data, size_t len) {
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return ".jpg";
    if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return ".png";
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) return ".gif";
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return ".webp";
    return NULL;
}

// An uploaded file's name as it is stored: in the part before the last dot
// (the stem) spaces and dots become '_', every character outside
// [A-Za-z0-9_-] is dropped, and an empty result becomes "image"; the
// extension keeps only [A-Za-z0-9] (media_name_with_extension() then makes
// it the content's real one). Nothing that could break out of a path, a
// URL, an HTML attribute or image-optimizer.sh's command line survives. The
// stem is capped so "_N" and the optimizer's "_full"/"_half"... still fit.
#define MEDIA_UPLOAD_STEM_MAX 120

static void media_sanitize_upload_name(const char *raw, char *out, size_t size) {
    const char *last_dot = strrchr(raw, '.');
    const char *stem_end = last_dot ? last_dot : raw + strlen(raw);

    size_t j = 0;
    for (const char *p = raw; p < stem_end && j < MEDIA_UPLOAD_STEM_MAX && j + 1 < size; p++) {
        char c = *p;
        if (c == ' ' || c == '.') out[j++] = '_';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_') out[j++] = c;
    }
    if (j == 0) j = (size_t)snprintf(out, size, "image");

    if (last_dot && j + 1 < size) {
        size_t dot_at = j;
        out[j++] = '.';
        for (const char *p = last_dot + 1; *p && j + 1 < size; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[j++] = c;
        }
        if (j == dot_at + 1) j = dot_at; // nothing usable after the dot
    }
    out[j] = '\0';
}

// Whether an earlier upload in `dirpath` already uses `stem`: its base-name
// link (under any image extension - "photo.png" and "photo.jpg" share their
// variants) or its _full/_half variant. lstat(), since the base name is a
// symlink to _half.
static int media_stem_taken(const char *dirpath, const char *stem) {
    static const char *suffixes[] = {
        ".jpg", ".jpeg", ".png", ".gif", ".webp",
        "_full.jpg", "_full.gif", "_half.jpg", "_half.gif",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s%s", dirpath, stem, suffixes[i]);
        if (lstat(path, &st) == 0) return 1;
    }
    return 0;
}

// Gives `name` ("stem.ext") a stem no earlier upload in `dirpath` uses,
// appending "_2", "_3"... - an upload never overwrites another's files.
// Returns 0 if no free name is found.
static int media_unique_name(const char *dirpath, char *name, size_t size) {
    char stem[MEDIA_UPLOAD_STEM_MAX + 1], ext[32] = "";
    const char *dot = strrchr(name, '.');
    size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
    if (stem_len > MEDIA_UPLOAD_STEM_MAX) stem_len = MEDIA_UPLOAD_STEM_MAX;
    memcpy(stem, name, stem_len);
    stem[stem_len] = '\0';
    if (dot) snprintf(ext, sizeof(ext), "%s", dot);

    if (!media_stem_taken(dirpath, stem)) return 1;
    for (int n = 2; n < 10000; n++) {
        char candidate[MEDIA_UPLOAD_STEM_MAX + 8];
        snprintf(candidate, sizeof(candidate), "%s_%d", stem, n);
        if (!media_stem_taken(dirpath, candidate)) {
            int written = snprintf(name, size, "%s%s", candidate, ext);
            return written > 0 && (size_t)written < size;
        }
    }
    return 0;
}

// Makes `name` end in the extension of the format its content really is, so
// the stored file is always served as what it is: "photo.jpg" holding a
// WebP becomes "photo.webp", "photo" becomes "photo.jpg". An extension that
// already matches is kept as written ("photo.JPEG" stays). Returns 0 if the
// result doesn't fit in `size`.
static int media_name_with_extension(char *name, size_t size, const char *ext) {
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        if (strcasecmp(dot, ext) == 0 ||
            (strcmp(ext, ".jpg") == 0 && strcasecmp(dot, ".jpeg") == 0)) return 1;
        *dot = '\0';
    }
    size_t len = strlen(name);
    if (len + strlen(ext) + 1 > size) return 0;
    memcpy(name + len, ext, strlen(ext) + 1);
    return 1;
}

// Writes an HTML fragment (a media directory <li>) as a 200 response.
static void send_html_fragment(void *ctx, const char *html) {
    char resp[256];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %zu\r\n"
             "X-Content-Type-Options: nosniff\r\n"
             "\r\n",
             strlen(html));
    connection_write(ctx, resp, strlen(resp));
    connection_write(ctx, html, strlen(html));
}

static void send_simple(void *ctx, const char *status, const char *body) {
    char header[512];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, body_len);
    connection_write(ctx, header, strlen(header));
    connection_write(ctx, body, body_len);
}

// Computes the browser "epoch" for `req`: the configured `force_epoch`
// override, if set; otherwise a "preview_epoch" query-string override, if
// present and valid; otherwise detect_epoch(User-Agent).
//
// preview_epoch backs /dashboard/settings/preview's iframe: every dynamic
// route needs to honor it (not just "/"), because the admin clicks through
// the real site inside that iframe - keepEpochOnNavigation() in
// preview_epoch3.html appends it to every same-origin link it finds after
// each navigation, so the chosen epoch survives clicking around instead of
// reverting to whatever User-Agent the browser actually sends. Reading it
// straight off req->url (rather than requiring every call site to have
// already parsed the query string into params[]) is what lets one change
// here cover every route.
// A request with no HTTP version and no headers at all: the HTTP/0.9
// "simple request" of a 1990-92 browser. (Cello sends version-less requests
// too, but its page requests carry headers - it is classified by its
// User-Agent like any other.)
static int is_http09_request(const HttpRequest *req) {
    return req->protocol[0] == '\0' && req->header_count == 0;
}

static int resolve_epoch(HttpRequest *req) {
    if (force_epoch >= EPOCH_WML && force_epoch <= EPOCH_MODERN) return force_epoch;

    static const char preview_key[] = "preview_epoch=";
    const char *p = strstr(req->url, preview_key);
    if (p && (p == req->url || p[-1] == '?' || p[-1] == '&')) {
        int preview_epoch = atoi(p + sizeof(preview_key) - 1);
        if (preview_epoch >= EPOCH_WML && preview_epoch <= EPOCH_MODERN) return preview_epoch;
    }

    // No User-Agent to classify, and a browser older than tables: text.
    if (is_http09_request(req)) return EPOCH_PRESTANDARD;

    return detect_epoch(get_header_value(req, "User-Agent"));
}

// Whether `req` may use /login and /dashboard: an epoch 3 page (after
// force_epoch / ?preview_epoch) for a browser the detection rules grant the
// dashboard - whose stylesheet needs more than the public epoch 3 one. The
// second half reads the real User-Agent, so ?preview_epoch=3 cannot lift an
// older browser into the dashboard.
static int dashboard_allowed(HttpRequest *req, int epoch) {
    return epoch == EPOCH_MODERN && detect_dashboard(get_header_value(req, "User-Agent"));
}

// Renders the centralized error_epoch<N>.html template via error_content() +
// buildPageWebSite(), and sends it with `status_line`. Falls back to
// send_simple() if the template could not be loaded/rendered.
static void send_error_response(void *ctx, int status_code, const char *status_line, int epoch) {
    char title[64];
    snprintf(title, sizeof(title), "{{SITE_NAME}} - Error %d", status_code);

    char *content  = error_content(epoch, status_code, NULL);
    char *body     = buildPageWebSite(epoch, title, content);
    char *response = body ? build_epoch_response_status(body, "", epoch, status_line) : NULL;
    free(body);

    if (!response) {
        // Last-resort fallback: template loading itself failed, so this must
        // not depend on the filesystem/template pipeline. Plain text only.
        send_simple(ctx, status_line, status_line);
        return;
    }

    connection_write(ctx, response, strlen(response));
    free(response);
}

// Sends a malloc'd full HTTP response, truncating to headers-only for HEAD
// requests, and frees it. If `response` is NULL (some step failed), sends an
// epoch-aware 500 error page instead.
static void send_or_error(void *ctx, char *response, const char *method, int epoch) {
    if (!response) {
        send_error_response(ctx, 500, "500 Internal Server Error", epoch);
        return;
    }

    size_t response_len = strlen(response);
    if (strcmp(method, "HEAD") == 0) {
        const char *header_end = strstr(response, "\r\n\r\n");
        response_len = header_end ? (size_t)(header_end - response) + 4 : response_len;
    }
    connection_write(ctx, response, response_len);
    free(response);
}

// Looks up db.entries.findOne({link, enabled: true}) and renders it via
// entry_page() + buildPageWebSite(), used by both /page/<link>
// (expected_type "page") and /blog/<link> (expected_type "blog"). Sends a
// 404 if the entry doesn't exist, mongodb is not ready, or entry.type !=
// expected_type. Takes ownership of `category_menu_html` (pass NULL for
// non-blog pages).
// include_drafts: the visitor holds a dashboard session, so an unpublished
// entry is served too (a preview from the editor) instead of a 404 - marked
// "[Draft]" in its title and kept out of any cache.
// `extra_html` (may be NULL; always taken over) goes after the entry's own
// content - a blog entry's comments section.
static void serve_cms_entry(void *ctx, const char *link, const char *expected_type,
                             const char *lang, const char *method, int epoch,
                             char *category_menu_html, int include_drafts, char *extra_html) {
    CmsEntry entry;
    if (mongodb_manager_is_ready() && cms_get_entry_by_link(link, lang, include_drafts, &entry)) {
        if (strcmp(entry.type, expected_type) != 0) {
            cms_entry_free(&entry);
            free(category_menu_html);
            free(extra_html);
            send_error_response(ctx, 404, "404 Not Found", epoch);
            return;
        }

        // For blog entries highlight the "/blog" menu item (section match).
        // For pages use the full "/page/<link>" URL (menu items point to specific pages).
        char current_url[256];
        if (strcmp(expected_type, "blog") == 0)
            snprintf(current_url, sizeof(current_url), "/blog");
        else
            snprintf(current_url, sizeof(current_url), "/page/%s", link);

        int draft = !entry.enabled;
        char *title   = draft ? render_template("[Draft] %s", entry.header_title)
                              : strdup(entry.header_title);
        char *content = entry_page(&entry, epoch);
        cms_entry_free(&entry);
        if (content && extra_html && extra_html[0]) content = str_append(content, extra_html);
        free(extra_html);
        extra_html = NULL;

        char *body = NULL;
        if (content && title) {
            body = buildEntryWebSiteAtUrl(epoch, title, content, current_url, category_menu_html);
            category_menu_html = NULL; // ownership transferred
        } else {
            free(content);
            free(category_menu_html);
        }
        char *response = body ? build_epoch_response(body, draft ? "Cache-Control: no-store\r\n" : "",
                                                      epoch) : NULL;
        free(title);
        free(body);
        send_or_error(ctx, response, method, epoch);
    } else {
        free(category_menu_html);
        free(extra_html);
        send_error_response(ctx, 404, "404 Not Found", epoch);
    }
}

// "Back to <entry title>" for a QR page's link back to `back` (a
// "/blog/<link>" or "/page/<link>" path), its title looked up here rather
// than carried in the QR link's own URL; just "Back" for any other path, or
// an entry that can't be found.
static void qr_back_label(const char *back, const char *lang, char *out, size_t out_size) {
    snprintf(out, out_size, "Back");
    const char *link = NULL;
    if (!strncmp(back, "/blog/", 6)) link = back + 6;
    else if (!strncmp(back, "/page/", 6)) link = back + 6;
    if (!link || !*link || !mongodb_manager_is_ready()) return;

    char slug[256];
    snprintf(slug, sizeof(slug), "%s", link);
    slug[strcspn(slug, "?#")] = '\0';

    CmsEntry entry;
    if (cms_get_entry_by_link(slug, lang, 0, &entry) && entry.header_title && entry.header_title[0])
        snprintf(out, out_size, "Back to %s", entry.header_title);
    cms_entry_free(&entry);
}

// Whether this request carries a valid dashboard session - what lets
// serve_cms_entry() show a draft.
static int viewer_can_preview_drafts(HttpRequest *req) {
    if (!mongodb_manager_is_ready()) return 0;
    char user_id[USER_ID_HEX_BUF_SIZE], role[USER_ROLE_BUF_SIZE];
    // The content team only: not a moderator (comments), nor a commenter.
    return validate_session_cookie(get_header_value(req, "Cookie"), user_id) == 1 &&
           cms_get_user_role(user_id, role, sizeof(role)) == 0 &&
           strcmp(role, "moderator") != 0 && strcmp(role, "commenter") != 0;
}

// Shared by every /dashboard/* sub-route: sends a 503 and returns 0 if
// mongodb is not ready; sends a 302 to /login and returns 0 if `req` has no
// valid session cookie; otherwise writes the session's user id into
// user_id_out (>= USER_ID_HEX_BUF_SIZE bytes) and returns 1. Callers must not
// write to `ctx` if this returns 0.
static int parse_urlencoded_field(const char *body, int body_length, const char *key,
                                   char *out, size_t out_size);

// The CSRF token a state-changing request carries: the X-CSRF-Token header
// (fetch/XHR, set by html/assets/js/csrf.js), else the csrf_token field of
// an urlencoded or multipart body (HTML forms, same script). Writes "" when
// there is none.
static void request_csrf_token_value(HttpRequest *req, char *out, size_t out_size) {
    out[0] = '\0';

    const char *header = get_header_value(req, CSRF_HEADER_NAME);
    if (header && header[0]) {
        strncpy(out, header, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    const char *ct = get_header_value(req, "Content-Type");
    if (!ct) return;

    if (strncasecmp(ct, "application/x-www-form-urlencoded", 33) == 0) {
        parse_urlencoded_field(req->body, req->body_length, CSRF_FIELD_NAME, out, out_size);
    } else if (strncasecmp(ct, "multipart/form-data", 19) == 0) {
        MultipartResult *mp = parse_multipart(req->body, (size_t)req->body_length, ct);
        const MultipartPart *part = mp ? multipart_find(mp, CSRF_FIELD_NAME) : NULL;
        if (part) {
            size_t len = part->data_len < out_size - 1 ? part->data_len : out_size - 1;
            memcpy(out, part->data, len);
            out[len] = '\0';
        }
        free_multipart(mp);
    }
}

static int is_safe_method(const char *method) {
    return strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0 ||
           strcmp(method, "OPTIONS") == 0;
}

// 1 if `url`'s path (query string aside) is exactly `path`.
static int url_path_is(const char *url, const char *path) {
    size_t len = strlen(path);
    return strncmp(url, path, len) == 0 && (url[len] == '\0' || url[len] == '?');
}

// The dashboard POSTs that change nothing - they compute something from the
// request and return it - and so stay open to a viewer: the entry editor's
// block preview and the browser-detection rules' dry run.
static int is_read_only_post(const char *url) {
    return url_path_is(url, "/dashboard/api/block-preview") ||
           url_path_is(url, "/dashboard/api/browsers/preview");
}

// A viewer's (Observador's) attempt to change something: JSON for the
// dashboard's AJAX endpoints, which their scripts read, else the error page
// - which, being under /dashboard, carries the read-only notice too.
static void send_read_only_refusal(void *ctx, HttpRequest *req, int epoch) {
    if (strncmp(req->url, "/dashboard/api/", 15) == 0) {
        char *response = build_json_response_status(
            "{\"ok\":false,\"error\":\"read-only account\"}", "403 Forbidden");
        send_or_error(ctx, response, req->method, epoch);
        return;
    }
    char *content  = error_content(epoch, 403,
        "This is a read-only account: it can open every page and see every value, but it can't "
        "save changes.");
    char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content) : NULL;
    char *response = body ? build_epoch_response_status(body, "", epoch, "403 Forbidden") : NULL;
    free(body);
    send_or_error(ctx, response, req->method, epoch);
}

#define MAINTENANCE_LOGIN_MESSAGE \
    "The site is under maintenance: only administrators can sign in right now."

// Under construction mode (cms_get_under_construction()) closes the
// dashboard to every role but an Administrador's - at sign-in, and for a
// session already open when the mode was turned on. Fails closed: a role
// that can't be read is locked out too.
static int maintenance_locks_out(const char *user_id) {
    bool under_construction = false;
    cms_get_under_construction(&under_construction, NULL);
    if (!under_construction) return 0;

    char role[USER_ROLE_BUF_SIZE];
    return cms_get_user_role(user_id, role, sizeof(role)) != 0 || strcmp(role, "admin") != 0;
}

// Ends `req`'s session (a non-admin's, see maintenance_locks_out()) and
// answers 503: the login page with MAINTENANCE_LOGIN_MESSAGE, clearing the
// cookie - JSON under /dashboard/api/, whose callers are scripts.
static void send_maintenance_lockout(void *ctx, HttpRequest *req, int epoch) {
    char token[SESSION_TOKEN_BUF_SIZE];
    if (extract_session_token(get_header_value(req, "Cookie"), token, sizeof(token)))
        destroy_session(token);
    request_user_set(NULL); // the page below is a signed-out one: no user link, no CSRF token

    if (strncmp(req->url, "/dashboard/api/", 15) == 0) {
        char *response = build_json_response_status(
            "{\"ok\":false,\"error\":\"" MAINTENANCE_LOGIN_MESSAGE "\"}", "503 Service Unavailable");
        send_or_error(ctx, response, req->method, epoch);
        return;
    }

    char clear_cookie[128];
    build_session_clear_cookie_header(clear_cookie, sizeof(clear_cookie));
    char *content  = login(epoch, MAINTENANCE_LOGIN_MESSAGE);
    char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content);
    char *response = body ? build_epoch_response_status(body, clear_cookie, epoch,
                                                        "503 Service Unavailable") : NULL;
    free(body);
    send_or_error(ctx, response, req->method, epoch);
}

// What each role may open in the dashboard. A commenter never signs in
// (auth.c refuses it, and making someone a commenter ends their sessions),
// so a session that still says "commenter" opens nothing; a moderator opens
// the dashboard home and the comment pages. Every other role, everything
// (each route still checks its own rule: admin only, own entries...).
static int role_may_open(const char *role, const char *url) {
    if (strcmp(role, "commenter") == 0) return 0;
    if (strcmp(role, "moderator") != 0) return 1;
    size_t n = strcspn(url, "?#");
    if (n == 10 && strncmp(url, "/dashboard", 10) == 0) return 1;
    return n >= 19 && strncmp(url, "/dashboard/comments", 19) == 0 && (n == 19 || url[19] == '/');
}

static int require_dashboard_session_ex(void *ctx, HttpRequest *req, int epoch, char *user_id_out,
                                        char *role_out, size_t role_size);

static int require_dashboard_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    char role[USER_ROLE_BUF_SIZE];
    return require_dashboard_session_ex(ctx, req, epoch, user_id_out, role, sizeof(role));
}

static int require_dashboard_session_ex(void *ctx, HttpRequest *req, int epoch, char *user_id_out,
                                        char *role_out, size_t role_size) {
    if (!mongodb_manager_is_ready()) {
        send_error_response(ctx, 503, "503 Service Unavailable", epoch);
        return 0;
    }

    const char *cookie = get_header_value(req, "Cookie");
    if (validate_session_cookie(cookie, user_id_out) != 1) {
        char *response = build_redirect_response("/login", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    if (maintenance_locks_out(user_id_out)) {
        send_maintenance_lockout(ctx, req, epoch);
        return 0;
    }

    // The session's user must still exist: a role that can't be read opens
    // nothing (it used to fall back to "admin").
    if (cms_get_user_role(user_id_out, role_out, role_size) != 0) {
        char *response = build_redirect_response("/login", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }
    if (!role_may_open(role_out, req->url)) {
        const char *home = strcmp(role_out, "moderator") == 0 ? "/dashboard" : "/";
        char *response = build_redirect_response(home, "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    // Every state-changing dashboard route goes through here, so this is the
    // one CSRF check: the token is bound to the session cookie (see
    // derive_csrf_token()), which a cross-site page can neither read nor
    // forge. SameSite=Lax on the cookie remains as a second layer.
    if (!is_safe_method(req->method)) {
        char supplied[CSRF_TOKEN_BUF_SIZE + 1];
        request_csrf_token_value(req, supplied, sizeof(supplied));
        if (!verify_csrf_token(cookie, supplied)) {
            LOG_WARN("CSRF check failed: %s %s", req->method, req->url);
            send_simple(ctx, "403 Forbidden", "Invalid or missing CSRF token. Reload the page and try again.");
            return 0;
        }

        // A viewer reads everything and writes nothing. Every dashboard
        // write is a POST through here (every GET only reads), so this one
        // check covers every route, present and future. Fails closed: a
        // role that can't be read can't write either.
        if (!is_read_only_post(req->url)) {
            if (strcmp(role_out, "viewer") == 0) {
                LOG_INFO("Read-only account: %s %s refused", req->method, req->url);
                send_read_only_refusal(ctx, req, epoch);
                return 0;
            }
        }
    }

    return 1;
}

// Whether `role` sees what an Administrador sees: "admin", or "viewer" - the
// read-only role, whose writes require_dashboard_session() refuses.
static int role_sees_everything(const char *role) {
    return strcmp(role, "admin") == 0 || strcmp(role, "viewer") == 0;
}

// Who answers visitors' comments: an Administrador (any comment) or an Autor
// (comments on their own blog entries) - not the Moderador or the Observador.
static int role_may_reply(const char *role) {
    return strcmp(role, "admin") == 0 || strcmp(role, "author") == 0;
}

// A moderation list tab from a "back" value; "pending" for anything else.
static const char *comments_tab(const char *back) {
    static const char *TABS[] = { "approved", "rejected", "unverified" };
    for (size_t i = 0; back && i < sizeof(TABS) / sizeof(TABS[0]); i++)
        if (strcmp(back, TABS[i]) == 0) return TABS[i];
    return "pending";
}

// require_dashboard_session() + cms_get_user_role(): role_out (>= USER_ROLE_BUF_SIZE
// bytes) is "admin", "author" or "viewer" (a missing `role` field, or a lookup error, defaults
// to "admin" - same backward-compatible convention as cms_get_user_role()). Same
// 0/1 return + ctx-write contract as require_dashboard_session().
static int require_dashboard_session_role(void *ctx, HttpRequest *req, int epoch,
                                            char *user_id_out, char *role_out, size_t role_size) {
    return require_dashboard_session_ex(ctx, req, epoch, user_id_out, role_out, role_size);
}

// require_dashboard_session_role() + a role that doesn't see everything ->
// 302 /dashboard, return 0. Used by routes reserved to Administrador (Users
// maintainer, entry deletion, settings...) - which a viewer may open too,
// since require_dashboard_session() already refused its writes.
static int require_admin_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    char role[USER_ROLE_BUF_SIZE];
    if (!require_dashboard_session_role(ctx, req, epoch, user_id_out, role, sizeof(role))) return 0;

    if (!role_sees_everything(role)) {
        char *response = build_redirect_response("/dashboard", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    return 1;
}

// The reason phrase for an extension's status code - only the code comes
// from the extension, so nothing it sends lands in the status line.
static const char *extension_status_line(int status) {
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 422: return "422 Unprocessable Content";
    case 503: return "503 Service Unavailable";
    default:  return status >= 500 ? "500 Internal Server Error"
                   : status >= 400 ? "400 Bad Request" : "200 OK";
    }
}

// /dashboard/ext/<name>[/...]: an enabled extension's pages (see
// extensions/extensions.h). Same gate as every dashboard page - epoch 3, a
// session, CSRF on writes, a viewer's writes refused - then the request goes
// to the extension with the user it was verified for, and its answer comes
// back as: an HTML fragment wrapped in the site's layout, a redirect kept
// under the extension's mount, or JSON passed through.
static void serve_extension(void *ctx, HttpRequest *req, const char *decoded_url) {
    int epoch = resolve_epoch(req);
    if (!dashboard_allowed(req, epoch)) {
        char *response = build_redirect_response("/dashboard", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return;
    }

    char user_id[USER_ID_HEX_BUF_SIZE];
    char role[USER_ROLE_BUF_SIZE];
    if (!require_dashboard_session_role(ctx, req, epoch, user_id, role, sizeof(role))) return;

    const char *rest = decoded_url + strlen(EXTENSION_ROUTE_PREFIX);
    size_t name_len = strcspn(rest, "/");
    char name[EXTENSION_NAME_MAX + 1];
    ExtensionInfo info;
    if (name_len == 0 || name_len >= sizeof(name)) {
        send_error_response(ctx, 404, "404 Not Found", epoch);
        return;
    }
    memcpy(name, rest, name_len);
    name[name_len] = '\0';
    if (!extensions_get(name, &info) || !info.enabled) {
        send_error_response(ctx, 404, "404 Not Found", epoch);
        return;
    }

    // The target as the browser sent it (still encoded, query included),
    // minus the mount. The name is [a-z0-9_-], so it reads the same encoded.
    char mount[sizeof(EXTENSION_ROUTE_PREFIX) + EXTENSION_NAME_MAX + 1];
    snprintf(mount, sizeof(mount), EXTENSION_ROUTE_PREFIX "%s", name);
    const char *tail = req->url + strlen(mount);
    char target[2048];
    snprintf(target, sizeof(target), "%s%s", tail[0] == '/' ? "" : "/", tail);

    ExtensionRequest rq = {
        .method = req->method,
        .mount = mount,
        .target = target,
        .content_type = get_header_value(req, "Content-Type"),
        .body = req->body,
        .body_len = req->body ? (size_t)req->body_length : 0,
        .user_id = user_id,
        .user_name = request_user_name(),
        .role = role,
        .lang = request_lang(),
        .theme = request_theme(),
        .epoch = epoch,
    };
    ExtensionResponse rs;
    int rc = extensions_forward(name, &rq, &rs);
    if (rc != 0) {
        send_error_response(ctx, rc == -2 ? 502 : 503,
                            rc == -2 ? "502 Bad Gateway" : "503 Service Unavailable", epoch);
        return;
    }

    if (rs.status >= 300 && rs.status < 400) {
        // Only a path inside the mount: an extension can't send the browser
        // off-site, nor elsewhere on this one.
        if (rs.location[0] == '/' && rs.location[1] != '/' && !strchr(rs.location, '\\')) {
            char location[sizeof(mount) + sizeof(rs.location)];
            snprintf(location, sizeof(location), "%s%s", mount, rs.location);
            send_or_error(ctx, build_redirect_response(location, "", epoch), req->method, epoch);
        } else {
            LOG_WARN("Extension %s: redirect outside its mount refused: %s", name, rs.location);
            send_error_response(ctx, 502, "502 Bad Gateway", epoch);
        }
    } else if (strncasecmp(rs.content_type, "text/html", 9) == 0) {
        char title[sizeof(rs.title) + 32];
        snprintf(title, sizeof(title), "{{SITE_NAME}} - %s", rs.title[0] ? rs.title : info.title);
        char *body = buildPageWebSite(epoch, title, rs.body);   // takes rs.body
        rs.body = NULL;
        char *response = body ? build_epoch_response_status(body, "", epoch,
                                                            extension_status_line(rs.status)) : NULL;
        free(body);
        send_or_error(ctx, response, req->method, epoch);
    } else if (strncasecmp(rs.content_type, "application/json", 16) == 0) {
        char *response = build_json_response_status(rs.body, extension_status_line(rs.status));
        if (response) {
            connection_write(ctx, response, strcmp(req->method, "HEAD") == 0
                                             ? (size_t)(strstr(response, "\r\n\r\n") + 4 - response)
                                             : strlen(response));
            free(response);
        } else {
            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
        }
    } else {
        LOG_WARN("Extension %s: unsupported Content-Type \"%s\"", name, rs.content_type);
        send_error_response(ctx, 502, "502 Bad Gateway", epoch);
    }
    free(rs.body);
}

// Renders /dashboard/settings into a response: `site_name` in its form,
// `error_message` above it, and the home page / under construction forms
// showing `home` - or, when NULL, what site_settings holds now.
static void send_settings_general_page(void *ctx, HttpRequest *req, int epoch, const char *lang,
                                       const char *site_name, const SiteHomeSettings *home,
                                       const char *error_message, const char *mail_notice) {
    SiteHomeSettings stored = { 0 };
    char *stored_page = NULL;
    if (!home) {
        stored.hide_home_blog = cms_get_hide_home_blog();
        cms_get_under_construction(&stored.under_construction, &stored_page);
        stored.under_construction_page = stored_page ? stored_page : "";
        home = &stored;
    }

    char *content  = site_settings_general_page(epoch, lang, site_name, home, error_message, mail_notice);
    free(stored_page);
    char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
    free(body);
    send_or_error(ctx, response, req->method, epoch);
}

// The routes that show the site's own content - home, pages, the blog and
// the retro menu page - and so the ones the under-construction page stands
// in for. Everything else stays reachable: /login and /dashboard, the
// language/theme selectors, QR and gallery pages, CSS and static files (the
// under-construction page's own images and styles among them).
static int is_site_content_route(const char *url) {
    return strcmp(url, "/") == 0 || strncmp(url, "/page/", 6) == 0 ||
           strcmp(url, "/blog") == 0 || strncmp(url, "/blog/", 6) == 0 ||
           strcmp(url, "/menu") == 0 || strcmp(url, "/search") == 0;
}

// While site_settings.under_construction is on, sends the home page with the
// chosen page entry's content blocks (drafts too) in place of the home
// content, and no menu or blog list - or, if the entry can't be found, the
// 503 error content there, so the site stays closed - and returns 1. Returns 0, sending
// nothing, when the mode is off. The caller skips this for an Administrador
// (request_user_is_admin()). no-store, so nobody keeps it once the site opens.
static int serve_under_construction(void *ctx, HttpRequest *req, const char *lang) {
    bool enabled = false;
    char *link = NULL;
    cms_get_under_construction(&enabled, &link);
    if (!enabled) {
        free(link);
        return 0;
    }

    int epoch = resolve_epoch(req);
    char *title = NULL;
    char *content = NULL;

    CmsEntry entry = { 0 };
    if (link && link[0] && cms_get_entry_by_link(link, lang, 1, &entry) &&
        strcmp(entry.type, "page") == 0) {
        title   = strdup(entry.header_title && entry.header_title[0] ? entry.header_title
                                                                  : "{{SITE_NAME}}");
        content = home_content_entry(&entry, epoch);
    } else {
        LOG_WARN("Maintenance mode: page '%s' not found", link ? link : "");
        title   = strdup("{{SITE_NAME}} - Maintenance");
        content = error_content(epoch, 503, NULL);
    }
    cms_entry_free(&entry);
    free(link);

    char *body = title && content ? buildUnderConstructionWebSite(epoch, title, content)
                                  : (free(content), NULL);
    char *response = body ? build_epoch_response(body, "Cache-Control: no-store\r\n", epoch) : NULL;
    free(title);
    free(body);
    send_or_error(ctx, response, req->method, epoch);
    return 1;
}

// The signed-in user's password, typed again (the `password` form field) -
// for actions that run code as root, so an open session alone isn't enough.
// On success copies the user's email into email_out and returns 1.
static int confirm_password(const HttpRequest *req, const char *user_id, char *email_out, size_t email_size) {
    char password[256] = "";
    char role[USER_ROLE_BUF_SIZE];
    parse_urlencoded_field(req->body, req->body_length, "password", password, sizeof(password));
    if (!password[0] || cms_get_user_values(user_id, email_out, email_size, role, sizeof(role)) != 0) return 0;

    char *id = auth_login_user(email_out, password);
    int ok = id && strcmp(id, user_id) == 0;
    free(id);
    sodium_memzero(password, sizeof(password));
    if (!ok) LOG_WARN("updates: wrong password confirming an update action (%s)", email_out);
    return ok;
}

// 1 if `role` is "admin" (or "viewer", who can open any entry in the editor
// and save none of it), or `role` is "author" and `entry` is a "blog" entry
// created by `user_id`. Gates the entry editor GET/POST routes - an Autor may
// only edit their own blog entries.
static int can_edit_entry(const char *role, const char *user_id, const CmsEntryEdit *entry) {
    if (role_sees_everything(role)) return 1;

    return strcmp(role, "author") == 0 &&
           strcmp(entry->type, "blog") == 0 &&
           strcmp(entry->created_by, user_id) == 0;
}

// Matches `decoded_url` against "<prefix>/<id><suffix>", where <id> is a
// single non-empty path segment (no '/'). On match, copies <id> into id_out
// (truncated to id_size - 1) and returns 1. `suffix` may be "" (the id is
// the last path component) or e.g. "/edit". Returns 0 on no match (id_out
// untouched).
static int match_id_route(const char *decoded_url, const char *prefix,
                           const char *suffix, char *id_out, size_t id_size) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(decoded_url, prefix, prefix_len) != 0 || decoded_url[prefix_len] != '/')
        return 0;

    const char *seg_start = decoded_url + prefix_len + 1;
    const char *slash = strchr(seg_start, '/');
    const char *seg_end;

    if (suffix[0] == '\0') {
        if (slash) return 0;
        seg_end = seg_start + strlen(seg_start);
    } else {
        if (!slash || strcmp(slash, suffix) != 0) return 0;
        seg_end = slash;
    }

    size_t seg_len = (size_t)(seg_end - seg_start);
    if (seg_len == 0 || seg_len >= id_size) return 0;

    memcpy(id_out, seg_start, seg_len);
    id_out[seg_len] = '\0';
    return 1;
}

// Matches "/themes/<key>/styles_epoch3.css" or "/themes/<key>/styles_admin_epoch3.css"
// exactly, extracting <key> and which stylesheet it is - the dynamic route that
// serves a theme's (possibly DB-customized, see cms_get_theme_css()) epoch 3
// stylesheets, ahead of the generic static-file fallback at the bottom of the
// GET dispatch chain.
static int match_theme_css_url(const char *decoded_url, char *key_out, size_t key_size,
                               ThemeCssSheet *sheet_out) {
    static const char *prefix = "/themes/";
    static const ThemeCssSheet sheets[] = { THEME_CSS_PUBLIC, THEME_CSS_ADMIN };
    size_t prefix_len = strlen(prefix);
    size_t url_len = strlen(decoded_url);
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    for (size_t i = 0; i < sizeof(sheets) / sizeof(sheets[0]); i++) {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "/%s", cms_theme_css_file(sheets[i]));
        size_t suffix_len = strlen(suffix);

        if (url_len <= prefix_len + suffix_len) continue;
        if (strcmp(decoded_url + url_len - suffix_len, suffix) != 0) continue;

        size_t key_len = url_len - prefix_len - suffix_len;
        if (key_len == 0 || key_len >= key_size) return 0;

        memcpy(key_out, decoded_url + prefix_len, key_len);
        key_out[key_len] = '\0';
        *sheet_out = sheets[i];
        return 1;
    }
    return 0;
}

// Matches the CSS editor's routes: "/dashboard/settings/themes/<key>/css"
// (public stylesheet) or ".../admin-css" (admin stylesheet), each followed
// by `tail` - "" for the editor itself, "/restore" for its restore form.
// Writes <key> and the stylesheet; returns 0 when `decoded_url` is neither.
static int match_theme_css_editor(const char *decoded_url, const char *tail,
                                  char *key_out, size_t key_size, ThemeCssSheet *sheet_out) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "/css%s", tail);
    if (match_id_route(decoded_url, "/dashboard/settings/themes", suffix, key_out, key_size)) {
        *sheet_out = THEME_CSS_PUBLIC;
        return 1;
    }
    snprintf(suffix, sizeof(suffix), "/admin-css%s", tail);
    if (match_id_route(decoded_url, "/dashboard/settings/themes", suffix, key_out, key_size)) {
        *sheet_out = THEME_CSS_ADMIN;
        return 1;
    }
    return 0;
}

// Where the CSS editor of `sheet` lives, for the redirect after a save/restore.
static void theme_css_editor_url(char *out, size_t out_size, const char *key, ThemeCssSheet sheet) {
    snprintf(out, out_size, "/dashboard/settings/themes/%s/%s", key,
             sheet == THEME_CSS_ADMIN ? "admin-css" : "css");
}

// Theme-asset endpoints (/dashboard/api/theme-assets/*) back the banner,
// footer and logo editors' image upload/browse widget: they write into
// html/content/themes/<theme>/<component>/epoch<N>/ - site content, next to
// the media library's html/content/posts/, never the theme's own
// html/themes/<theme>/assets/, which ships with Boat Rudder and an update
// overwrites. So the widget only lists (and can only delete) the admin's
// uploads, never the images the theme's default markup uses.
//
// `key` is the theme being edited - taken explicitly from the request
// (query string), never from request_theme() (the admin's own active
// theme for this request, which need not be the theme whose banner/footer/
// logo editor is open - see theme-scoped-personalization-plan.md §4).
// `component` is the editor's own name: "banner", "footer", "logo" or
// "home-blog" (the home blog list's background image);
// `epoch_str` must be one of "-1".."3". Fills dir_out (>= 256 bytes) with
// "./html/content/themes/<key>/<component>/epoch<epoch_str>" and returns 1,
// or returns 0 (dir_out untouched) if any argument is invalid.
static int theme_assets_dir(const char *key, const char *component, const char *epoch_str,
                             char *dir_out, size_t dir_size) {
    if (!theme_key_is_valid(key)) return 0;
    if (!component || !epoch_str) return 0;
    if (strcmp(component, "banner") != 0 && strcmp(component, "footer") != 0 &&
        strcmp(component, "logo") != 0 && strcmp(component, "home-blog") != 0)
        return 0;

    static const char *valid_epochs[] = {"-1", "0", "1", "2", "3"};
    int epoch_ok = 0;
    for (size_t i = 0; i < sizeof(valid_epochs) / sizeof(valid_epochs[0]); i++) {
        if (strcmp(epoch_str, valid_epochs[i]) == 0) { epoch_ok = 1; break; }
    }
    if (!epoch_ok) return 0;

    int n = snprintf(dir_out, dir_size, "./html/content/themes/%s/%s/epoch%s",
                      key, component, epoch_str);
    return n > 0 && (size_t)n < dir_size;
}

// mkdir() one path component at a time - unlike media uploads (whose parent
// directory is always created up front by the "new directory" route), a
// theme-asset directory only exists once something was uploaded to it.
static void mkdir_recursive(const char *dir) {
    char tmp[512];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

// Sanitizes an uploaded/targeted filename in place: strips anything but
// alphanumerics/-/./_ (spaces become '-'), same whitelist already used by
// the media library's upload handler. Also rejects a name that is empty,
// starts with '.' (hidden file / "." / ".."), or still contains "..".
static int sanitize_asset_filename(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t k = 0; in[k] && j < out_size - 1; k++) {
        char c = in[k];
        if (c == ' ') out[j++] = '-';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
            out[j++] = c;
    }
    out[j] = '\0';
    return j > 0 && out[0] != '.' && !strstr(out, "..");
}

static int has_extension_ci(const char *filename, const char *ext) {
    size_t flen = strlen(filename), elen = strlen(ext);
    return flen > elen && strcasecmp(filename + flen - elen, ext) == 0;
}

// Extension allowlist for the theme-assets upload endpoint (banner/footer/
// logo image widgets) - the endpoint had none before the logo feature
// needed one, since epoch -1's uploads must be an image stb_image.h can
// decode (see image_convert_to_wbmp() below), not an arbitrary file.
static int theme_asset_image_extension_ok(const char *filename) {
    return has_extension_ci(filename, ".png") || has_extension_ci(filename, ".jpg") ||
           has_extension_ci(filename, ".jpeg") || has_extension_ci(filename, ".gif");
}

// Epoch -1 (WAP/WML) uploads, every component: a PNG only. The PNG is kept -
// the source for any later edit, and what the dashboard's WAP preview shows
// (browsers can't read WBMP) - and the upload handler writes its 1-bit WBMP
// twin next to it, the file real devices get (see wap_twin_name()). A direct
// .wbmp upload isn't offered: nobody should have to hand-produce one.
static int epoch_neg1_upload_extension_ok(const char *filename) {
    return has_extension_ci(filename, ".png");
}

// An epoch -1 image's twin: "logo.png" <-> "logo.wbmp" - the same base name,
// `ext` (".png"/".wbmp") instead of its extension. Returns 0 if `name` has
// no extension or the result doesn't fit.
static int wap_twin_name(const char *name, const char *ext, char *out, size_t out_size) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    int n = snprintf(out, out_size, "%.*s%s", (int)(dot - name), name, ext);
    return n > 0 && (size_t)n < out_size;
}

// The site favicons uploaded from /dashboard/settings (see page_layout.h's
// page_layout_favicon_path()): a real .ico, at most FAVICON_MAX_BYTES, and
// for epoch 2 - whose browsers read only the 16x16 image - one that has a
// 16x16 entry. Checks the ICONDIR header and that every entry's image lies
// inside the file. Returns NULL when `data` is acceptable, else the error
// code /dashboard/settings shows a message for.
#define FAVICON_MAX_BYTES (256 * 1024)

static const char *favicon_ico_problem(const unsigned char *data, size_t len, int epoch) {
    if (len == 0) return "favicon-type";
    if (len > FAVICON_MAX_BYTES) return "favicon-size";
    if (len < 6 || data[0] != 0 || data[1] != 0 || data[2] != 1 || data[3] != 0)
        return "favicon-type";

    size_t count = (size_t)data[4] | ((size_t)data[5] << 8);
    if (count == 0 || 6 + 16 * count > len) return "favicon-type";

    int has_16 = 0;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *e = data + 6 + 16 * i;
        size_t size = (size_t)e[8] | ((size_t)e[9] << 8) | ((size_t)e[10] << 16) | ((size_t)e[11] << 24);
        size_t off  = (size_t)e[12] | ((size_t)e[13] << 8) | ((size_t)e[14] << 16) | ((size_t)e[15] << 24);
        if (size == 0 || off > len || size > len - off) return "favicon-type";
        if (e[0] == 16 && e[1] == 16) has_16 = 1;
    }
    if (epoch == EPOCH_MIDDLE && !has_16) return "favicon-16";
    return NULL;
}

// Builds {"files":["name1","name2",...]} for the banner/footer editor's
// asset browser. Returns a malloc'd string, always valid JSON (an invalid
// component/epoch or an unreadable directory yields an empty list, not an
// error - this endpoint only helps the admin find a path to paste, nothing
// depends on it being exhaustive).
static char *theme_assets_list_json(const char *key, const char *component, const char *epoch_str) {
    char dir[256];
    if (!theme_assets_dir(key, component, epoch_str, dir, sizeof(dir)))
        return strdup("{\"files\":[]}");

    DIR *d = opendir(dir);
    if (!d) return strdup("{\"files\":[]}");

    char *json = strdup("{\"files\":[");
    int first = 1;
    struct dirent *entry;
    while (json && (entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *escaped = json_escape_alloc(entry->d_name);
        if (!escaped) continue;

        char *piece = render_template(first ? "\"%s\"" : ",\"%s\"", escaped);
        free(escaped);
        if (piece) { json = str_append(json, piece); first = 0; }
        free(piece);
    }
    closedir(d);

    if (!json) return strdup("{\"files\":[]}");
    json = str_append(json, "]}");
    return json ? json : strdup("{\"files\":[]}");
}

// Matches "/dashboard/api/entries/<entry_id>/blocks/<block_id>/delete". On
// match, copies both ids (truncated to their buffer sizes) and returns 1; 0
// on no match (out buffers untouched).
static int match_block_delete_route(const char *decoded_url, char *entry_id_out,
                                     size_t entry_id_size, char *block_id_out,
                                     size_t block_id_size) {
    static const char prefix[] = "/dashboard/api/entries/";
    static const char mid[]    = "/blocks/";
    static const char suffix[] = "/delete";

    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    const char *entry_start = decoded_url + prefix_len;
    const char *mid_start = strstr(entry_start, mid);
    if (!mid_start) return 0;

    const char *block_start = mid_start + (sizeof(mid) - 1);
    size_t block_len = strlen(block_start);
    size_t suffix_len = sizeof(suffix) - 1;
    if (block_len <= suffix_len || strcmp(block_start + (block_len - suffix_len), suffix) != 0)
        return 0;
    block_len -= suffix_len;

    size_t entry_len = (size_t)(mid_start - entry_start);
    if (entry_len == 0 || entry_len >= entry_id_size) return 0;
    if (block_len == 0 || block_len >= block_id_size) return 0;

    memcpy(entry_id_out, entry_start, entry_len);
    entry_id_out[entry_len] = '\0';
    memcpy(block_id_out, block_start, block_len);
    block_id_out[block_len] = '\0';
    return 1;
}

// Matches "/dashboard/settings/themes/<key>/<segment>/<epoch>" (`segment`
// is "banner" or "footer"; `epoch` is the trailing segment, possibly
// negative - "-1"). On match, copies key_out and epoch_out (each truncated
// to its buffer size) and returns 1; 0 on no match.
static int match_theme_epoch_route(const char *decoded_url, const char *segment,
                                    char *key_out, size_t key_size,
                                    char *epoch_out, size_t epoch_size) {
    static const char prefix[] = "/dashboard/settings/themes/";
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    const char *key_start = decoded_url + prefix_len;
    const char *slash1 = strchr(key_start, '/');
    if (!slash1) return 0;

    size_t key_len = (size_t)(slash1 - key_start);
    if (key_len == 0 || key_len >= key_size) return 0;

    size_t segment_len = strlen(segment);
    const char *segment_start = slash1 + 1;
    if (strncmp(segment_start, segment, segment_len) != 0 || segment_start[segment_len] != '/')
        return 0;

    const char *epoch_start = segment_start + segment_len + 1;
    size_t epoch_len = strlen(epoch_start);
    if (epoch_len == 0 || epoch_len >= epoch_size || strchr(epoch_start, '/')) return 0;

    memcpy(key_out, key_start, key_len);
    key_out[key_len] = '\0';
    memcpy(epoch_out, epoch_start, epoch_len);
    epoch_out[epoch_len] = '\0';
    return 1;
}

// Returns the value of a query parameter by key, or "" if not found.
static const char *get_query_param(QueryParam *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) return params[i].value;
    }
    return "";
}

// Extracts the url-decoded value of `key` from an
// "application/x-www-form-urlencoded" body into `out` (NUL-terminated,
// truncated to out_size - 1). Returns 1 if `key` was found, 0 otherwise
// (out is left as an empty string).
static int parse_urlencoded_field(const char *body, int body_length, const char *key,
                                   char *out, size_t out_size) {
    if (out_size == 0) return 0;
    out[0] = '\0';
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) return 0;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = (size_t)(field_end - (eq + 1));
            char *raw = malloc(val_len + 1);
            if (!raw) return 0;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            // Decoding only ever shrinks or preserves length (%XX -> 1 byte,
            // + -> 1 byte), so decoding in place into `raw` is safe.
            url_decode(raw, raw);
            strncpy(out, raw, out_size - 1);
            out[out_size - 1] = '\0';
            free(raw);
            return 1;
        }

        p = amp ? amp + 1 : end;
    }
    return 0;
}

// A background color field posts as two inputs - an <input type="color">
// (`color_key`) and an opacity percentage <input type="number">
// (`alpha_key`) - joined here into the single stored "#rrggbb[aa]" value via
// cms_join_hex_alpha(). Missing/unparseable fields fall back to opaque black
// (cms_join_hex_alpha()'s own invalid-input fallback) and 100% opacity.
static void parse_bg_color_field(const char *body, int body_length, const char *color_key,
                                  const char *alpha_key, char *out, size_t out_size) {
    char rgb[8] = "#000000";
    char alpha_str[8] = "100";
    parse_urlencoded_field(body, body_length, color_key, rgb, sizeof(rgb));
    parse_urlencoded_field(body, body_length, alpha_key, alpha_str, sizeof(alpha_str));
    cms_join_hex_alpha(rgb, atoi(alpha_str), out, out_size);
}

// Like parse_urlencoded_field(), but collects every value for `key` (not just
// the first), e.g. repeated "categories=..." fields from a
// <select multiple>. Each out_values[i] is malloc'd (caller must free).
// Returns the number of values found, up to max_count.
static size_t parse_urlencoded_multi(const char *body, int body_length, const char *key,
                                      char **out_values, size_t max_count) {
    size_t count = 0;
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end && count < max_count) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) break;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = (size_t)(field_end - (eq + 1));
            char *raw = malloc(val_len + 1);
            if (!raw) break;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            // Decoding only ever shrinks or preserves length, so decoding in
            // place into `raw` is safe.
            url_decode(raw, raw);
            out_values[count++] = raw;
        }

        p = amp ? amp + 1 : end;
    }

    return count;
}

// Fills values[i] (caller-allocated, lang_count entries) with the url-decoded
// "name_<langs[i].code>" field from req's body, for
// cms_create_category()/cms_update_category(). Each values[i] is malloc'd
// and must be freed by the caller.
static void parse_category_form(HttpRequest *req, const CmsLanguageItem *langs,
                                 size_t lang_count, char **values) {
    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_link/out_order/out_enabled and values[i] (caller-allocated,
// lang_count entries) from req's body's "link"/"order"/"enabled"/
// "name_<langs[i].code>" fields, for cms_create_menu_item()/
// cms_update_menu_item(). Each values[i] is malloc'd and must be freed by
// the caller. An unchecked "enabled" checkbox sends no field at all, so its
// absence means false.
static void parse_menu_form(HttpRequest *req, char *out_link, size_t out_link_size,
                             int *out_order, bool *out_enabled,
                             const CmsLanguageItem *langs, size_t lang_count, char **values) {
    parse_urlencoded_field(req->body, req->body_length, "link", out_link, out_link_size);

    char order_str[16];
    parse_urlencoded_field(req->body, req->body_length, "order", order_str, sizeof(order_str));
    *out_order = atoi(order_str);

    char enabled_str[8];
    *out_enabled = parse_urlencoded_field(req->body, req->body_length, "enabled",
                                           enabled_str, sizeof(enabled_str)) != 0;

    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_email/out_password/out_role (caller-allocated buffers) from req's
// body's "email"/"password"/"role" fields, for cms_create_user()/
// cms_update_user(). An absent "role" field defaults to "author" (the safer
// default for a new account).
static void parse_user_form(HttpRequest *req, char *out_email, size_t out_email_size,
                             char *out_password, size_t out_password_size,
                             char *out_role, size_t out_role_size) {
    parse_urlencoded_field(req->body, req->body_length, "email", out_email, out_email_size);
    parse_urlencoded_field(req->body, req->body_length, "password", out_password, out_password_size);

    if (!parse_urlencoded_field(req->body, req->body_length, "role", out_role, out_role_size)) {
        strncpy(out_role, "author", out_role_size - 1);
        out_role[out_role_size - 1] = '\0';
    }
}

void http_route(read_func_t read_func, void *ctx, const char *root_directory) {
    LOG_DEBUG("http_route() called");

    if (!ctx) {
        LOG_ERROR("http_route: ctx is NULL");
        return;
    }

    char *raw_request = malloc(RAW_REQUEST_SIZE);
    if (!raw_request) {
        LOG_ERROR("http_route: malloc failed");
        connection_close(ctx);
        return;
    }

    HttpRequest req;
    memset(&req, 0, sizeof(req));

    // --- Read headers ---
    int bytes_read = 0;
    while (bytes_read < (int)RAW_REQUEST_SIZE - 1) {
        int ret = read_func(ctx, raw_request + bytes_read,
                            RAW_REQUEST_SIZE - 1 - bytes_read);
        if (ret > 0) {
            bytes_read += ret;
            raw_request[bytes_read] = '\0';

            // A real HTTP/0.9 "simple request" is just "GET /path" - no
            // version token, and per spec no headers or blank-line
            // terminator ever follow it. That looks identical to an
            // ordinary request still mid-transmission until the request
            // line itself is checked for a version token: waiting for
            // "\r\n\r\n" here left a real Cello 1.x image fetch hanging
            // forever, since it never sends one for at least some requests.
            int has_versioned_line = 0;
            const char *line_end = strstr(raw_request, "\r\n");
            if (!line_end) line_end = strchr(raw_request, '\n');
            if (line_end) {
                size_t line_len = (size_t)(line_end - raw_request);
                if (line_len < 700) {
                    char first_line[700];
                    memcpy(first_line, raw_request, line_len);
                    first_line[line_len] = '\0';
                    char m[16], u[600], v[16];
                    int fields = sscanf(first_line, "%15s %599s %15s", m, u, v);
                    if (fields == 2) break;
                    has_versioned_line = (fields == 3);
                }
            }

            // Ordinary request: wait for the blank line ending the header
            // block. A bare '\n' (no '\r') is accepted too, rather than
            // waiting forever for a "\r\n\r\n" a line-ending quirk like that
            // would never actually send.
            if (strstr(raw_request, "\r\n\r\n") || strstr(raw_request, "\n\n")) break;

            // A real HTTP/1.0 request line with zero headers (or a few) and
            // no blank-line terminator at all - a real Cello 1.x fetching an
            // <img>, confirmed live: it sends a version token, sometimes a
            // header or two, and never the blank line that would normally
            // end the block. Waiting for one it will never send meant a
            // silent 30s hang and then no response whatsoever. A brief
            // pause to see whether more is actually still arriving tells
            // the two cases apart without misreading a merely-slow,
            // well-formed multi-packet request as complete. TLS clients are
            // never this old, so this only runs for plain connections.
            if (has_versioned_line && !((connection_ctx_t *)ctx)->ssl) {
                struct pollfd pfd = { .fd = ((connection_ctx_t *)ctx)->client_socket, .events = POLLIN };
                if (poll(&pfd, 1, 400) <= 0) break;
                continue;
            }

            if (bytes_read >= (int)RAW_REQUEST_SIZE - 1) {
                LOG_WARN("Headers exceed %d bytes", RAW_REQUEST_SIZE);
                send_error_response(ctx, 431, "431 Request Header Fields Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }
            continue;
        }
        if (ret == 0) {
            LOG_DEBUG("Peer closed connection while reading");
        } else {
            LOG_ERROR("read_func error: %d", ret);
        }
        goto cleanup;
    }

    raw_request[bytes_read] = '\0';
    if (bytes_read == 0) goto cleanup;

    // --- Read full body based on Content-Length ---
    {
        const char *hdr_end = strstr(raw_request, "\r\n\r\n");
        if (hdr_end) {
            int header_size = (int)(hdr_end + 4 - raw_request);
            int body_in_buffer = bytes_read - header_size;

            // Extract Content-Length from raw headers
            int content_length = 0, has_length = 0;
            for (const char *h = raw_request; h < hdr_end; h++) {
                if (h[0] == '\r' && h[1] == '\n' &&
                    strncasecmp(h + 2, "Content-Length:", 15) == 0) {
                    const char *v = h + 17;
                    while (*v == ' ') v++;
                    content_length = atoi(v);
                    has_length = content_length >= 0;
                    break;
                }
            }

            // Bytes past Content-Length aren't part of the body: NCSA Mosaic
            // 2 and IE 2 send a CRLF after a POST's body that the length
            // doesn't count (RFC 2616 4.1 asks servers to tolerate it). Kept,
            // it would end the last form field - the comment form's token,
            // whose shape check then fails.
            if (has_length && body_in_buffer > content_length) {
                bytes_read = header_size + content_length;
                raw_request[bytes_read] = '\0';
                body_in_buffer = content_length;
            }

            if (content_length > (int)MAX_BODY_SIZE) {
                LOG_WARN("Body too large: %d > %d", content_length, (int)MAX_BODY_SIZE);
                send_error_response(ctx, 413, "413 Payload Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }

            if (content_length > 0 && content_length > body_in_buffer) {
                int total_needed = header_size + content_length;
                char *big_buf = realloc(raw_request, total_needed + 1);
                if (!big_buf) {
                    LOG_ERROR("realloc failed for body (%d bytes)", total_needed);
                    send_error_response(ctx, 500, "500 Internal Server Error", EPOCH_PRESTANDARD);
                    goto cleanup;
                }
                raw_request = big_buf;

                int remaining = content_length - body_in_buffer;
                int offset = bytes_read;
                while (remaining > 0) {
                    int ret = read_func(ctx, raw_request + offset, remaining);
                    if (ret <= 0) {
                        LOG_ERROR("Connection closed while reading body (%d remaining)", remaining);
                        goto cleanup;
                    }
                    offset += ret;
                    remaining -= ret;
                }
                bytes_read = offset;
                raw_request[bytes_read] = '\0';
            }
        }
    }

    // --- Parse request line + headers ---
    if (parse_http_request(raw_request, bytes_read, &req) != 0) {
        LOG_WARN("Malformed HTTP request");
        send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
        goto cleanup;
    }

    // A real HTTP/0.9 simple request (req.protocol left "" by
    // parse_http_request - see http_request_parser.c) legitimately has no
    // "HTTP/..." token at all; every other request must have one.
    if (req.protocol[0] != '\0' && strncmp(req.protocol, "HTTP/", 5) != 0) {
        send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
        goto cleanup;
    }

    LOG_INFO("%s %s %s", req.method, req.url, req.protocol);

    // A genuine HTTP/0.9 client (WorldWideWeb, the early Line Mode Browser,
    // ViolaWWW, Erwise): no version and, per its spec, no headers - so no
    // User-Agent either. It expects the document alone, no status line or
    // headers, which it would show as the start of the page. Static files
    // turn this back off below (Cello). See is_http09_request().
    ((connection_ctx_t *)ctx)->simple_response = is_http09_request(&req);

    // --- Determine real client IP ---
    // A PROXY protocol header (read in connection_thread.c) wins; otherwise
    // honor X-Real-IP / X-Forwarded-For only from trusted reverse proxies.
    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    if (((connection_ctx_t *)ctx)->proxy_client_ip[0]) {
        snprintf(client_ip, sizeof(client_ip), "%s", ((connection_ctx_t *)ctx)->proxy_client_ip);
    } else {
        connection_ctx_t *conn = (connection_ctx_t *)ctx;
        struct sockaddr_storage peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        char peer_ip[INET6_ADDRSTRLEN] = "unknown";

        if (getpeername(conn->client_socket, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
            if (peer_addr.ss_family == AF_INET) {
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)&peer_addr)->sin_addr,
                          peer_ip, sizeof(peer_ip));
            } else if (peer_addr.ss_family == AF_INET6) {
                inet_ntop(AF_INET6,
                          &((struct sockaddr_in6 *)&peer_addr)->sin6_addr,
                          peer_ip, sizeof(peer_ip));
            }
        }

        if (is_trusted_proxy(peer_ip)) {
            const char *real_ip  = get_header_value(&req, "X-Real-IP");
            const char *fwd      = get_header_value(&req, "X-Forwarded-For");
            if (real_ip && real_ip[0]) {
                snprintf(client_ip, sizeof(client_ip), "%s", real_ip);
            } else if (fwd && fwd[0]) {
                snprintf(client_ip, sizeof(client_ip), "%s", fwd);
                char *comma = strchr(client_ip, ',');
                if (comma) *comma = '\0';
                // trim leading space
                char *p = client_ip;
                while (*p == ' ') p++;
                if (p != client_ip) memmove(client_ip, p, strlen(p) + 1);
            } else {
                snprintf(client_ip, sizeof(client_ip), "%s", peer_ip);
            }
        } else {
            snprintf(client_ip, sizeof(client_ip), "%s", peer_ip);
        }
    }

    {
        const char *ua = get_header_value(&req, "User-Agent");
        LOG_DEBUG("Client IP: %s | UA: %s", client_ip, ua ? ua : "(none)");
    }

    // --- Route ---
    {
        char route[2048];
        QueryParam params[MAX_PARAMS];
        int param_count = 0;
        char url_copy[2048];
        strncpy(url_copy, req.url, sizeof(url_copy) - 1);
        url_copy[sizeof(url_copy) - 1] = '\0';
        url_parse(url_copy, route, sizeof(route), params, &param_count);

        char decoded_url[2048];
        url_decode(decoded_url, route);

        // Resolve the request's language once, before anything renders: the
        // menu reads it too, several layers down (see utils/request_lang.h).
        request_lang_set(get_header_value(&req, "Cookie"),
                          get_query_param(params, param_count, "lang"));
        request_path_set(decoded_url);
        request_user_set(get_header_value(&req, "Cookie"));
        request_theme_set(get_header_value(&req, "Cookie"),
                           get_query_param(params, param_count, "theme"));
        request_site_set(get_header_value(&req, "Host"));
        // Independent of resolve_epoch() below: that resolves which
        // templates render (honoring force_epoch/preview_epoch overrides),
        // this resolves what the *real* visiting browser can decode, off
        // its actual User-Agent regardless of any override - see
        // utils/request_charset.h.
        request_charset_set(get_header_value(&req, "User-Agent"), is_http09_request(&req));
        request_code_lines_set(get_query_param(params, param_count, "code_lines"));
        request_wml_set(req.url, get_query_param(params, param_count, "wml_pages"));
        char content_lang[16];
        strncpy(content_lang, request_lang(), sizeof(content_lang) - 1);
        content_lang[sizeof(content_lang) - 1] = '\0';

        // Record the visit for /dashboard/analytics (no-op for bots, static
        // assets and /dashboard itself - see analytics_track_visit()'s own
        // filtering). resolve_epoch() is cheap and pure, so calling it again
        // here for tracking is fine even though every route handler below
        // calls it a second time for its own use.
        analytics_track_visit(req.method, decoded_url, get_header_value(&req, "User-Agent"),
                               client_ip, resolve_epoch(&req));

        char id[32];
        char block_id[32];
        char theme_epoch_str[8];
        ThemeCssSheet css_sheet;

        if (strncmp(decoded_url, EXTENSION_ROUTE_PREFIX, strlen(EXTENSION_ROUTE_PREFIX)) == 0 &&
            (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0 ||
             strcmp(req.method, "POST") == 0)) {
            serve_extension(ctx, &req, decoded_url);

        } else if (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0) {
            if (strcmp(decoded_url, "/") == 0 &&
                site_page_applies(get_query_param(params, param_count, "site"))) {
                // No Host, so no site named yet: the front sent this here
                // only because this is its default site (see
                // modules/site_page/site_page.h). Before the
                // under-construction check - this page is about choosing a
                // site, not this site's content.
                int epoch = resolve_epoch(&req);
                char *content = site_page(epoch);
                char *body = content ? buildPageWebSite(epoch, "Sites", content) : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (is_site_content_route(decoded_url) && !request_user_is_admin() &&
                serve_under_construction(ctx, &req, content_lang)) {
                // Sent: the under-construction page stands in for the site.

            } else if (strcmp(decoded_url, "/") == 0) {
                int epoch = resolve_epoch(&req);

                char *body = buildHomeWebSite(epoch, content_lang);
                if (!body) LOG_ERROR("buildHomeWebSite failed for epoch %d", epoch);

                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/login") == 0) {
                int epoch = resolve_epoch(&req);

                char user_id[USER_ID_HEX_BUF_SIZE];
                const char *cookie = get_header_value(&req, "Cookie");

                if (mongodb_manager_is_ready() && validate_session_cookie(cookie, user_id) == 1) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = dashboard_allowed(&req, epoch) ? login(epoch, NULL)
                                                                    : login_unavailable(epoch);
                    char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strcmp(decoded_url, "/dashboard") == 0) {
                int epoch = resolve_epoch(&req);

                if (!mongodb_manager_is_ready()) {
                    send_error_response(ctx, 503, "503 Service Unavailable", epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    const char *cookie = get_header_value(&req, "Cookie");

                    int signed_in = validate_session_cookie(cookie, user_id) == 1;
                    // A session whose user is gone, or a commenter's, is no
                    // sign-in at all (see role_may_open()).
                    char role[USER_ROLE_BUF_SIZE];
                    if (signed_in && (cms_get_user_role(user_id, role, sizeof(role)) != 0 ||
                                      strcmp(role, "commenter") == 0))
                        signed_in = 0;
                    if (signed_in && maintenance_locks_out(user_id)) {
                        send_maintenance_lockout(ctx, &req, epoch);
                    } else if (signed_in) {

                        char *content  = epoch == EPOCH_MODERN && !dashboard_allowed(&req, epoch)
                                             ? login_unavailable(epoch)
                                             : dashboard(epoch, content_lang, user_id, role);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *response = build_redirect_response("/login", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/entries") == 0) {
                // "View all" from the dashboard home, which itself lists only
                // the entries pending publication.
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        char *content  = dashboard_entries(epoch, content_lang, user_id, role);
                        char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Entries", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = categories_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = categories_admin_form(epoch, "", langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        if (cms_get_category_name_values(id, langs, lang_count, values) == 0) {
                            char *content  = categories_admin_form(epoch, id, langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/languages") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = languages_admin(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = menu_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = menu_admin_form(epoch, "", "", 0, false, langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        char link[256];
                        int order;
                        bool enabled;
                        if (cms_get_menu_item_values(id, langs, lang_count, values,
                                                      link, sizeof(link), &order, &enabled) == 0) {
                            char *content  = menu_admin_form(epoch, id, link, order, enabled,
                                                              langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        // ?error=<code> after a failed favicon upload.
                        static const struct { const char *code, *message; } ERRORS[] = {
                            { "favicon-type",  "The favicon must be an .ico file." },
                            { "favicon-16",    "The epoch 2 favicon must contain a 16x16 image." },
                            { "favicon-size",  "The favicon must be at most 256 KiB." },
                            { "favicon-write", "Could not save the favicon." },
                        };
                        const char *error_code = get_query_param(params, param_count, "error");
                        const char *error_message = NULL;
                        for (size_t i = 0; error_code && i < sizeof(ERRORS) / sizeof(ERRORS[0]); i++)
                            if (strcmp(error_code, ERRORS[i].code) == 0) error_message = ERRORS[i].message;

                        // ?mail=<code> after "Send a test email".
                        static const struct { const char *code, *message; } MAIL_NOTICES[] = {
                            { "queued",   "Test email queued to your account's address - it should arrive within a minute. If it doesn't, the server log says why." },
                            { "no-email", "Your account has no valid email address to send the test to." },
                            { "failed",   "The test email could not be queued (mail not set, or a database error)." },
                        };
                        const char *mail_code = get_query_param(params, param_count, "mail");
                        const char *mail_notice = NULL;
                        for (size_t i = 0; mail_code && i < sizeof(MAIL_NOTICES) / sizeof(MAIL_NOTICES[0]); i++)
                            if (strcmp(mail_code, MAIL_NOTICES[i].code) == 0) mail_notice = MAIL_NOTICES[i].message;

                        char *name = cms_get_site_name();
                        send_settings_general_page(ctx, &req, epoch, content_lang, name, NULL,
                                                   error_message, mail_notice);
                        free(name);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/banner", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *values[EPOCH_COUNT];
                            cms_get_theme_banner_values(id, values);
                            char *content  = site_settings_banner_page(epoch, id, values);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                            for (int i = 0; i < EPOCH_COUNT; i++) free(values[i]);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/footer", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *values[EPOCH_COUNT];
                            cms_get_theme_footer_values(id, values);
                            char *content  = site_settings_footer_page(epoch, id, values);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                            for (int i = 0; i < EPOCH_COUNT; i++) free(values[i]);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/logo", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            CmsLogoConfig configs[EPOCH_COUNT];
                            for (int e = -1; e <= 3; e++)
                                cms_get_theme_logo_config(id, e, &configs[epoch_to_index(e)]);
                            char *content  = site_settings_logo_page(epoch, id, configs);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        }
                    }
                }

            } else if (match_theme_css_editor(decoded_url, "", id, sizeof(id), &css_sheet)) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *custom    = cms_get_theme_css_value(id, css_sheet);
                            char *original  = cms_get_theme_css_original(id, css_sheet);
                            char *content   = site_settings_css_page(epoch, id, css_sheet, custom, original);
                            free(custom);
                            free(original);
                            char *body      = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response  = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        }
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/preview") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = site_settings_preview_page(epoch);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/browsers") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = browser_detection_admin_page(
                            epoch, get_header_value(&req, "User-Agent"));
                        char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Browser detection", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/updates") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *error = get_query_param(params, param_count, "error");
                        char *content  = updates_admin_page(epoch, error);
                        char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Updates", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/updates/progress") == 0) {
                int epoch = resolve_epoch(&req);
                if (!dashboard_allowed(&req, epoch)) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *json = updates_admin_progress_json();
                        char *response = build_json_response(json ? json : "{\"state\":\"none\"}");
                        free(json);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/extensions") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        extensions_rescan();
                        const char *error = get_query_param(params, param_count, "error");
                        char *content  = extensions_admin_page(epoch, error);
                        char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Extensions", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/fonts") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *error = get_query_param(params, param_count, "error");
                        char *content  = fonts_admin_list(epoch, error);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/themes") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char **keys = NULL;
                        size_t key_count = 0;
                        list_theme_keys(&keys, &key_count);

                        char *active = cms_get_active_theme_key();

                        ThemeEntry *entries = calloc(key_count, sizeof(ThemeEntry));
                        for (size_t i = 0; entries && i < key_count; i++) {
                            strncpy(entries[i].key, keys[i], sizeof(entries[i].key) - 1);
                            entries[i].active = strcmp(keys[i], active) == 0;
                        }

                        char *content  = entries ? site_settings_themes_page(epoch, entries, key_count) : NULL;
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        free(entries);
                        free(active);
                        free_theme_keys(keys, key_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/customize", id, sizeof(id))) {
                // The theme customizer; the list above links here.
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            ThemeCustomizeData data = {0};
                            char *active = cms_get_active_theme_key();
                            snprintf(data.theme.key, sizeof(data.theme.key), "%s", id);
                            data.theme.active = strcmp(id, active) == 0;
                            free(active);
                            cms_get_theme_colors(id, &data.theme.colors);
                            cms_get_theme_banner_values(id, data.banner);
                            cms_get_theme_footer_values(id, data.footer);
                            for (int e = -1; e <= 3; e++) {
                                cms_get_theme_logo_config(id, e, &data.logo[epoch_to_index(e)]);
                                if (e == EPOCH_MIDDLE || e == EPOCH_MODERN) {
                                    data.home_blog_background[epoch_to_index(e)] =
                                        cms_get_theme_home_blog_background(id, e);
                                    data.home_blog_item_background[epoch_to_index(e)] =
                                        cms_get_theme_home_blog_item_background(id, e);
                                }
                            }
                            data.home_blog_item_color_shown =
                                cms_get_theme_home_blog_item_color_shown(id, EPOCH_MIDDLE);
                            data.css_custom   = cms_get_theme_css_value(id, THEME_CSS_PUBLIC);
                            data.css_original = cms_get_theme_css_original(id, THEME_CSS_PUBLIC);

                            char *content  = site_settings_customize_page(epoch, &data);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);

                            for (int i = 0; i < EPOCH_COUNT; i++) {
                                free(data.banner[i]);
                                free(data.footer[i]);
                                free(data.home_blog_background[i]);
                                free(data.home_blog_item_background[i]);
                            }
                            free(data.css_custom);
                            free(data.css_original);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "", id, sizeof(id))) {
                // The old one-theme page: everything it held now lives in
                // the customizer.
                int epoch = resolve_epoch(&req);
                char target[128];
                snprintf(target, sizeof(target), "/dashboard/settings/themes/%s/customize",
                         theme_key_is_valid(id) ? id : "");
                char *response = build_redirect_response(theme_key_is_valid(id) ? target : "/dashboard/settings/themes",
                                                          "", epoch);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/dashboard/api/theme-assets/list") == 0) {
                int epoch = resolve_epoch(&req);
                if (!dashboard_allowed(&req, epoch)) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *theme_key = get_query_param(params, param_count, "theme");
                        const char *component = get_query_param(params, param_count, "component");
                        const char *epoch_str = get_query_param(params, param_count, "epoch");
                        char *json = theme_assets_list_json(theme_key, component, epoch_str);
                        char *response = build_json_response(json ? json : "{\"files\":[]}");
                        free(json);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *list = get_query_param(params, param_count, "list");
                        char *content  = users_admin_list(epoch, NULL, list && strcmp(list, "commenters") == 0);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/analytics") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *period = get_query_param(params, param_count, "period");
                        const char *date   = get_query_param(params, param_count, "date");
                        const char *from   = get_query_param(params, param_count, "from");
                        const char *to     = get_query_param(params, param_count, "to");
                        int year  = atoi(get_query_param(params, param_count, "year"));
                        int month = atoi(get_query_param(params, param_count, "month"));
                        int week  = atoi(get_query_param(params, param_count, "week"));

                        char *content  = analytics_view(epoch, period, year, month, week, date, from, to);
                        char *body     = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = users_admin_form(epoch, "", "", "", "", "author", NULL);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char email[256];
                        char role[USER_ROLE_BUF_SIZE];
                        if (cms_get_user_values(id, email, sizeof(email), role, sizeof(role)) == 0) {
                            char *name = cms_get_user_name_by_id(id);
                            char username[USERNAME_BUF_SIZE];
                            cms_get_username_by_id(id, username, sizeof(username));
                            char *content  = users_admin_form(epoch, id, name ? name : "", username,
                                                              email, role, NULL);
                            free(name);
                            char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/entries", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        CmsEntryEdit entry;
                        if (cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                            if (!can_edit_entry(role, user_id, &entry)) {
                                char *response = build_redirect_response("/dashboard", "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                CmsCategoryItem *categories = NULL;
                                size_t category_count = 0;
                                cms_get_categories(content_lang, &categories, &category_count);

                                char *content  = entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count);
                                char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                                free(body);
                                send_or_error(ctx, response, req.method, epoch);

                                cms_categories_free(categories, category_count);
                            }
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_entry_edit_free(&entry, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/media") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(media_scope(role, user_id), &dirs, &dir_count);
                        media_label_owners(dirs, dir_count, role, user_id);

                        char *content  = media_admin_page(epoch, dirs, dir_count, NULL, 0);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Media", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/contents") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        const char *dir_id = get_query_param(params, param_count, "directory");
                        const char *start_str = get_query_param(params, param_count, "start");
                        const char *end_str = get_query_param(params, param_count, "end");
                        int skip = atoi(start_str);
                        int limit = atoi(end_str) - skip;
                        if (limit <= 0) limit = MEDIA_PAGE_SIZE;

                        CmsMediaItem *items = NULL;
                        size_t item_count = 0;
                        cms_get_media_items(dir_id[0] ? dir_id : NULL, media_scope(role, user_id), skip, limit,
                                            &items, &item_count);

                        char *html = media_admin_render_items(items, item_count, epoch);
                        char *response = build_json_response(html ? html : "");
                        free(html);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);

                        cms_media_items_free(items, item_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/directory/item") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        const char *dir_id = get_query_param(params, param_count, "id");
                        const char *scope = media_scope(role, user_id);
                        CmsMediaDirectory dir;
                        if (!dir_id || !cms_get_media_directory_by_id(dir_id, &dir) ||
                            (scope && strcmp(dir.author_id, scope) != 0)) {
                            send_simple(ctx, "404 Not Found", "Directory not found");
                        } else {
                            char username[64] = {0};
                            cms_get_username_by_id(dir.author_id, username, sizeof(username));
                            snprintf(dir.author_name, sizeof(dir.author_name), "%s", username);
                            media_label_owners(&dir, 1, role, user_id);

                            char *html = media_admin_render_directory_item(&dir, epoch);
                            char *response = build_json_response(html ? html : "");
                            free(html);
                            connection_write(ctx, response, strlen(response));
                            free(response);
                            connection_close(ctx);
                        }
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/modal") == 0) {
                int epoch = resolve_epoch(&req);

                if (!dashboard_allowed(&req, epoch)) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        const char *scope = media_scope(role, user_id);
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(scope, &dirs, &dir_count);
                        media_label_owners(dirs, dir_count, role, user_id);

                        // Opens on the latest uploads across every directory
                        // (newest first, one page): with no directory selected,
                        // the picker's infinite scroll keeps paging that same
                        // all-directories list, and clicking a directory narrows it.
                        CmsMediaItem *items = NULL;
                        size_t item_count = 0;
                        cms_get_media_items(NULL, scope, 0, MEDIA_PAGE_SIZE, &items, &item_count);

                        // ?browse=1: the theme customizer's "View images" -
                        // nothing to pick for, so no "Use selected".
                        const char *browse = get_query_param(params, param_count, "browse");
                        char *html = media_admin_modal(epoch, dirs, dir_count, items, item_count,
                                                       !(browse && browse[0] == '1'));
                        if (html) {
                            char header_buf[128];
                            snprintf(header_buf, sizeof(header_buf),
                                     "Content-Type: text/html; charset=UTF-8\r\n"
                                     "Content-Length: %zu\r\n", strlen(html));
                            char response_buf[256];
                            snprintf(response_buf, sizeof(response_buf),
                                     "HTTP/1.1 200 OK\r\n%s\r\n", header_buf);
                            connection_write(ctx, response_buf, strlen(response_buf));
                            connection_write(ctx, html, strlen(html));
                            free(html);
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "Error");
                        }
                        connection_close(ctx);

                        cms_media_items_free(items, item_count);
                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/logout") == 0) {
                // POST only (see the POST "/logout" handler below): a GET a
                // third-party page could trigger with an <img> must not sign
                // the visitor out.
                send_error_response(ctx, 405, "405 Method Not Allowed", resolve_epoch(&req));

            } else if (strcmp(decoded_url, "/language/set") == 0) {
                // Stores the choice and bounces back. A plain GET on purpose:
                // it has to work from a nav-bar link on a browser with no
                // JavaScript and no form support to speak of.
                int epoch = resolve_epoch(&req);

                // url_parse() leaves query values percent-encoded, so "%2Fblog"
                // has to be decoded before it can be recognised as a path.
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));

                char safe_return[512];
                language_sanitize_return(return_raw, safe_return, sizeof(safe_return));

                char code[16];
                char extra[192] = "";
                if (request_lang_validate(get_query_param(params, param_count, "code"),
                                           code, sizeof(code))) {
                    // One year: the choice should outlive the browsing session.
                    snprintf(extra, sizeof(extra),
                             "Set-Cookie: lang=%s; Path=/; Max-Age=31536000; SameSite=Lax\r\n",
                             code);
                }

                char *abs_return = absolute_location(ctx, &req, safe_return);
                char *response = build_redirect_response(abs_return ? abs_return : safe_return,
                                                          extra, epoch);
                free(abs_return);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/theme/set") == 0) {
                // Mirrors /language/set exactly, including the plain-GET
                // rationale: a nav-bar link with no JS/form needed. Epoch 3
                // only offers this from the navbar (see menu.c's
                // theme_selector()), but the route itself doesn't check
                // epoch - a hand-typed link works from any browser.
                int epoch = resolve_epoch(&req);

                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));

                char safe_return[512];
                language_sanitize_return(return_raw, safe_return, sizeof(safe_return));

                char extra[384] = "";
                const char *key = get_query_param(params, param_count, "key");
                if (theme_key_is_valid(key)) {
                    snprintf(extra, sizeof(extra),
                             "Set-Cookie: theme=%s; Path=/; Max-Age=31536000; SameSite=Lax\r\n",
                             key);
                }

                char *abs_return = absolute_location(ctx, &req, safe_return);
                char *response = build_redirect_response(abs_return ? abs_return : safe_return,
                                                          extra, epoch);
                free(abs_return);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/dashboard/comments") == 0) {
                int epoch = resolve_epoch(&req);
                if (!dashboard_allowed(&req, epoch)) {
                    send_or_error(ctx, build_redirect_response("/dashboard", "", epoch), req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        // An Autor moderates comments on their own blog entries only; a
                        // Moderador every comment, but deletes and blocks nothing.
                        const char *status = get_query_param(params, param_count, "status");
                        if (!status || (strcmp(status, "approved") != 0 && strcmp(status, "rejected") != 0 &&
                                        strcmp(status, "unverified") != 0))
                            status = "pending";
                        static const struct { const char *code, *message; } DONE[] = {
                            { "approved", "Comment approved: it is public now." },
                            { "rejected", "Comment rejected: it is not shown." },
                            { "deleted",  "Comment deleted." },
                            { "blocked",  "Email blocked: its future comments are refused." },
                            { "unblocked", "Email unblocked." },
                            { "resent",   "A new confirmation email is on its way (the old link no longer works); the comment has 24 more hours." },
                            { "replied",  "Reply saved: it shows under the comment." },
                            { "replied-approved", "Reply saved and comment approved: both are public now." },
                            { "reply-deleted", "Reply deleted." },
                            { "error",    "That didn't work - the comment may be gone, or not on your entries." },
                        };
                        const char *done = get_query_param(params, param_count, "done");
                        const char *notice = NULL;
                        for (size_t i = 0; done && i < sizeof(DONE) / sizeof(DONE[0]); i++)
                            if (strcmp(done, DONE[i].code) == 0) notice = DONE[i].message;
                        int moderator = strcmp(role, "moderator") == 0;
                        char *content = comments_admin_page(epoch, status,
                                                            role_sees_everything(role) || moderator ? NULL : user_id,
                                                            !moderator, role_sees_everything(role),
                                                            role_may_reply(role), content_lang, notice);
                        char *body = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Comments", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/comments", "/reply", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (!dashboard_allowed(&req, epoch)) {
                    send_or_error(ctx, build_redirect_response("/dashboard", "", epoch), req.method, epoch);
                } else if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    const char *back = comments_tab(get_query_param(params, param_count, "back"));
                    const char *owner = strcmp(role, "admin") == 0 ? NULL : user_id;
                    char *content = role_may_reply(role) && cms_comments_can_moderate(id, owner)
                        ? comments_admin_reply_page(epoch, id, content_lang, back, NULL, NULL) : NULL;
                    if (!content) {
                        char location[128];
                        snprintf(location, sizeof(location), "/dashboard/comments?status=%s&done=error", back);
                        send_or_error(ctx, build_redirect_response(location, "", epoch), req.method, epoch);
                    } else {
                        char *body = buildPageWebSite(epoch, "{{SITE_NAME}} - Reply to a comment", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/comments/captcha") == 0) {
                // The comment form's challenge image for epochs 1-2, which
                // can't show the inline one: a GIF drawn again from the
                // token (?t=) - a black-and-white WBMP for WAP phones with
                // ?format=wbmp - only while it is the site's own and
                // unexpired (captcha.h). Nothing is stored or written.
                const char *format = get_query_param(params, param_count, "format");
                int wbmp = format && strcmp(format, "wbmp") == 0;
                unsigned char key[SITE_SECRET_BYTES];
                size_t gif_len = 0;
                const char *token = get_query_param(params, param_count, "t");
                unsigned char *gif = cms_get_site_secret(key) != 0 ? NULL
                    : wbmp ? captcha_token_wbmp(key, token, time(NULL), &gif_len)
                           : captcha_token_gif(key, token, time(NULL), &gif_len);
                sodium_memzero(key, sizeof(key));
                if (!gif) {
                    send_error_response(ctx, 404, "404 Not Found", resolve_epoch(&req));
                } else {
                    char header[256];
                    snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-store\r\n"
                             "X-Content-Type-Options: nosniff\r\n"
                             "Connection: close\r\n"
                             "\r\n", wbmp ? "image/vnd.wap.wbmp" : "image/gif", gif_len);
                    connection_write(ctx, header, strlen(header));
                    if (strcmp(req.method, "HEAD") != 0) connection_write(ctx, (const char *)gif, gif_len);
                    free(gif);
                }

            } else if (strcmp(decoded_url, "/comments/verify") == 0) {
                // The link in a comment's confirmation email: a page with a
                // button - the GET itself confirms nothing (comments.h).
                int epoch = resolve_epoch(&req);
                char *content = comments_verify_page(epoch, get_query_param(params, param_count, "token"));
                char *body = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Confirm your comment", content) : NULL;
                char *response = body ? build_epoch_response(body, "Cache-Control: no-store\r\n", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/search") == 0) {
                // Every epoch: the form, and the published blog entries and
                // pages matching ?q= (see search_page.h).
                int epoch = resolve_epoch(&req);
                const char *raw_q = get_query_param(params, param_count, "q");
                char query[512] = "";
                if (raw_q && strlen(raw_q) < sizeof(query)) url_decode(query, raw_q);
                char *content = search_page(epoch, query);
                char *body    = content ? buildPageWebSiteAtUrl(epoch, "{{SITE_NAME}} - Search", content, "/search") : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/language") == 0) {
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char *content = language_page(epoch, return_raw);
                char *body    = content ? buildPageWebSite(epoch, "Language", content) : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/menu") == 0) {
                // The full site menu, for WML's compact "[Menu]" link (see
                // menu.c): the page layout's own menu renders in full on
                // this path, and the content is just a way back to where
                // the reader came from. Epochs without a menu-page template
                // simply don't have this page.
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char safe_return[512];
                language_sanitize_return(return_raw, safe_return, sizeof(safe_return));

                char *tpl_path = generate_url_theme("menu/menu-page_epoch%d.html", epoch);
                char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
                free(tpl_path);
                if (!tpl) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    char *content = render_template(tpl, safe_return);
                    free(tpl);
                    char *body = content ? buildPageWebSite(epoch, "Menu", content) : NULL;
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strcmp(decoded_url, "/theme") == 0) {
                // Mirrors /language exactly - the epoch 1/2 nav bar link's
                // full-page fallback (menu.c's theme_selector()), listing
                // every theme (theme_page.c).
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char *content = theme_page(epoch, return_raw);
                char *body    = content ? buildPageWebSite(epoch, "Theme", content) : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/page/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                serve_cms_entry(ctx, decoded_url + 6, "page", content_lang, req.method, epoch, NULL,
                                viewer_can_preview_drafts(&req), NULL);

            } else if (strncmp(decoded_url, "/qr/", 4) == 0 && decoded_url[4] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *code = decoded_url + 4;
                char *target = mongodb_manager_is_ready() ? short_link_resolve(code) : NULL;
                if (target) {
                    char *response = build_redirect_response(target, "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                    free(target);
                } else {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                }

            } else if (strncmp(decoded_url, "/youtube-qr/", 12) == 0 && decoded_url[12] != '\0') {
                // Always epoch 0's own page, regardless of the visitor's
                // detected epoch: this route exists solely so an inline QR
                // this wide doesn't get split across a paginated text
                // browser's screen boundary (see render_youtube_embed()'s
                // own comment) - there is only ever one variant of it.
                const char *video_id = decoded_url + 12;

                char back_raw[512];
                url_decode(back_raw, get_query_param(params, param_count, "back"));
                char safe_back[512];
                language_sanitize_return(back_raw, safe_back, sizeof(safe_back));

                char back_label[560];
                qr_back_label(safe_back, content_lang, back_label, sizeof(back_label));

                char short_url[128];
                youtube_short_url(video_id, short_url, sizeof(short_url));

                char *qr_text = request_needs_legacy_charset()
                    ? generate_qr_asciiblock_text(short_url)
                    : generate_qr_halfblock_text(short_url);

                if (!qr_text) {
                    send_error_response(ctx, 404, "404 Not Found", EPOCH_PRESTANDARD);
                } else {
                    char *page_tpl_path = generate_url_theme(
                        "elements/youtube-embed/youtube-qr-page_epoch%d.html", EPOCH_PRESTANDARD);
                    char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                    free(page_tpl_path);

                    char *page_html = page_tpl
                        ? render_template(page_tpl, qr_text, short_url, safe_back, back_label)
                        : NULL;
                    free(page_tpl);
                    free(qr_text);

                    if (page_html) {
                        char *response = build_epoch_response(page_html, "", EPOCH_PRESTANDARD);
                        send_or_error(ctx, response, req.method, EPOCH_PRESTANDARD);
                        free(page_html);
                    } else {
                        send_error_response(ctx, 500, "500 Internal Server Error", EPOCH_PRESTANDARD);
                    }
                }

            } else if (strncmp(decoded_url, "/image-qr/", 10) == 0 && decoded_url[10] != '\0' &&
                       strlen(decoded_url + 10) < 60) {
                // Same reasoning and structure as /youtube-qr/ just above:
                // always epoch 0's own page, so an inline QR never gets cut
                // across a paginated text browser's screen boundary. The
                // code names an existing short_links entry (see
                // render_image()/short_links.h) whose target is the image's
                // own full-size URL - the QR here encodes the short
                // "/qr/<code>" redirect to it, not the image path directly,
                // for the same QR-density reasons as the gallery/video pages.
                const char *code = decoded_url + 10;

                char back_raw[512];
                url_decode(back_raw, get_query_param(params, param_count, "back"));
                char safe_back[512];
                language_sanitize_return(back_raw, safe_back, sizeof(safe_back));

                char back_label[560];
                qr_back_label(safe_back, content_lang, back_label, sizeof(back_label));

                char short_path[64];
                snprintf(short_path, sizeof(short_path), "/qr/%.59s", code);
                char short_url[600];
                if (public_url[0]) {
                    snprintf(short_url, sizeof(short_url), "%s%s", public_url, short_path);
                } else {
                    char *abs = absolute_location(ctx, &req, short_path);
                    snprintf(short_url, sizeof(short_url), "%s", abs ? abs : short_path);
                    free(abs);
                }

                char *qr_text = request_needs_legacy_charset()
                    ? generate_qr_asciiblock_text(short_url)
                    : generate_qr_halfblock_text(short_url);

                if (!qr_text) {
                    send_error_response(ctx, 404, "404 Not Found", EPOCH_PRESTANDARD);
                } else {
                    char *page_tpl_path = generate_url_theme(
                        "elements/image/image-qr-page_epoch%d.html", EPOCH_PRESTANDARD);
                    char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                    free(page_tpl_path);

                    char *page_html = page_tpl
                        ? render_template(page_tpl, qr_text, short_url, safe_back, back_label)
                        : NULL;
                    free(page_tpl);
                    free(qr_text);

                    if (page_html) {
                        char *response = build_epoch_response(page_html, "", EPOCH_PRESTANDARD);
                        send_or_error(ctx, response, req.method, EPOCH_PRESTANDARD);
                        free(page_html);
                    } else {
                        send_error_response(ctx, 500, "500 Internal Server Error", EPOCH_PRESTANDARD);
                    }
                }

            } else if (strncmp(decoded_url, "/gallery/", 9) == 0 && decoded_url[9] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *gallery_id = decoded_url + 9;
                const char *img_str = get_query_param(params, param_count, "img");
                int img_index = img_str[0] ? atoi(img_str) : 0;

                if (!mongodb_manager_is_ready() || strlen(gallery_id) != 24) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    CmsMediaGallery gallery;
                    if (!cms_get_media_gallery(gallery_id, &gallery)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                    // The "< Back" link goes to the entry the gallery belongs
                    // to, named by its title, so a reader who followed a link
                    // into the gallery can find their way back to what they
                    // were reading - not just to wherever they happened to
                    // click in from. Falls back to the browser history when
                    // the entry cannot be resolved (e.g. a gallery left
                    // without a parent after the entry was deleted).
                    char back_url[280] = "";
                    char back_title[256] = "";
                    bool has_backlink = cms_get_entry_backlink(gallery.entry_id, content_lang,
                                                                back_url, sizeof(back_url),
                                                                back_title, sizeof(back_title));
                    const char *back_href = has_backlink ? back_url : "javascript:history.back()";
                    char back_label[300];
                    if (has_backlink && back_title[0])
                        snprintf(back_label, sizeof(back_label), "Back to %s", back_title);
                    else
                        snprintf(back_label, sizeof(back_label), "Back");

                    if (epoch >= 3) {
                        char *item_tpl_path = generate_url_theme("elements/gallery/gallery-page-item_epoch%d.html", epoch);
                        char *item_tpl = item_tpl_path ? read_file_to_string(item_tpl_path) : NULL;
                        free(item_tpl_path);

                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!item_tpl || !page_tpl) {
                            free(item_tpl); free(page_tpl);
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *items_html = strdup("");
                            for (size_t i = 0; items_html && i < gallery.url_count; i++) {
                                char *full  = image_url_variant(gallery.urls[i], "_full");
                                char *small = image_url_variant(gallery.urls[i], "_small");
                                char *item  = (full && small) ? render_template(item_tpl, full, small) : NULL;
                                if (item) items_html = str_append(items_html, item);
                                free(item); free(full); free(small);
                            }
                            free(item_tpl);

                            char *page_html = items_html ? render_template(page_tpl, back_href, back_label, items_html) : NULL;
                            free(items_html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    } else {
                        // Epochs -1/0/1/2: paginated gallery
                        int count = (int)gallery.url_count;
                        int cur = (img_index >= 0 && img_index < count) ? img_index : 0;
                        int prev_idx = (cur - 1 + count) % count;
                        int next_idx = (cur + 1) % count;

                        // The "< Back" link is fixed markup baked into the page
                        // shell itself (gallery-page_epoch<N>.html) - it never
                        // varies and takes no argument, so it does not need its
                        // own template. Epoch 3's shell already did this; the
                        // others used to render a separate gallery-page-back
                        // file and prepend it here, which duplicated the link
                        // on epoch 0, whose shell already carried its own copy.
                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!page_tpl) {
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *html = strdup("");

                            if (epoch <= 0) {
                                // Text-only browsers (epoch 0) and WAP phones
                                // (WML) can't show any of the actual photos -
                                // "[Image 1] [Image 2] ..." was markup with
                                // nothing behind it to view. A QR code that
                                // resolves to this same gallery gives the
                                // reader a real way to see it: scan it with
                                // the phone that's sitting right next to the
                                // old machine. Mirrors the previous site.
                                char gallery_path[300];
                                snprintf(gallery_path, sizeof(gallery_path), "/gallery/%s", gallery_id);

                                // A QR built straight from this page's own URL grows
                                // with it - the gallery path alone is already 33+
                                // characters before the domain, dense enough to make
                                // the Cello-rendered text QR (see qr_generator.h)
                                // overflow its own row width. Routing it through
                                // "/qr/<code>" instead keeps the QR (and the link
                                // printed under it, since a reader retyping it by
                                // hand benefits the same way) as short as the code
                                // itself, regardless of how long gallery_path is.
                                char *short_code = mongodb_manager_is_ready()
                                    ? short_link_get_or_create(gallery_path) : NULL;

                                char gallery_url[600];
                                if (short_code) {
                                    char qr_path[64];
                                    snprintf(qr_path, sizeof(qr_path), "/qr/%s", short_code);
                                    if (public_url[0]) {
                                        snprintf(gallery_url, sizeof(gallery_url), "%s%s", public_url, qr_path);
                                    } else {
                                        char *abs = absolute_location(ctx, &req, qr_path);
                                        snprintf(gallery_url, sizeof(gallery_url), "%s", abs ? abs : qr_path);
                                        free(abs);
                                    }
                                    free(short_code);
                                } else if (public_url[0]) {
                                    snprintf(gallery_url, sizeof(gallery_url), "%s%s", public_url, gallery_path);
                                } else {
                                    char *abs = absolute_location(ctx, &req, gallery_path);
                                    snprintf(gallery_url, sizeof(gallery_url), "%s", abs ? abs : gallery_path);
                                    free(abs);
                                }

                                char *qr_tpl_path = generate_url_theme("elements/gallery/gallery-page-qr_epoch%d.html", epoch);
                                char *qr_tpl = qr_tpl_path ? read_file_to_string(qr_tpl_path) : NULL;
                                free(qr_tpl_path);

                                if (qr_tpl && epoch == EPOCH_WML) {
                                    char qr_dir[512];
                                    snprintf(qr_dir, sizeof(qr_dir), "%s/content/qr", root_directory);
                                    mkdir(qr_dir, 0755);

                                    char qr_fs_path[512];
                                    snprintf(qr_fs_path, sizeof(qr_fs_path), "%s/content/qr/gallery-%s.wbmp",
                                             root_directory, gallery_id);
                                    char qr_web_path[280];
                                    snprintf(qr_web_path, sizeof(qr_web_path), "/content/qr/gallery-%s.wbmp", gallery_id);

                                    if (generate_qr_wbmp(gallery_url, qr_fs_path) == 0) {
                                        char *line = render_template(qr_tpl, qr_web_path, gallery_url, gallery_url);
                                        if (line) html = str_append(html, line);
                                        free(line);
                                    }
                                } else if (qr_tpl) {
                                    // Cello, the one real exception classified
                                    // into epoch 0 (see detect_epoch.c/
                                    // request_charset.h), can't decode the
                                    // Unicode half-blocks any more than it
                                    // can decode any other multi-byte UTF-8 -
                                    // it gets the plain-ASCII equivalent.
                                    char *qr_text = request_needs_legacy_charset()
                                        ? generate_qr_asciiblock_text(gallery_url)
                                        : generate_qr_halfblock_text(gallery_url);
                                    if (qr_text) {
                                        char *line = render_template(qr_tpl, qr_text, gallery_url, gallery_url);
                                        if (line) html = str_append(html, line);
                                        free(line);
                                    }
                                    free(qr_text);
                                }
                                free(qr_tpl);
                            } else {
                                char *main_tpl_path  = generate_url_theme("elements/gallery/gallery-page-main_epoch%d.html", epoch);
                                char *main_tpl       = main_tpl_path ? read_file_to_string(main_tpl_path) : NULL;
                                // One wrapper, %s in the middle - not a start/end
                                // pair. A template is a hole to fill, not two
                                // halves of one to reassemble by hand.
                                char *strip_path     = generate_url_theme("elements/gallery/gallery-page-thumbstrip_epoch%d.html", epoch);
                                char *strip_tpl       = strip_path ? read_file_to_string(strip_path) : NULL;
                                char *thumb_tpl_path = generate_url_theme("elements/gallery/gallery-page-thumb_epoch%d.html", epoch);
                                char *thumb_tpl      = thumb_tpl_path ? read_file_to_string(thumb_tpl_path) : NULL;
                                free(main_tpl_path); free(strip_path); free(thumb_tpl_path);

                                if (main_tpl && strip_tpl && thumb_tpl) {
                                    // The main image is the point of this page.
                                    // Epoch 2 gets `_half` (capped at 1024px,
                                    // keeps the source format); epoch 1 predates
                                    // progressive JPEG entirely and can only
                                    // show GIF, so it gets `_medium` instead,
                                    // rewritten to .gif like every other epoch-1
                                    // image on the site.
                                    char *main_url = (epoch == EPOCH_EARLY)
                                        ? image_url_variant(gallery.urls[cur], "_medium")
                                        : image_url_variant(gallery.urls[cur], "_half");
                                    if (epoch == EPOCH_EARLY && main_url) {
                                        char *dot = strrchr(main_url, '.');
                                        if (dot) strcpy(dot, ".gif");
                                    }

                                    // The width goes in pixels, read from the
                                    // file. A percentage on <img> is HTML 4.0
                                    // and these browsers draw it zero-wide;
                                    // they also never reflow, so the image is
                                    // capped to the width of the screen they
                                    // ran on. Narrower images keep their own
                                    // size rather than being blown up.
                                    char *main_block = render_template(main_tpl,
                                        gallery_id, prev_idx, main_url ? main_url : "",
                                        gallery_id, next_idx);
                                    free(main_url);
                                    if (main_block) { html = str_append(html, main_block); free(main_block); }

                                    // No table, no fixed columns: each thumbnail
                                    // is just an inline <a><img>, so the browser
                                    // wraps as many as fit the screen it's
                                    // actually running on rather than however
                                    // many happened to fit whatever screen a
                                    // fixed column count was chosen for.
                                    char *thumbs_html = strdup("");
                                    for (int i = 0; i < count && thumbs_html; i++) {
                                        char *t = (epoch == 1)
                                            ? image_url_variant(gallery.urls[i], "_micro")
                                            : image_url_variant(gallery.urls[i], "_small");
                                        if (epoch == 1 && t) {
                                            char *dot = strrchr(t, '.'); if (dot) strcpy(dot, ".gif");
                                        }
                                        int border = (i == cur) ? 3 : 1;
                                        char *thumb = render_template(thumb_tpl, gallery_id, i, t ? t : "", border);
                                        free(t);

                                        if (thumb) { thumbs_html = str_append(thumbs_html, thumb); free(thumb); }
                                    }

                                    char *strip = thumbs_html ? render_template(strip_tpl, thumbs_html) : NULL;
                                    free(thumbs_html);
                                    if (strip) { html = str_append(html, strip); free(strip); }
                                }
                                free(main_tpl); free(strip_tpl); free(thumb_tpl);
                            }

                            char *page_html = html ? render_template(page_tpl, back_href, back_label, html) : NULL;
                            free(html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    }
                    } // close the back-link scope
                    cms_media_gallery_free(&gallery);
                }

            } else if (strcmp(decoded_url, "/blog") == 0) {
                int epoch = resolve_epoch(&req);

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);
                char *cat_menu = category_menu_render(cats, cat_count, NULL, epoch);
                cms_categories_free(cats, cat_count);

                char *content  = blog_list(epoch, content_lang);
                char *body     = buildBlogListWebSiteAtUrl(epoch, "{{SITE_NAME}} - Blog", content, "/blog", cat_menu);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/blog/category/", 15) == 0 && decoded_url[15] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *cat_slug = decoded_url + 15;

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);

                char *cat_id   = NULL;
                char *cat_name = NULL;
                for (size_t i = 0; i < cat_count; i++) {
                    char *slug = slugify(cats[i].name);
                    if (slug && strcmp(slug, cat_slug) == 0) {
                        cat_id   = strdup(cats[i].id);
                        cat_name = strdup(cats[i].name);
                    }
                    free(slug);
                    if (cat_id) break;
                }

                if (!cat_id) {
                    cms_categories_free(cats, cat_count);
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    char *cat_menu = category_menu_render(cats, cat_count, cat_slug, epoch);
                    cms_categories_free(cats, cat_count);

                    char *content = blog_list_category(epoch, content_lang, cat_id);
                    free(cat_id);

                    char *page_title = render_template("Blog - %s", cat_name ? cat_name : cat_slug);
                    free(cat_name);

                    char *body     = buildBlogListWebSiteAtUrl(epoch, page_title ? page_title : "Blog",
                                                               content, "/blog", cat_menu);
                    free(page_title);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strcmp(decoded_url, "/blog/categories") == 0) {
                // The full category menu, for WML's compact "[Categories]"
                // link (see category_menu.c), plus a way back. The category
                // being browsed (when coming from /blog/category/<slug>)
                // stays highlighted. Epochs without a category-menu-page
                // template simply don't have this page.
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char safe_return[512];
                language_sanitize_return(return_raw[0] ? return_raw : "/blog",
                                         safe_return, sizeof(safe_return));

                char *tpl_path = generate_url_theme("category-menu/category-menu-page_epoch%d.html", epoch);
                char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
                free(tpl_path);
                if (!tpl) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    char current[sizeof(safe_return)] = "";
                    if (strncmp(safe_return, "/blog/category/", 15) == 0) {
                        snprintf(current, sizeof(current), "%s", safe_return + 15);
                        current[strcspn(current, "?#&")] = '\0';
                    }
                    CmsCategoryItem *cats = NULL;
                    size_t cat_count = 0;
                    cms_get_categories(content_lang, &cats, &cat_count);
                    char *cat_menu = category_menu_render(cats, cat_count,
                                                          current[0] ? current : NULL, epoch);
                    cms_categories_free(cats, cat_count);

                    char *content = render_template(tpl, cat_menu ? cat_menu : "", safe_return);
                    free(cat_menu);
                    free(tpl);
                    char *body = content ? buildPageWebSite(epoch, "Categories", content) : NULL;
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strncmp(decoded_url, "/blog/", 6) == 0 && strlen(decoded_url) > 6 + 8 &&
                       strcmp(decoded_url + strlen(decoded_url) - 8, "/comment") == 0 &&
                       !memchr(decoded_url + 6, '/', strlen(decoded_url) - 6 - 8)) {
                // WAP's comment form, a page of its own (comments.h); other
                // epochs have it under the entry.
                int epoch = resolve_epoch(&req);
                char link[256];
                snprintf(link, sizeof(link), "%.*s", (int)(strlen(decoded_url) - 6 - 8), decoded_url + 6);
                if (epoch != EPOCH_WML) {
                    char back[300];
                    snprintf(back, sizeof(back), "/blog/%s#comments", link);
                    send_or_error(ctx, build_redirect_response(back, "", epoch), req.method, epoch);
                } else {
                    const char *step = get_query_param(params, param_count, "step");
                    const char *sent = get_query_param(params, param_count, "comment");
                    char *content = sent[0] ? comments_wap_sent_page(link, sent)
                                         : comments_wap_form_page(link, content_lang,
                                                                  strcmp(step, "2") == 0 ? 2 : 1, NULL);
                    if (!content) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char *body = buildPageWebSite(epoch, "{{SITE_NAME}} - Comment", content);
                        char *response = body ? build_epoch_response(body, "Cache-Control: no-store\r\n", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strncmp(decoded_url, "/blog/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                // ?comment=sent|pending after the comment form (comments.h).
                char *comments = comments_section(epoch, decoded_url + 6, content_lang, NULL,
                                                  get_query_param(params, param_count, "comment"));
                serve_cms_entry(ctx, decoded_url + 6, "blog", content_lang, req.method, epoch, NULL,
                                viewer_can_preview_drafts(&req), comments);

            } else if (match_theme_css_url(decoded_url, id, sizeof(id), &css_sheet)) {
                if (!theme_key_is_valid(id)) {
                    send_error_response(ctx, 404, "404 Not Found", resolve_epoch(&req));
                } else {
                    char *css = cms_get_theme_css(id, css_sheet);
                    char *response = build_css_response(css ? css : "", "200 OK");
                    free(css);
                    if (response) {
                        connection_write(ctx, response, strlen(response));
                        free(response);
                    }
                    connection_close(ctx);
                }

            } else {
                // Files keep their status line and headers even for a
                // version-less request: Cello 1.x fetches images that way
                // and needs Content-Type to show them (static_file_server.c).
                ((connection_ctx_t *)ctx)->simple_response = 0;
                const char *ims = get_header_value(&req, "If-Modified-Since");
                int status = serve_static_file(ctx, root_directory, decoded_url, ims);
                if (status != 0) {
                    const char *status_line = status == 404 ? "404 Not Found"
                                             : status == 403 ? "403 Forbidden"
                                             : "500 Internal Server Error";
                    send_error_response(ctx, status, status_line, resolve_epoch(&req));
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/login") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *content  = login_unavailable(epoch);
                char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (!mongodb_manager_is_ready()) {
                send_error_response(ctx, 503, "503 Service Unavailable", epoch);

            } else {
                char email[256];
                char password[256];
                parse_urlencoded_field(req.body, req.body_length, "user", email, sizeof(email));
                parse_urlencoded_field(req.body, req.body_length, "password", password, sizeof(password));

                char *user_id = auth_login_user(email, password);
                char *response = NULL;

                // Checked only once the password is right, so the message
                // tells nothing to someone who doesn't know it.
                if (user_id && maintenance_locks_out(user_id)) {
                    LOG_INFO("Sign-in refused (maintenance mode, not an administrator): %s", email);
                    free(user_id);
                    char *content  = login(epoch, MAINTENANCE_LOGIN_MESSAGE);
                    char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content);
                    response       = body ? build_epoch_response_status(body, "", epoch,
                                                                        "503 Service Unavailable") : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                } else if (user_id) {
                    char *token = generate_session_token();
                    if (token && create_session(user_id, token, session_ttl_seconds) == 0) {
                        char cookie_header[256];
                        build_session_cookie_header(token, session_ttl_seconds, cookie_header, sizeof(cookie_header));
                        response = build_redirect_response("/dashboard", cookie_header, epoch);
                    }
                    free(token);
                    free(user_id);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = login(epoch, "Invalid email or password.");
                    char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Login", content);
                    response       = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (match_id_route(decoded_url, "/dashboard/settings/extensions", "/enable", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/settings/extensions", "/disable", id, sizeof(id)))) {
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            if (require_admin_session(ctx, &req, epoch, user_id)) {
                int enable = strcmp(decoded_url + strlen(decoded_url) - strlen("/enable"), "/enable") == 0;
                int rc = extensions_set_enabled(id, enable);
                const char *target = rc == 0  ? "/dashboard/settings/extensions"
                                   : rc == -1 ? "/dashboard/settings/extensions?error=That+extension+can%27t+be+enabled+-+see+its+problem+below."
                                              : "/dashboard/settings/extensions?error=Could+not+save+the+change+-+database+error.";
                send_or_error(ctx, build_redirect_response(target, "", epoch), req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (match_id_route(decoded_url, "/dashboard/comments/commenter", "/block", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments/commenter", "/unblock", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments/commenter", "/block-delete", id, sizeof(id)))) {
            // Blocking an email is an Administrador's call.
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            if (require_admin_session(ctx, &req, epoch, user_id)) {
                const char *verb = strrchr(decoded_url, '/') + 1;
                int unblock = strcmp(verb, "unblock") == 0;
                int rc = cms_comments_block(id, !unblock, strcmp(verb, "block-delete") == 0);
                char back[16] = "pending";
                parse_urlencoded_field(req.body, req.body_length, "back", back, sizeof(back));
                if (strcmp(back, "approved") != 0 && strcmp(back, "rejected") != 0 &&
                    strcmp(back, "unverified") != 0) strcpy(back, "pending");
                char location[128];
                snprintf(location, sizeof(location), "/dashboard/comments?status=%s&done=%s", back,
                         rc != 0 ? "error" : unblock ? "unblocked" : "blocked");
                send_or_error(ctx, build_redirect_response(location, "", epoch), req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (match_id_route(decoded_url, "/dashboard/comments", "/reply", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments", "/reply/delete", id, sizeof(id)))) {
            // The one reply to a comment: an Administrador's on any, an Autor's on
            // their own entries'.
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            char role[USER_ROLE_BUF_SIZE];
            if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                char back_field[16] = "";
                parse_urlencoded_field(req.body, req.body_length, "back", back_field, sizeof(back_field));
                const char *back = comments_tab(back_field);
                const char *owner = strcmp(role, "admin") == 0 ? NULL : user_id;
                int may = role_may_reply(role) && cms_comments_can_moderate(id, owner);
                const char *done = "error";
                if (may && strcmp(strrchr(decoded_url, '/'), "/delete") == 0) {
                    if (cms_comments_delete_reply(id) == 0) done = "reply-deleted";
                } else if (may) {
                    // Room for more than a reply may hold, so a long one is refused, not cut.
                    size_t size = COMMENT_REPLY_MAX * 4 + 1;
                    char *text = calloc(1, size);
                    const char *error = NULL;
                    CommentReplyResult rc = COMMENT_REPLY_ERROR;
                    if (text) {
                        parse_urlencoded_field(req.body, req.body_length, "text", text, size);
                        char *base = public_url[0] ? strdup(public_url) : absolute_location(ctx, &req, "");
                        rc = comments_reply(id, user_id, text, size, content_lang, base ? base : "", &error);
                        free(base);
                    }
                    if (rc == COMMENT_REPLY_INVALID) {
                        // The page again, with what was typed and why it was refused.
                        char *content = comments_admin_reply_page(epoch, id, content_lang, back, text, error);
                        char *body = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Reply to a comment", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        free(text);
                        send_or_error(ctx, response, req.method, epoch);
                        done = NULL;
                    } else {
                        free(text);
                        if (rc == COMMENT_REPLY_SAVED) done = "replied";
                        else if (rc == COMMENT_REPLY_APPROVED) done = "replied-approved";
                    }
                }
                if (done) {
                    char location[128];
                    snprintf(location, sizeof(location), "/dashboard/comments?status=%s&done=%s", back, done);
                    send_or_error(ctx, build_redirect_response(location, "", epoch), req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (match_id_route(decoded_url, "/dashboard/comments", "/approve", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments", "/reject", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments", "/delete", id, sizeof(id)) ||
                    match_id_route(decoded_url, "/dashboard/comments", "/resend", id, sizeof(id)))) {
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            char role[USER_ROLE_BUF_SIZE];
            if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                const char *verb = strrchr(decoded_url, '/') + 1;
                int moderator = strcmp(role, "moderator") == 0;
                const char *owner = role_sees_everything(role) || moderator ? NULL : user_id;
                int rc = -1;
                if (cms_comments_can_moderate(id, owner)) {
                    if (strcmp(verb, "resend") == 0) {
                        char *base = public_url[0] ? strdup(public_url) : absolute_location(ctx, &req, "");
                        rc = comments_resend(id, content_lang, base ? base : "");
                        free(base);
                    } else if (strcmp(verb, "delete") == 0) rc = moderator ? -1 : cms_comments_delete(id);
                    else rc = cms_comments_set_status(id, strcmp(verb, "approve") == 0 ? "approved" : "rejected", user_id);
                }
                char back[16] = "pending";
                parse_urlencoded_field(req.body, req.body_length, "back", back, sizeof(back));
                if (strcmp(back, "approved") != 0 && strcmp(back, "rejected") != 0 &&
                    strcmp(back, "unverified") != 0) strcpy(back, "pending");
                char location[128];
                snprintf(location, sizeof(location), "/dashboard/comments?status=%s&done=%s", back,
                         rc != 0 ? "error" : strcmp(verb, "resend") == 0 ? "resent"
                         : strcmp(verb, "delete") == 0 ? "deleted"
                         : strcmp(verb, "approve") == 0 ? "approved" : "rejected");
                send_or_error(ctx, build_redirect_response(location, "", epoch), req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/comments/verify") == 0) {
            int epoch = resolve_epoch(&req);
            char token[80] = "";
            parse_urlencoded_field(req.body, req.body_length, "token", token, sizeof(token));
            char *set_cookie = NULL;
            char *base = public_url[0] ? strdup(public_url) : absolute_location(ctx, &req, "");
            char *content = comments_verify_post(epoch, token, content_lang, base ? base : "", &set_cookie);
            free(base);
            char *body = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Comment confirmed", content) : NULL;
            char headers[512];
            snprintf(headers, sizeof(headers), "Cache-Control: no-store\r\n%s", set_cookie ? set_cookie : "");
            char *response = body ? build_epoch_response(body, headers, epoch) : NULL;
            free(body);
            free(set_cookie);
            send_or_error(ctx, response, req.method, epoch);

        } else if (strcmp(req.method, "POST") == 0 && strncmp(decoded_url, "/blog/", 6) == 0 &&
                   strlen(decoded_url) > 6 + 9 &&
                   strcmp(decoded_url + strlen(decoded_url) - 9, "/comments") == 0) {
            // A visitor's comment on a blog entry (comments.h): back to the
            // entry with a notice, or the entry again with the form and why.
            int epoch = resolve_epoch(&req);
            char link[256];
            size_t link_len = strlen(decoded_url) - 6 - 9;
            if (link_len == 0 || link_len >= sizeof(link) || memchr(decoded_url + 6, '/', link_len)) {
                send_error_response(ctx, 404, "404 Not Found", epoch);
            } else {
                memcpy(link, decoded_url + 6, link_len);
                link[link_len] = '\0';
                char back[300];
                snprintf(back, sizeof(back), "/blog/%s", link);
                char *base = public_url[0] ? strdup(public_url) : absolute_location(ctx, &req, "");
                CommentForm form;
                CommentPostResult result = comments_post(link, content_lang, req.body, req.body_length,
                                                         get_header_value(&req, "Cookie"), client_ip,
                                                         base ? base : "", get_header_value(&req, "User-Agent"),
                                                         epoch, &form);
                free(base);
                if (result == COMMENT_POST_NOT_FOUND) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else if (result == COMMENT_POST_FORM && epoch == EPOCH_WML) {
                    // WAP's form is on pages of its own: why it was refused,
                    // with the way back to it (the phone kept what was typed).
                    char *content = comments_wap_form_page(link, content_lang, 2, form.error);
                    char *body = content ? buildPageWebSite(epoch, "{{SITE_NAME}} - Comment", content) : NULL;
                    char *response = body ? build_epoch_response(body, "Cache-Control: no-store\r\n", epoch) : NULL;
                    free(body);
                    if (response) send_or_error(ctx, response, req.method, epoch);
                    else send_error_response(ctx, 404, "404 Not Found", epoch);
                } else if (result == COMMENT_POST_FORM) {
                    char *comments = comments_section(epoch, link, content_lang, &form, NULL);
                    serve_cms_entry(ctx, link, "blog", content_lang, "GET", epoch, NULL, 0, comments);
                } else if (!get_header_value(&req, "Host") || !get_header_value(&req, "Host")[0]) {
                    // No Host (NCSA Mosaic 2, HTTP/1.0 browsers): a redirect
                    // can't name this server - Mosaic makes a bare path the
                    // hostless "http:///..." - so the entry is served right
                    // here, with the notice, as epoch 1's language and theme
                    // links avoid redirects too. A reload resends the form,
                    // which the spent challenge then refuses.
                    char *comments = comments_section(epoch, link, content_lang, NULL,
                                                      result == COMMENT_POST_PENDING ? "pending" : "sent");
                    serve_cms_entry(ctx, link, "blog", content_lang, "GET", epoch, NULL, 0, comments);
                } else {
                    // Epochs 0-2 carry the language (0-1) and theme (1-2)
                    // in the URL, not a cookie (build_epoch_response.c);
                    // and epoch 1 gets no #fragment, which an HTTP/1.0
                    // browser may send on as part of the query.
                    char keep[96] = "";
                    if (epoch == EPOCH_PRESTANDARD || epoch == EPOCH_WML)
                        snprintf(keep, sizeof(keep), "&lang=%s", request_lang());
                    else if (epoch == EPOCH_EARLY)
                        snprintf(keep, sizeof(keep), "&theme=%s&lang=%s", request_theme(), request_lang());
                    else if (epoch == EPOCH_MIDDLE)
                        snprintf(keep, sizeof(keep), "&theme=%s", request_theme());
                    char location[420];
                    // WAP: to a short page with the thanks - the entry's
                    // comments are many packets away from its top.
                    if (epoch == EPOCH_WML) strcat(back, "/comment");
                    snprintf(location, sizeof(location), "%s?comment=%s%s%s", back,
                             result == COMMENT_POST_PENDING ? "pending" : "sent", keep,
                             epoch == EPOCH_EARLY || epoch == EPOCH_WML ? "" : "#comments");
                    // Absolute: Mosaic turns a bare "/blog/..." into the
                    // hostless "http:///blog/..." (absolute_location()).
                    char *abs = absolute_location(ctx, &req, location);
                    send_or_error(ctx, build_redirect_response(abs ? abs : location, "", epoch), req.method, epoch);
                    free(abs);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/logout") == 0) {
            // Posted by the dashboard's "Log out" button (nav-admin/nav-author_epoch3.html),
            // whose csrf_token field csrf.js adds on submit. Not routed through
            // require_dashboard_session(): an expired session, or MongoDB
            // being down, must still clear the cookie - but whenever a session
            // cookie is present the CSRF token has to match it, so a
            // cross-site form can't sign the visitor out.
            int epoch = resolve_epoch(&req);

            char token[SESSION_TOKEN_BUF_SIZE];
            const char *cookie = get_header_value(&req, "Cookie");
            int has_token = extract_session_token(cookie, token, sizeof(token));

            char supplied[CSRF_TOKEN_BUF_SIZE + 1];
            if (has_token) request_csrf_token_value(&req, supplied, sizeof(supplied));

            if (has_token && !verify_csrf_token(cookie, supplied)) {
                LOG_WARN("CSRF check failed: %s %s", req.method, req.url);
                send_simple(ctx, "403 Forbidden", "Invalid or missing CSRF token. Reload the page and try again.");
            } else {
                if (has_token && mongodb_manager_is_ready()) destroy_session(token);

                char clear_cookie[128];
                build_session_clear_cookie_header(clear_cookie, sizeof(clear_cookie));

                char *response = build_redirect_response("/", clear_cookie, epoch);
                send_or_error(ctx, response, req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/categories/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_create_category(langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_update_category(id, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_category(id);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/menu/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_create_menu_item(link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_update_menu_item(id, link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_menu_item(id);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/api/block-preview") == 0) {
            // The entry editor's preview of an unselected block, for the
            // types whose public rendering is server-made (code-text's
            // highlighting, gallery's thumbnails, table's themed rows, the YouTube
            // iframe, image's size/float, paragraph's style): the same epoch 3 output a
            // reader gets, so the editor never shows a second, drifting copy.
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    size_t cap = req.body_length > 0 ? (size_t)req.body_length + 1 : 1;
                    char *text = malloc(cap);
                    char type[32];
                    char extra[256];
                    char *html = NULL;
                    if (text) {
                        parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type));
                        parse_urlencoded_field(req.body, req.body_length, "text", text, cap);
                        parse_urlencoded_field(req.body, req.body_length, "extra", extra, sizeof(extra));
                        if (strcmp(type, "code-text") == 0 || strcmp(type, "gallery") == 0 ||
                            strcmp(type, "table") == 0 || strcmp(type, "youtube-embed") == 0 ||
                            strcmp(type, "image") == 0 || strcmp(type, "paragraph") == 0)
                            html = entry_page_render_block(type, text, extra, EPOCH_MODERN);
                    }
                    free(text);

                    if (html) {
                        char head[256];
                        snprintf(head, sizeof(head),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html; charset=UTF-8\r\n"
                                 "Content-Length: %zu\r\n"
                                 "X-Content-Type-Options: nosniff\r\n"
                                 "Connection: close\r\n\r\n", strlen(html));
                        connection_write(ctx, head, strlen(head));
                        connection_write(ctx, html, strlen(html));
                        free(html);
                    } else {
                        send_simple(ctx, "500 Internal Server Error", "Error");
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/entries/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    const char *type = strcmp(role, "author") == 0 ? "blog" : "page";
                    char *new_id = cms_create_entry(user_id, type);
                    char *response;
                    if (new_id) {
                        char location[64];
                        snprintf(location, sizeof(location), "/dashboard/entries/%s/edit", new_id);
                        response = build_redirect_response(location, "", epoch);
                    } else {
                        response = build_redirect_response("/dashboard", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                    free(new_id);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/entries", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_entry(id);

                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/meta", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char link[256];
                        char type[16];
                        parse_urlencoded_field(req.body, req.body_length, "link", link, sizeof(link));
                        if (strcmp(role, "author") == 0) {
                            strcpy(type, "blog");
                        } else if (!parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type))) {
                            strcpy(type, "page");
                        }

                        char enabled_str[8];
                        bool enabled = parse_urlencoded_field(req.body, req.body_length, "enabled",
                                                               enabled_str, sizeof(enabled_str)) != 0;

                        char *category_ids[CATEGORY_LIST_LIMIT];
                        size_t category_count = parse_urlencoded_multi(req.body, req.body_length, "categories",
                                                                         category_ids, CATEGORY_LIST_LIMIT);

                        if (cms_update_entry_meta(id, link, type, enabled, category_ids, category_count) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < category_count; i++) free(category_ids[i]);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/header", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char image_url[1024];
                        char date[16];
                        char hide_author_str[8];
                        parse_urlencoded_field(req.body, req.body_length, "image_url", image_url, sizeof(image_url));
                        parse_urlencoded_field(req.body, req.body_length, "date", date, sizeof(date));
                        // Unchecked checkboxes are not submitted at all, so an
                        // absent field means "show the author".
                        parse_urlencoded_field(req.body, req.body_length, "hide_author",
                                               hide_author_str, sizeof(hide_author_str));
                        bool hide_author = (strcmp(hide_author_str, "1") == 0);
                        char closed_str[8];
                        parse_urlencoded_field(req.body, req.body_length, "comments_closed",
                                               closed_str, sizeof(closed_str));
                        bool comments_closed = (strcmp(closed_str, "1") == 0);

                        char **title_values   = calloc(lang_count, sizeof(char *));
                        char **summary_values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) {
                            char field[40], value[1024];

                            snprintf(field, sizeof(field), "title_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            title_values[i] = strdup(value);

                            snprintf(field, sizeof(field), "summary_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            summary_values[i] = strdup(value);
                        }

                        if (cms_update_entry_header(id, image_url, date, hide_author, comments_closed,
                                                     langs, lang_count,
                                                     title_values, summary_values) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < lang_count; i++) {
                            free(title_values[i]);
                            free(summary_values[i]);
                        }
                        free(title_values);
                        free(summary_values);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/content", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char count_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "content_count", count_str, sizeof(count_str));
                        int block_count = atoi(count_str);
                        if (block_count < 0) block_count = 0;
                        if (block_count > 200) block_count = 200;

                        CmsContentBlockEdit *blocks = calloc((size_t)block_count, sizeof(CmsContentBlockEdit));
                        for (int i = 0; i < block_count; i++) {
                            char field[48], value[8192];

                            snprintf(field, sizeof(field), "content_%d_id", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].id = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_type", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].type = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_order", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].order = atoi(value);

                            snprintf(field, sizeof(field), "content_%d_extra_data", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].extra_data = strdup(value);

                            blocks[i].text_values = calloc(lang_count, sizeof(char *));
                            for (size_t j = 0; j < lang_count; j++) {
                                snprintf(field, sizeof(field), "content_%d_text_%s", i, langs[j].code);
                                parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                                blocks[i].text_values[j] = strdup(value);
                            }
                        }

                        if (cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count) == 0) {
                            for (int gi = 0; gi < block_count; gi++) {
                                if (blocks[gi].type && strcmp(blocks[gi].type, "gallery") == 0 &&
                                    blocks[gi].text_values && blocks[gi].text_values[0] &&
                                    blocks[gi].text_values[0][0]) {
                                    char gal_id[25];
                                    const char *existing = blocks[gi].extra_data;
                                    if (cms_upsert_media_gallery(
                                            (existing && strlen(existing) == 24) ? existing : NULL,
                                            id, blocks[gi].text_values[0], gal_id) == 0) {
                                        free(blocks[gi].extra_data);
                                        blocks[gi].extra_data = strdup(gal_id);
                                        cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count);
                                    }
                                }
                            }
                            // Hand back the (possibly freshly minted) block ids
                            // so the editor's data-block-id attributes stay in
                            // sync with what is now stored.
                            size_t ids_size = 32 + (size_t)block_count * 28;
                            char *ids_json = malloc(ids_size);
                            size_t off = (size_t)snprintf(ids_json, ids_size, "{\"ok\":true,\"ids\":[");
                            for (int i = 0; i < block_count; i++)
                                off += (size_t)snprintf(ids_json + off, ids_size - off, "%s\"%s\"",
                                                        i ? "," : "",
                                                        blocks[i].id ? blocks[i].id : "");
                            snprintf(ids_json + off, ids_size - off, "]}");
                            response = build_json_response(ids_json);
                            free(ids_json);
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (int i = 0; i < block_count; i++) {
                            free(blocks[i].id);
                            free(blocks[i].type);
                            free(blocks[i].extra_data);
                            for (size_t j = 0; j < lang_count; j++) free(blocks[i].text_values[j]);
                            free(blocks[i].text_values);
                        }
                        free(blocks);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_block_delete_route(decoded_url, id, sizeof(id), block_id, sizeof(block_id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else if (cms_remove_entry_content_block(id, block_id) == 0) {
                        response = build_json_response("{\"ok\":true}");
                    } else {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"remove failed\"}", "400 Bad Request");
                    }
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/blocks", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response = NULL;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else {
                        char type[16];
                        char order_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type));
                        parse_urlencoded_field(req.body, req.body_length, "order", order_str, sizeof(order_str));
                        int order = atoi(order_str);

                        char new_block_id[32];
                        if (cms_add_entry_content_block(id, type, order, new_block_id, sizeof(new_block_id)) == 0) {
                            CmsContentBlockEdit block = {0};
                            block.id = new_block_id;
                            block.type = type;
                            block.order = order;
                            block.extra_data = (char *)"";
                            block.text_values = calloc(lang_count, sizeof(char *));
                            for (size_t i = 0; i < lang_count; i++) block.text_values[i] = (char *)"";

                            char *html = entry_editor_render_block(&block, langs, lang_count, epoch);
                            free(block.text_values);

                            char *html_json = html ? json_escape_alloc(html) : NULL;
                            free(html);

                            if (html_json) {
                                char *json_body = render_template(
                                    "{\"ok\":true,\"block_id\":\"%s\",\"html\":\"%s\"}", new_block_id, html_json);
                                response = json_body ? build_json_response(json_body) : NULL;
                                free(json_body);
                            }
                            free(html_json);
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"create failed\"}", "400 Bad Request");
                        }
                    }

                    if (!response)
                        response = build_json_response_status("{\"ok\":false,\"error\":\"internal error\"}", "500 Internal Server Error");
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (strcmp(decoded_url, "/dashboard/settings/favicon") == 0 ||
                    strcmp(decoded_url, "/dashboard/settings/favicon/delete") == 0)) {
            // The site favicons: multipart `epoch` (2/3) + `file` (.ico)
            // uploads one, written to a temporary name and renamed over the
            // old so a request never sees half a file; .../delete (field
            // `epoch`) removes it, back to the theme's own <link>.
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    int is_delete = strcmp(decoded_url, "/dashboard/settings/favicon/delete") == 0;
                    const char *error = NULL;
                    char path[64];

                    if (is_delete) {
                        char target[4] = "";
                        parse_urlencoded_field(req.body, req.body_length, "epoch", target, sizeof(target));
                        if (page_layout_favicon_path(atoi(target), path, sizeof(path))) unlink(path);
                    } else {
                        const char *ct = get_header_value(&req, "Content-Type");
                        MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);
                        const MultipartPart *epoch_part = mp ? multipart_find(mp, "epoch") : NULL;
                        const MultipartPart *file_part = mp ? multipart_find(mp, "file") : NULL;

                        int target = epoch_part && epoch_part->data_len == 1 ? epoch_part->data[0] - '0' : -9;
                        if (!file_part || !page_layout_favicon_path(target, path, sizeof(path))) {
                            error = "favicon-type";
                        } else {
                            error = favicon_ico_problem((const unsigned char *)file_part->data,
                                                        file_part->data_len, target);
                        }

                        if (!error) {
                            char tmp[80];
                            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
                            FILE *f = fopen(tmp, "wb");
                            int ok = f && fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                            if (f && fclose(f) != 0) ok = 0;
                            if (ok && rename(tmp, path) == 0) {
                                LOG_INFO("Favicon saved: %s", path);
                            } else {
                                unlink(tmp);
                                error = "favicon-write";
                            }
                        }
                        free_multipart(mp);
                    }

                    char target_url[96];
                    snprintf(target_url, sizeof(target_url), "/dashboard/settings%s%s",
                             error ? "?error=" : "", error ? error : "");
                    char *response = build_redirect_response(target_url, "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/settings") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[128];
                    parse_urlencoded_field(req.body, req.body_length, "site_name", name, sizeof(name));

                    if (name[0] && cms_update_site_name(name) == 0) {
                        char *response = build_redirect_response("/dashboard/settings", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        send_settings_general_page(ctx, &req, epoch, content_lang, name, NULL,
                            name[0] ? "Could not save the site name." : "The site name cannot be empty.", NULL);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/settings/comments") == 0) {
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            if (require_admin_session(ctx, &req, epoch, user_id)) {
                // An unchecked checkbox sends no field at all. Comments can't be
                // turned on without mail; turning them off always works.
                char on_str[8];
                bool on = parse_urlencoded_field(req.body, req.body_length, "comments_enabled",
                                                 on_str, sizeof(on_str)) != 0 && mailer_configured();
                cms_update_comments_enabled(on);
                send_or_error(ctx, build_redirect_response("/dashboard/settings#comments", "", epoch),
                              req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/settings/mail/test") == 0) {
            int epoch = resolve_epoch(&req);
            char user_id[USER_ID_HEX_BUF_SIZE];
            if (require_admin_session(ctx, &req, epoch, user_id)) {
                char email[256] = "", role[USER_ROLE_BUF_SIZE];
                const char *result = "failed";
                if (cms_get_user_values(user_id, email, sizeof(email), role, sizeof(role)) != 0 ||
                    !mailer_valid_address(email)) {
                    result = "no-email";
                } else {
                    char *site = cms_get_site_name();
                    char subject[200];
                    snprintf(subject, sizeof(subject), "%s - test email", site ? site : "Boat Rudder");
                    if (mailer_enqueue(email, subject,
                            "This is a test email from your site's dashboard.\n\n"
                            "If you are reading it, mail is set up correctly: visitors' comment\n"
                            "confirmations will be delivered the same way.\n") == 0)
                        result = "queued";
                    free(site);
                }
                char location[96];
                snprintf(location, sizeof(location), "/dashboard/settings?mail=%s#comments", result);
                send_or_error(ctx, build_redirect_response(location, "", epoch), req.method, epoch);
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/settings/home") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    // An unchecked checkbox sends no field at all.
                    char hide_str[8];
                    bool hide = parse_urlencoded_field(req.body, req.body_length, "hide_home_blog",
                                                       hide_str, sizeof(hide_str)) != 0;

                    if (cms_update_hide_home_blog(hide) == 0) {
                        char *response = build_redirect_response("/dashboard/settings", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        bool under_construction = false;
                        char *page = NULL;
                        cms_get_under_construction(&under_construction, &page);
                        SiteHomeSettings home = { hide, under_construction, page ? page : "" };
                        char *name = cms_get_site_name();
                        send_settings_general_page(ctx, &req, epoch, content_lang, name, &home,
                            "Could not save the home page settings.", NULL);
                        free(name);
                        free(page);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/settings/under-construction") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    // An unchecked checkbox sends no field at all.
                    char enabled_str[8];
                    bool enabled = parse_urlencoded_field(req.body, req.body_length, "under_construction",
                                                          enabled_str, sizeof(enabled_str)) != 0;
                    char page[256];
                    parse_urlencoded_field(req.body, req.body_length, "under_construction_page",
                                           page, sizeof(page));

                    // Turning it on needs a page to show: a "page" entry, drafts included.
                    const char *error_message = NULL;
                    if (enabled) {
                        CmsEntry entry = { 0 };
                        int found = page[0] && cms_get_entry_by_link(page, content_lang, 1, &entry) &&
                                    strcmp(entry.type, "page") == 0;
                        cms_entry_free(&entry);
                        if (!found) error_message = "Choose the page to show while the site is in maintenance.";
                    }
                    if (!error_message && cms_update_under_construction(enabled, page) != 0)
                        error_message = "Could not save the maintenance mode.";

                    if (!error_message) {
                        char *response = build_redirect_response("/dashboard/settings", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        SiteHomeSettings home = { cms_get_hide_home_blog(), enabled, page };
                        char *name = cms_get_site_name();
                        send_settings_general_page(ctx, &req, epoch, content_lang, name, &home,
                                                   error_message, NULL);
                        free(name);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/themes", "/activate", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_set_active_theme(id);
                    // Back to the list or to this theme's page, whichever the
                    // button was on - nothing else is accepted as a target.
                    char theme_page[128], return_to[128];
                    snprintf(theme_page, sizeof(theme_page), "/dashboard/settings/themes/%s/customize", id);
                    parse_urlencoded_field(req.body, req.body_length, "return", return_to, sizeof(return_to));
                    const char *target = strcmp(return_to, theme_page) == 0 ? theme_page : "/dashboard/settings/themes";
                    char *response = build_redirect_response(target, "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/themes", "/colors", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        CmsThemeColors colors = {0};
                        parse_bg_color_field(req.body, req.body_length, "navbar-background",
                                             "navbar-background-alpha",
                                             colors.navbar_background, sizeof(colors.navbar_background));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-normal",
                                                colors.navbar_menu_normal, sizeof(colors.navbar_menu_normal));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-hover",
                                                colors.navbar_menu_hover, sizeof(colors.navbar_menu_hover));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-active",
                                                colors.navbar_menu_active, sizeof(colors.navbar_menu_active));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-logo",
                                                colors.navbar_logo, sizeof(colors.navbar_logo));
                        parse_urlencoded_field(req.body, req.body_length, "link-normal",
                                                colors.link_normal, sizeof(colors.link_normal));
                        parse_urlencoded_field(req.body, req.body_length, "link-hover",
                                                colors.link_hover, sizeof(colors.link_hover));
                        parse_urlencoded_field(req.body, req.body_length, "link-visited",
                                                colors.link_visited, sizeof(colors.link_visited));
                        parse_urlencoded_field(req.body, req.body_length, "link-active",
                                                colors.link_active, sizeof(colors.link_active));
                        parse_bg_color_field(req.body, req.body_length, "body-background",
                                             "body-background-alpha",
                                             colors.body_background, sizeof(colors.body_background));
                        parse_urlencoded_field(req.body, req.body_length, "body-background-epoch1",
                                                colors.body_background_epoch1,
                                                sizeof(colors.body_background_epoch1));
                        parse_bg_color_field(req.body, req.body_length, "page-content-background",
                                             "page-content-background-alpha",
                                             colors.page_content_background, sizeof(colors.page_content_background));
                        parse_urlencoded_field(req.body, req.body_length, "body-text",
                                                colors.body_text, sizeof(colors.body_text));
                        parse_urlencoded_field(req.body, req.body_length, "body-text-muted",
                                                colors.body_text_muted, sizeof(colors.body_text_muted));
                        parse_urlencoded_field(req.body, req.body_length, "body-surface",
                                                colors.body_surface, sizeof(colors.body_surface));
                        parse_urlencoded_field(req.body, req.body_length, "body-surface-raised",
                                                colors.body_surface_raised, sizeof(colors.body_surface_raised));
                        parse_urlencoded_field(req.body, req.body_length, "body-border",
                                                colors.body_border, sizeof(colors.body_border));
                        parse_bg_color_field(req.body, req.body_length, "home-content-background",
                                             "home-content-background-alpha",
                                             colors.home_content_background, sizeof(colors.home_content_background));
                        parse_urlencoded_field(req.body, req.body_length, "home-content-text",
                                                colors.home_content_text, sizeof(colors.home_content_text));
                        parse_bg_color_field(req.body, req.body_length, "blog-list-item-background",
                                             "blog-list-item-background-alpha",
                                             colors.blog_list_item_background, sizeof(colors.blog_list_item_background));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-border",
                                                colors.blog_list_item_border, sizeof(colors.blog_list_item_border));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-author",
                                                colors.blog_list_item_author, sizeof(colors.blog_list_item_author));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-categories",
                                                colors.blog_list_item_categories, sizeof(colors.blog_list_item_categories));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-categories-hover",
                                                colors.blog_list_item_categories_hover, sizeof(colors.blog_list_item_categories_hover));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-date",
                                                colors.blog_list_item_date, sizeof(colors.blog_list_item_date));
                        parse_urlencoded_field(req.body, req.body_length, "table-header",
                                                colors.table_header, sizeof(colors.table_header));
                        parse_urlencoded_field(req.body, req.body_length, "table-border",
                                                colors.table_border, sizeof(colors.table_border));
                        parse_urlencoded_field(req.body, req.body_length, "table-row-a",
                                                colors.table_row_a, sizeof(colors.table_row_a));
                        parse_urlencoded_field(req.body, req.body_length, "table-row-b",
                                                colors.table_row_b, sizeof(colors.table_row_b));
                        parse_urlencoded_field(req.body, req.body_length, "code-background",
                                                colors.code_background, sizeof(colors.code_background));
                        parse_urlencoded_field(req.body, req.body_length, "code-text",
                                                colors.code_text, sizeof(colors.code_text));
                        parse_urlencoded_field(req.body, req.body_length, "code-keyword",
                                                colors.code_keyword, sizeof(colors.code_keyword));
                        parse_urlencoded_field(req.body, req.body_length, "code-string",
                                                colors.code_string, sizeof(colors.code_string));
                        parse_urlencoded_field(req.body, req.body_length, "code-comment",
                                                colors.code_comment, sizeof(colors.code_comment));
                        parse_urlencoded_field(req.body, req.body_length, "code-number",
                                                colors.code_number, sizeof(colors.code_number));
                        parse_urlencoded_field(req.body, req.body_length, "code-variable",
                                                colors.code_variable, sizeof(colors.code_variable));
                        parse_urlencoded_field(req.body, req.body_length, "code-tag",
                                                colors.code_tag, sizeof(colors.code_tag));
                        parse_urlencoded_field(req.body, req.body_length, "code-line-number",
                                                colors.code_line_number, sizeof(colors.code_line_number));
                        parse_urlencoded_field(req.body, req.body_length, "footer-logo",
                                                colors.footer_logo, sizeof(colors.footer_logo));
                        parse_bg_color_field(req.body, req.body_length, "footer-logo-background",
                                             "footer-logo-background-alpha",
                                             colors.footer_logo_background, sizeof(colors.footer_logo_background));

                        char logo_font[128] = "";
                        parse_urlencoded_field(req.body, req.body_length, "logo-font",
                                                logo_font, sizeof(logo_font));

                        cms_update_theme_colors(id, &colors);
                        cms_update_theme_logo_font(id, logo_font);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/customize", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "banner", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char html[THEME_ASSET_HTML_MAX];
                        parse_urlencoded_field(req.body, req.body_length, "html", html, sizeof(html));

                        cms_update_theme_banner(id, atoi(theme_epoch_str), html);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/banner", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "home-blog", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            // The home blog's background images (epoch 2/3) - the list's
            // ("background") and each item's ("item-background"): bare
            // filenames the theme-assets widget uploaded to content/themes/
            // <key>/home-blog/epoch<N>/, "" for the theme's own. Sanitized
            // here with the upload's own rules, since page_layout.c/
            // blog_list.c put them straight into a CSS url('...') / an HTML
            // attribute. Epoch 2's panel also posts "item-background-color"
            // ("1" when its checkbox is ticked, absent otherwise).
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char raw[256], file[256] = "", raw_item[256], item_file[256] = "";
                    parse_urlencoded_field(req.body, req.body_length, "background", raw, sizeof(raw));
                    parse_urlencoded_field(req.body, req.body_length, "item-background",
                                           raw_item, sizeof(raw_item));
                    int target = atoi(theme_epoch_str);

                    if (!theme_key_is_valid(id) || (target != EPOCH_MIDDLE && target != EPOCH_MODERN)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else if ((raw[0] && !sanitize_asset_filename(raw, file, sizeof(file))) ||
                               (raw_item[0] &&
                                !sanitize_asset_filename(raw_item, item_file, sizeof(item_file)))) {
                        send_simple(ctx, "400 Bad Request", "Invalid file name");
                    } else {
                        cms_update_theme_home_blog_background(id, target, file);
                        cms_update_theme_home_blog_item_background(id, target, item_file);
                        if (target == EPOCH_MIDDLE) {
                            char show_color[4] = "";
                            parse_urlencoded_field(req.body, req.body_length, "item-background-color",
                                                   show_color, sizeof(show_color));
                            cms_update_theme_home_blog_item_color_shown(id, target,
                                                                        strcmp(show_color, "1") == 0);
                        }
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to),
                                 "/dashboard/settings/themes/%s/customize", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "footer", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char html[THEME_ASSET_HTML_MAX];
                        parse_urlencoded_field(req.body, req.body_length, "html", html, sizeof(html));

                        cms_update_theme_footer(id, atoi(theme_epoch_str), html);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/footer", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "logo", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        int logo_epoch = atoi(theme_epoch_str);

                        CmsLogoConfig cfg = {0};
                        char mode_str[16] = "";
                        parse_urlencoded_field(req.body, req.body_length, "mode", mode_str, sizeof(mode_str));
                        parse_urlencoded_field(req.body, req.body_length, "text", cfg.text, sizeof(cfg.text));
                        parse_urlencoded_field(req.body, req.body_length, "font", cfg.font, sizeof(cfg.font));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-image", cfg.navbar_image, sizeof(cfg.navbar_image));
                        parse_urlencoded_field(req.body, req.body_length, "footer-image", cfg.footer_image, sizeof(cfg.footer_image));

                        // The mode is enforced server-side per epoch, not
                        // trusted from the submitted radio: epoch 0 is
                        // always text, epoch -1/1/2 are always image, only
                        // epoch 3 actually offers the admin a choice.
                        if (logo_epoch == 0) {
                            cfg.mode = LOGO_MODE_TEXT;
                        } else if (logo_epoch == -1 || logo_epoch == 1 || logo_epoch == 2) {
                            cfg.mode = LOGO_MODE_IMAGE;
                        } else if (strcmp(mode_str, "text") == 0) {
                            cfg.mode = LOGO_MODE_TEXT;
                        } else {
                            cfg.mode = LOGO_MODE_IMAGE;
                        }

                        cms_update_theme_logo_config(id, logo_epoch, &cfg);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/logo", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_css_editor(decoded_url, "/restore", id, sizeof(id), &css_sheet)) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        cms_update_theme_css(id, css_sheet, "");
                        char redirect_to[128];
                        theme_css_editor_url(redirect_to, sizeof(redirect_to), id, css_sheet);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_css_editor(decoded_url, "", id, sizeof(id), &css_sheet)) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char css[THEME_ASSET_HTML_MAX];
                        parse_urlencoded_field(req.body, req.body_length, "css", css, sizeof(css));

                        cms_update_theme_css(id, css_sheet, css);
                        char redirect_to[128];
                        theme_css_editor_url(redirect_to, sizeof(redirect_to), id, css_sheet);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/languages/add") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char code[16];
                    parse_urlencoded_field(req.body, req.body_length, "code", code, sizeof(code));

                    if (cms_add_language(code) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "Unknown language, or already added.");
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/default", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_set_default_language(id);

                    char *response = build_redirect_response("/dashboard/languages", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/remove", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (cms_remove_language(id) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "The default language, or the last one, cannot be removed.");
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/users/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[256], username[USERNAME_BUF_SIZE], email[256], password[256];
                    char role[USER_ROLE_BUF_SIZE];
                    parse_urlencoded_field(req.body, req.body_length, "name", name, sizeof(name));
                    parse_urlencoded_field(req.body, req.body_length, "username", username, sizeof(username));
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));

                    char *response;
                    int created = cms_create_user(name, username, email, password, role);
                    if (created == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        const char *error_message =
                            created == CMS_USER_USERNAME_INVALID
                                ? "The username must be 3-32 characters: a-z, 0-9, dot, dash or underscore, not starting with a dot."
                            : created == CMS_USER_USERNAME_TAKEN
                                ? "That username is already taken."
                                : "Could not create the user. Check the email and the password.";
                        char *content  = users_admin_form(epoch, "", name, username, email, role,
                                                          error_message);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[256], email[256], password[256], role[USER_ROLE_BUF_SIZE];
                    parse_urlencoded_field(req.body, req.body_length, "name", name, sizeof(name));
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));
                    // Fixed at creation: whatever the form sends, the stored one.
                    char username[USERNAME_BUF_SIZE];
                    cms_get_username_by_id(id, username, sizeof(username));

                    char *response;
                    if (strcmp(id, user_id) == 0 && strcmp(role, "admin") != 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_form(epoch, id, name, username, email, role,
                                              "You are the only administrator: you cannot remove your own administrator role.");
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (strcmp(role, "commenter") != 0 && !password[0] && !cms_user_has_password(id)) {
                        // A commenter promoted to the team needs a way to sign in.
                        char *content  = users_admin_form(epoch, id, name, username, email, role,
                                              "This user has no password yet (a commenter): set one so they can sign in.");
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_update_user(id, name, email, role, password) == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        char *content  = users_admin_form(epoch, id, name, username, email, role,
                                              "Could not update the user. Check the email.");
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char target_role[USER_ROLE_BUF_SIZE];
                    char *response;

                    if (strcmp(id, user_id) == 0) {
                        char *content  = users_admin_list(epoch, "You cannot delete your own account.", 0);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_get_user_role(id, target_role, sizeof(target_role)) == 0 &&
                               strcmp(target_role, "admin") == 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_list(epoch, "The last administrator cannot be deleted.", 0);
                        char *body     = buildPageWebSite(epoch, "{{SITE_NAME}} - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else {
                        cms_delete_user(id);
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        // ---- Media admin POST routes ----
        //
        // Every on-disk path below is built from DB records and from names
        // that passed media_dir_name_valid()/media_username_valid() - never
        // from a client-supplied path - and every change is gated by
        // media_can_manage(): an Autor only touches their own directories
        // and items.

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    char name[256];
                    parse_urlencoded_field(req.body, req.body_length, "newpath", name, sizeof(name));

                    char username[64] = {0};
                    cms_get_username_by_id(user_id, username, sizeof(username));

                    CmsMediaDirectory existing;
                    if (!media_dir_name_valid(name)) {
                        send_simple(ctx, "400 Bad Request", "Invalid directory name (3-60 chars, a-z 0-9 _ -)");
                    } else if (!media_username_valid(username)) {
                        LOG_WARN("media directory create: unusable username '%s' for user %s", username, user_id);
                        send_simple(ctx, "400 Bad Request", "Your account name cannot be used as a media directory");
                    } else if (cms_get_media_directory_by_name(name, user_id, &existing)) {
                        send_simple(ctx, "409 Conflict", "A directory with that name already exists");
                    } else {
                        char dirpath[512];
                        snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, name);

                        struct stat st;
                        if (stat(dirpath, &st) == -1) {
                            char parent_path[512];
                            snprintf(parent_path, sizeof(parent_path), "./html/content/posts/%s", username);
                            mkdir(parent_path, 0775);
                            mkdir(dirpath, 0775);
                        }

                        char new_id[25];
                        if (cms_create_media_directory(name, "posts", user_id, new_id) == 0) {
                            CmsMediaDirectory dir;
                            if (cms_get_media_directory_by_id(new_id, &dir)) {
                                snprintf(dir.author_name, sizeof(dir.author_name), "%s", username);
                                media_label_owners(&dir, 1, role, user_id);
                                char *html = media_admin_render_directory_item(&dir, epoch);
                                if (html) {
                                    send_html_fragment(ctx, html);
                                    free(html);
                                } else {
                                    send_simple(ctx, "500 Internal Server Error", "Render error");
                                }
                            } else {
                                send_simple(ctx, "500 Internal Server Error", "DB lookup error");
                            }
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "DB insert error");
                        }
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/rename") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    // Only the directory id and the new name are read from the
                    // body; the current name and the owner come from the DB.
                    char dir_id[32], newname[256];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));
                    parse_urlencoded_field(req.body, req.body_length, "newname", newname, sizeof(newname));

                    CmsMediaDirectory dir, clash;
                    char owner[64] = {0};

                    if (!cms_get_media_directory_by_id(dir_id, &dir)) {
                        send_simple(ctx, "404 Not Found", "Unknown directory");
                    } else if (!media_can_manage(role, user_id, dir.author_id)) {
                        send_simple(ctx, "403 Forbidden", "You can only rename your own directories");
                    } else if (!media_dir_name_valid(newname)) {
                        send_simple(ctx, "400 Bad Request", "Invalid directory name (3-60 chars, a-z 0-9 _ -)");
                    } else if (cms_get_username_by_id(dir.author_id, owner, sizeof(owner)) != 0 ||
                               !media_username_valid(owner) || !media_dir_name_valid(dir.name)) {
                        LOG_WARN("media directory rename: unusable stored path for dir %s ('%s'/'%s')",
                                 dir.id, owner, dir.name);
                        send_simple(ctx, "409 Conflict", "This directory cannot be renamed");
                    } else if (strcmp(dir.name, newname) != 0 &&
                               cms_get_media_directory_by_name(newname, dir.author_id, &clash)) {
                        send_simple(ctx, "409 Conflict", "A directory with that name already exists");
                    } else {
                        char old_path[512], new_path[512];
                        snprintf(old_path, sizeof(old_path), "./html/content/posts/%s/%s", owner, dir.name);
                        snprintf(new_path, sizeof(new_path), "./html/content/posts/%s/%s", owner, newname);

                        struct stat st;
                        int ok;
                        if (strcmp(dir.name, newname) == 0) {
                            ok = 1;
                        } else if (stat(new_path, &st) == 0) {
                            ok = 0;
                            LOG_WARN("media directory rename: %s already exists on disk", new_path);
                        } else {
                            // ENOENT: the directory was never created on disk
                            // (no upload yet) - only the record changes.
                            ok = rename(old_path, new_path) == 0 || errno == ENOENT;
                            if (!ok) LOG_ERROR("media directory rename %s -> %s: %s",
                                               old_path, new_path, strerror(errno));
                        }

                        if (!ok || cms_rename_media_directory(dir.id, newname) != 0) {
                            send_simple(ctx, "500 Internal Server Error", "Could not rename directory");
                        } else {
                            CmsMediaDirectory renamed;
                            char *html = NULL;
                            if (cms_get_media_directory_by_id(dir.id, &renamed)) {
                                snprintf(renamed.author_name, sizeof(renamed.author_name), "%s", owner);
                                media_label_owners(&renamed, 1, role, user_id);
                                html = media_admin_render_directory_item(&renamed, epoch);
                            }
                            if (html) {
                                send_html_fragment(ctx, html);
                                free(html);
                            } else {
                                send_simple(ctx, "500 Internal Server Error", "Render error");
                            }
                        }
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    char dir_id[32];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));

                    CmsMediaDirectory dir;
                    int64_t item_count;

                    if (!cms_get_media_directory_by_id(dir_id, &dir)) {
                        send_simple(ctx, "404 Not Found", "Unknown directory");
                    } else if (!media_can_manage(role, user_id, dir.author_id)) {
                        send_simple(ctx, "403 Forbidden", "You can only delete your own directories");
                    } else if ((item_count = cms_count_media_in_directory(dir.id)) != 0) {
                        // Deleting it would orphan the items' records and files.
                        if (item_count < 0)
                            send_simple(ctx, "500 Internal Server Error", "DB error");
                        else
                            send_simple(ctx, "409 Conflict", "The directory is not empty. Delete or move its images first.");
                    } else {
                        char owner[64] = {0};
                        int disk_ok = 1;

                        if (cms_get_username_by_id(dir.author_id, owner, sizeof(owner)) == 0 &&
                            media_username_valid(owner) && media_dir_name_valid(dir.name)) {
                            char dirpath[512];
                            snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", owner, dir.name);
                            if (rmdir(dirpath) != 0 && errno != ENOENT) {
                                disk_ok = 0;
                                LOG_WARN("media directory delete: rmdir %s: %s", dirpath, strerror(errno));
                            }
                        } else {
                            // A legacy record whose name is not a safe path
                            // component: drop the record, touch nothing on disk.
                            LOG_WARN("media directory delete: skipping disk for dir %s ('%s'/'%s')",
                                     dir.id, owner, dir.name);
                        }

                        if (!disk_ok)
                            send_simple(ctx, "409 Conflict", "The directory still contains files on disk");
                        else if (cms_delete_media_directory(dir.id) != 0)
                            send_simple(ctx, "500 Internal Server Error", "Could not delete directory");
                        else
                            send_simple(ctx, "200 OK", "Deleted");
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    char ids[4096];
                    parse_urlencoded_field(req.body, req.body_length, "ids", ids, sizeof(ids));

                    int deleted = 0, failed = 0, forbidden = 0;
                    char *saveptr = NULL;
                    for (char *tok = strtok_r(ids, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
                        CmsMediaItem item;
                        if (!cms_get_media_item_by_id(tok, &item)) {
                            failed++;
                        } else if (!media_can_manage(role, user_id, item.author_id)) {
                            forbidden++;
                        } else {
                            if (media_item_paths_valid(&item))
                                media_delete_variants(&item);
                            else
                                LOG_WARN("media delete: skipping disk for item %s (unsafe stored path)", item.id);
                            if (cms_delete_media(tok) == 0) deleted++; else failed++;
                        }
                    }

                    LOG_INFO("media delete: deleted=%d failed=%d forbidden=%d", deleted, failed, forbidden);
                    if (deleted == 0 && forbidden > 0)
                        send_simple(ctx, "403 Forbidden", "You can only delete your own images");
                    else if (deleted == 0 && failed > 0)
                        send_simple(ctx, "500 Internal Server Error", "Could not delete images");
                    else
                        send_simple(ctx, "200 OK", "Deleted");
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/move") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    char ids[4096], dest_dir[32];
                    parse_urlencoded_field(req.body, req.body_length, "ids", ids, sizeof(ids));
                    parse_urlencoded_field(req.body, req.body_length, "dest_dir", dest_dir, sizeof(dest_dir));

                    CmsMediaDirectory dest;
                    if (!cms_get_media_directory_by_id(dest_dir, &dest)) {
                        send_simple(ctx, "400 Bad Request", "Unknown destination directory");
                    } else if (!media_can_manage(role, user_id, dest.author_id)) {
                        send_simple(ctx, "403 Forbidden", "You can only move images into your own directories");
                    } else if (!media_dir_name_valid(dest.name)) {
                        send_simple(ctx, "400 Bad Request", "Invalid destination directory");
                    } else {
                        int moved = 0, failed = 0, forbidden = 0;
                        char *saveptr = NULL;
                        for (char *tok = strtok_r(ids, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
                            CmsMediaItem item;
                            if (!cms_get_media_item_by_id(tok, &item)) {
                                failed++;
                            } else if (!media_can_manage(role, user_id, item.author_id) ||
                                       strcmp(item.author_id, dest.author_id) != 0) {
                                // Files live under their author's tree, so a
                                // directory of another author is no valid target.
                                forbidden++;
                            } else if (media_item_paths_valid(&item) &&
                                       media_move_variants(&item, dest.name) == 0 &&
                                       cms_move_media(tok, dest.id) == 0) {
                                moved++;
                            } else {
                                failed++;
                            }
                        }

                        LOG_INFO("media move: moved=%d failed=%d forbidden=%d to dir=%s",
                                 moved, failed, forbidden, dest.id);
                        if (moved == 0 && forbidden > 0)
                            send_simple(ctx, "403 Forbidden", "Images can only be moved between their author's directories");
                        else if (moved == 0 && failed > 0)
                            send_simple(ctx, "500 Internal Server Error", "Could not move images");
                        else
                            send_simple(ctx, "200 OK", "Moved");
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    const char *ct = get_header_value(&req, "Content-Type");
                    MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                    if (!mp) {
                        send_simple(ctx, "400 Bad Request", "Invalid multipart body");
                    } else {
                        const MultipartPart *dir_part = multipart_find(mp, "media-directory-selected");
                        const MultipartPart *file_part = multipart_find(mp, "file");

                        char sanitized[256] = {0};
                        if (file_part) media_sanitize_upload_name(file_part->filename, sanitized, sizeof(sanitized));

                        char dir_id_buf[32] = {0};
                        if (dir_part) {
                            size_t dlen = dir_part->data_len < sizeof(dir_id_buf) - 1 ? dir_part->data_len : sizeof(dir_id_buf) - 1;
                            memcpy(dir_id_buf, dir_part->data, dlen);
                        }

                        char username[64] = {0};
                        cms_get_username_by_id(user_id, username, sizeof(username));

                        // No directory selected (or the id no longer exists): fall
                        // back to this author's "default" directory rather than
                        // rejecting the upload. A directory of another author is
                        // refused: uploads always land in the caller's own tree.
                        CmsMediaDirectory dir;
                        int dir_found = cms_get_media_directory_by_id(dir_id_buf, &dir);

                        // The content decides the format; the name only follows it.
                        const char *image_ext = file_part
                            ? media_image_extension((const unsigned char *)file_part->data, file_part->data_len)
                            : NULL;

                        if (!file_part || file_part->filename[0] == '\0') {
                            send_simple(ctx, "400 Bad Request", "Missing file");
                        } else if (!image_ext) {
                            // Not a JPEG, PNG, GIF or WebP, whatever its name says:
                            // its first bytes show what it is (AVIF, HEIC, ...).
                            const unsigned char *d = (const unsigned char *)file_part->data;
                            size_t n = file_part->data_len;
                            LOG_WARN("media upload rejected: '%s' (%zu bytes, starts "
                                     "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x)",
                                     file_part->filename, n,
                                     n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, n > 2 ? d[2] : 0,
                                     n > 3 ? d[3] : 0, n > 4 ? d[4] : 0, n > 5 ? d[5] : 0,
                                     n > 6 ? d[6] : 0, n > 7 ? d[7] : 0, n > 8 ? d[8] : 0,
                                     n > 9 ? d[9] : 0, n > 10 ? d[10] : 0, n > 11 ? d[11] : 0);
                            send_simple(ctx, "415 Unsupported Media Type",
                                        "Only JPEG, PNG, GIF and WebP images can be uploaded");
                        } else if (!media_name_with_extension(sanitized, sizeof(sanitized), image_ext) ||
                                   !media_file_name_valid(sanitized)) {
                            send_simple(ctx, "400 Bad Request", "Invalid file name");
                        } else if (!media_username_valid(username)) {
                            LOG_WARN("media upload: unusable username '%s' for user %s", username, user_id);
                            send_simple(ctx, "400 Bad Request", "Your account name cannot be used as a media directory");
                        } else if (dir_found && strcmp(dir.author_id, user_id) != 0) {
                            send_simple(ctx, "403 Forbidden", "You can only upload into your own directories");
                        } else if (!dir_found &&
                                   resolve_or_create_default_media_directory(user_id, username, &dir) != 0) {
                            send_simple(ctx, "500 Internal Server Error", "Could not resolve upload directory");
                        } else if (!media_dir_name_valid(dir.name)) {
                            send_simple(ctx, "400 Bad Request", "Invalid upload directory");
                        } else {
                            strncpy(dir_id_buf, dir.id, sizeof(dir_id_buf) - 1);

                            // Create dir on disk
                            char dirpath[512];
                            snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, dir.name);
                            mkdir(dirpath, 0775);

                            if (!media_unique_name(dirpath, sanitized, sizeof(sanitized))) {
                                send_simple(ctx, "409 Conflict", "Could not find a free name for this file");
                                free_multipart(mp);
                                goto cleanup;
                            }

                            // Write file
                            char filepath[1024];
                            snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, sanitized);
                            // A new file only: O_EXCL fails on any existing name and
                            // O_NOFOLLOW on a symlink, so an upload can never write
                            // through an earlier upload's base-name link into its
                            // _half variant, or over any other file.
                            int fd = open(filepath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
                            FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
                            if (fd >= 0 && !f) close(fd);
                            int write_ok = 0;
                            if (f) {
                                write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                                fclose(f);
                            }

                            if (!write_ok) {
                                send_simple(ctx, "500 Internal Server Error", "Could not write file");
                            } else {
                                LOG_INFO("Media file saved: %s (%zu bytes)", filepath, file_part->data_len);

                                // Run image optimizer. Every argument is made of
                                // validated [A-Za-z0-9._-] components, so the single
                                // quotes cannot be broken out of.
                                char command[4096];
                                snprintf(command, sizeof(command),
                                         "./scripts/image-optimizer.sh '%s' '%s' '%s'",
                                         dirpath, dirpath, filepath);
                                LOG_INFO("Running optimizer: %s", command);

                                char optimized_path[1024] = {0};
                                FILE *pipe = popen(command, "r");
                                if (pipe) {
                                    if (fgets(optimized_path, sizeof(optimized_path), pipe))
                                        optimized_path[strcspn(optimized_path, "\n")] = '\0';
                                    pclose(pipe);
                                }

                                // The optimizer prints the _full variant it wrote. Anything
                                // else - an error, its "Skipping ... (not an image)" line, or
                                // nothing - means no variants exist: discard the upload
                                // instead of publishing the raw file.
                                size_t dlen = strlen(dirpath);
                                struct stat ost;
                                int optimized_ok = strncmp(optimized_path, dirpath, dlen) == 0 &&
                                                   optimized_path[dlen] == '/' &&
                                                   !strstr(optimized_path + dlen + 1, "/") &&
                                                   stat(optimized_path, &ost) == 0 && S_ISREG(ost.st_mode);

                                if (!optimized_ok) {
                                    LOG_WARN("media upload: optimizer produced no output for %s ('%s')",
                                             filepath, optimized_path);
                                    unlink(filepath);
                                    send_simple(ctx, "415 Unsupported Media Type", "The file could not be processed as an image");
                                } else {
                                    // Build filename for DB: strip path, remove _full suffix
                                    char *basename = strrchr(optimized_path, '/');
                                    basename = basename ? basename + 1 : optimized_path;
                                    char db_name[512];
                                    strncpy(db_name, basename, sizeof(db_name) - 1);
                                    db_name[sizeof(db_name) - 1] = '\0';
                                    char *dot = strrchr(db_name, '.');
                                    if (dot) {
                                        char *full_tag = strstr(db_name, "_full");
                                        if (full_tag && full_tag < dot)
                                            memmove(full_tag, full_tag + 5, strlen(full_tag + 5) + 1);
                                    }

                                    char media_id[25];
                                    cms_insert_media(db_name, user_id, dir_id_buf, media_id);

                                    // dir_id rides along so the picker can select/reveal
                                    // the "default" directory when none was chosen up front.
                                    char json_response[2048];
                                    snprintf(json_response, sizeof(json_response),
                                             "{\"ok\":true,\"filename\":\"%s\",\"dir_id\":\"%s\"}",
                                             optimized_path, dir_id_buf);
                                    char *response = build_json_response(json_response);
                                    connection_write(ctx, response, strlen(response));
                                    free(response);
                                }
                            }
                        }
                        free_multipart(mp);
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (strcmp(decoded_url, "/dashboard/api/browsers/preview") == 0 ||
                    strcmp(decoded_url, "/dashboard/api/browsers") == 0)) {
            // The Browser detection page's JSON endpoints: a dry run of the
            // edited rules, and saving them (browser_detection_admin.h).
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *ua = get_header_value(&req, "User-Agent");
                    const char *status = "500 Internal Server Error";
                    char *json = strcmp(decoded_url, "/dashboard/api/browsers") == 0
                        ? browser_detection_admin_save(req.body, (size_t)req.body_length, ua, user_id, &status)
                        : browser_detection_admin_preview(req.body, (size_t)req.body_length, ua, &status);
                    char *response = build_json_response_status(json ? json : "{\"error\":\"out of memory\"}",
                                                                status);
                    free(json);
                    if (response) connection_write(ctx, response, strlen(response));
                    free(response);
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/settings/browsers/reset") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    // The factory rules grant the dashboard to every browser
                    // that can reach this page, so no lock-out check here.
                    cms_browser_detection_reset();
                    cms_browser_detection_reload();
                    char *response = build_redirect_response("/dashboard/settings/browsers", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/settings/updates/check") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    updates_admin_check_now();
                    char *response = build_redirect_response("/dashboard/settings/updates", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   (strcmp(decoded_url, "/dashboard/settings/updates/install") == 0 ||
                    strcmp(decoded_url, "/dashboard/settings/updates/rollback") == 0)) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    int install = strstr(decoded_url, "/install") != NULL;
                    char email[256] = "";
                    char version[64] = "";
                    const char *error = "password";
                    if (confirm_password(&req, user_id, email, sizeof(email))) {
                        parse_urlencoded_field(req.body, req.body_length, "version", version, sizeof(version));
                        error = updates_admin_request(install ? "install" : "rollback",
                                                      install ? version : NULL, email);
                    }
                    char location[96];
                    snprintf(location, sizeof(location), "/dashboard/settings/updates%s%s",
                             error ? "?error=" : "", error ? error : "");
                    char *response = build_redirect_response(location, "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/settings/fonts/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *ct = get_header_value(&req, "Content-Type");
                    MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                    const MultipartPart *name_part = mp ? multipart_find(mp, "name") : NULL;
                    const MultipartPart *file_part = mp ? multipart_find(mp, "file") : NULL;

                    // Font names become both a Mongo document field and a
                    // literal CSS font-family value (see page_layout.c's
                    // build_logo_font_css()) - restricted to a plain-text
                    // whitelist so neither can break out of either context.
                    char name[81] = "";
                    if (name_part && name_part->data_len > 0) {
                        size_t len = name_part->data_len < sizeof(name) - 1
                            ? name_part->data_len : sizeof(name) - 1;
                        size_t j = 0;
                        for (size_t k = 0; k < len; k++) {
                            char c = name_part->data[k];
                            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')
                                name[j++] = c;
                        }
                        name[j] = '\0';
                    }

                    char sanitized[256];
                    int valid_ext = 0;
                    if (file_part && file_part->filename[0] &&
                        sanitize_asset_filename(file_part->filename, sanitized, sizeof(sanitized))) {
                        const char *dot = strrchr(sanitized, '.');
                        static const char *allowed[] = {".ttf", ".otf", ".woff", ".woff2"};
                        for (size_t i = 0; dot && i < sizeof(allowed) / sizeof(allowed[0]); i++)
                            if (strcasecmp(dot, allowed[i]) == 0) { valid_ext = 1; break; }
                    }

                    if (!name[0] || !valid_ext) {
                        char *response = build_redirect_response(
                            "/dashboard/settings/fonts?error=Name+and+a+.ttf%2F.otf%2F.woff%2F.woff2+file+are+required",
                            "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        mkdir_recursive("./html/assets/fonts");

                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./html/assets/fonts/%s", sanitized);

                        FILE *f = fopen(filepath, "wb");
                        int write_ok = 0;
                        if (f) {
                            write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                            fclose(f);
                        }

                        if (!write_ok) {
                            char *response = build_redirect_response(
                                "/dashboard/settings/fonts?error=Could+not+write+font+file", "", epoch);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            cms_add_font(name, sanitized);
                            LOG_INFO("Font saved: %s (%s)", filepath, name);
                            char *response = build_redirect_response("/dashboard/settings/fonts", "", epoch);
                            send_or_error(ctx, response, req.method, epoch);
                        }
                    }

                    free_multipart(mp);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/fonts", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char filename[256];
                    if (cms_get_font_filename(id, filename, sizeof(filename)) && filename[0]) {
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./html/assets/fonts/%s", filename);
                        remove(filepath);
                    }
                    cms_delete_font(id);
                    char *response = build_redirect_response("/dashboard/settings/fonts", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/theme-assets/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *theme_key = get_query_param(params, param_count, "theme");
                    const char *component = get_query_param(params, param_count, "component");
                    const char *epoch_str = get_query_param(params, param_count, "epoch");

                    char dir[256];
                    if (!theme_assets_dir(theme_key, component, epoch_str, dir, sizeof(dir))) {
                        send_simple(ctx, "400 Bad Request", "Invalid theme, component or epoch");
                    } else {
                        const char *ct = get_header_value(&req, "Content-Type");
                        MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                        const MultipartPart *file_part = mp ? multipart_find(mp, "file") : NULL;
                        char sanitized[256];
                        int is_epoch_neg1 = epoch_str && strcmp(epoch_str, "-1") == 0;
                        int ext_ok = file_part && file_part->filename[0] &&
                            (is_epoch_neg1 ? epoch_neg1_upload_extension_ok(file_part->filename)
                                           : theme_asset_image_extension_ok(file_part->filename));
                        if (!file_part || file_part->filename[0] == '\0' ||
                            !sanitize_asset_filename(file_part->filename, sanitized, sizeof(sanitized)) ||
                            !ext_ok) {
                            send_simple(ctx, "400 Bad Request", "Missing or invalid file");
                        } else {
                            mkdir_recursive(dir);

                            char filepath[512];
                            snprintf(filepath, sizeof(filepath), "%s/%s", dir, sanitized);

                            FILE *f = fopen(filepath, "wb");
                            int write_ok = 0;
                            if (f) {
                                write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                                fclose(f);
                            }

                            // Epoch -1 (WAP/WML): keep the PNG, write its
                            // WBMP twin, and report the WBMP's name - the
                            // one WML references (the logo form stores it;
                            // the WAP preview swaps .wbmp for .png).
                            if (write_ok && is_epoch_neg1) {
                                char wbmp_name[256];
                                char wbmp_path[512];
                                if (wap_twin_name(sanitized, ".wbmp", wbmp_name, sizeof(wbmp_name)) &&
                                    snprintf(wbmp_path, sizeof(wbmp_path), "%s/%s", dir, wbmp_name) <
                                        (int)sizeof(wbmp_path) &&
                                    image_convert_to_wbmp(filepath, wbmp_path, 200) == 0) {
                                    strncpy(sanitized, wbmp_name, sizeof(sanitized) - 1);
                                    sanitized[sizeof(sanitized) - 1] = '\0';
                                } else {
                                    remove(filepath);
                                    write_ok = 0;
                                }
                            }

                            if (!write_ok) {
                                send_simple(ctx, "500 Internal Server Error", "Could not write file");
                            } else {
                                LOG_INFO("Theme asset saved: %s/%s", dir, sanitized);
                                char *escaped = json_escape_alloc(sanitized);
                                char *json = render_template("{\"ok\":true,\"filename\":\"%s\"}", escaped ? escaped : sanitized);
                                free(escaped);
                                char *response = build_json_response(json ? json : "{\"ok\":true}");
                                free(json);
                                connection_write(ctx, response, strlen(response));
                                free(response);
                            }
                        }

                        free_multipart(mp);
                        connection_close(ctx);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/theme-assets/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (!dashboard_allowed(&req, epoch)) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *theme_key = get_query_param(params, param_count, "theme");
                    const char *component = get_query_param(params, param_count, "component");
                    const char *epoch_str = get_query_param(params, param_count, "epoch");
                    const char *file = get_query_param(params, param_count, "file");

                    char dir[256];
                    char sanitized[256];
                    if (!theme_assets_dir(theme_key, component, epoch_str, dir, sizeof(dir)) ||
                        !sanitize_asset_filename(file, sanitized, sizeof(sanitized)) ||
                        strcmp(file, sanitized) != 0) {
                        send_simple(ctx, "400 Bad Request", "Invalid theme, component, epoch or file");
                    } else {
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "%s/%s", dir, sanitized);
                        unlink(filepath);

                        // Epoch -1 images come in pairs: either one takes the other along.
                        char twin[256];
                        const char *twin_ext = has_extension_ci(sanitized, ".wbmp") ? ".png"
                                             : has_extension_ci(sanitized, ".png") ? ".wbmp" : NULL;
                        if (strcmp(epoch_str, "-1") == 0 && twin_ext &&
                            wap_twin_name(sanitized, twin_ext, twin, sizeof(twin))) {
                            snprintf(filepath, sizeof(filepath), "%s/%s", dir, twin);
                            unlink(filepath);
                        }
                        send_simple(ctx, "200 OK", "Deleted");
                    }
                }
            }

        } else if (strcmp(req.method, "OPTIONS") == 0) {
            const char *opts =
                "HTTP/1.1 204 No Content\r\n"
                "Allow: GET, HEAD, OPTIONS, POST\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            connection_write(ctx, opts, strlen(opts));

        } else {
            send_error_response(ctx, 405, "405 Method Not Allowed", resolve_epoch(&req));
        }
    }

cleanup:
    free(raw_request);
    if (req.body) { free(req.body); req.body = NULL; }
    connection_close(ctx);
}
