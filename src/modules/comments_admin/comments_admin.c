#include "comments_admin.h"
#include "../../db/cms_comments.h"
#include "../comments/comments.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *html_encode_alloc(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (out) html_encode(out, src, cap);
    return out;
}

// Escaped, line breaks as <br>.
static char *text_html(const char *text) {
    char *enc = html_encode_alloc(text);
    if (!enc) return NULL;
    char *out = strdup("");
    for (const char *p = enc; out && *p; p++) {
        char one[2] = { *p, 0 };
        out = str_append(out, *p == '\n' ? "<br>" : one);
    }
    free(enc);
    return out;
}

static char *action(const char *tpl, const char *path, const char *back, const char *modifier,
                    const char *confirm, const char *label) {
    char attr[160] = "";
    if (confirm) snprintf(attr, sizeof(attr), " onsubmit=\"return confirm('%s');\"", confirm);
    return render_template(tpl, path, back, modifier, attr, label);
}

static char *row_actions(const char *tpl, const CmsCommentAdmin *c, const char *status, int can_delete,
                         int can_block) {
    char path[96];
    char *out = strdup("");
    struct { const char *verb, *label, *modifier, *confirm; int show; } acts[] = {
        { "approve", strcmp(status, "rejected") == 0 ? "Restore" : "Approve", " boat-rudder-btn--primary", NULL,
          strcmp(status, "approved") != 0 },
        { "reject", strcmp(status, "approved") == 0 ? "Hide" : "Reject", "", NULL, strcmp(status, "rejected") != 0 },
        { "delete", "Delete", " boat-rudder-btn--danger", "Delete this comment for good?", can_delete },
        { "resend", "Resend email", "", NULL, strcmp(status, "unverified") == 0 },
    };
    for (size_t i = 0; out && i < sizeof(acts) / sizeof(acts[0]); i++) {
        if (!acts[i].show) continue;
        snprintf(path, sizeof(path), "%s/%s", c->id, acts[i].verb);
        char *a = action(tpl, path, status, acts[i].modifier, acts[i].confirm, acts[i].label);
        out = a ? str_append(out, a) : (free(out), NULL);
        free(a);
    }
    if (out && can_block && c->commenter_id[0]) {
        char *a;
        if (c->commenter_blocked) {
            snprintf(path, sizeof(path), "commenter/%s/unblock", c->commenter_id);
            a = action(tpl, path, status, "", NULL, "Unblock email");
        } else {
            snprintf(path, sizeof(path), "commenter/%s/block", c->commenter_id);
            a = action(tpl, path, status, "", "Refuse every future comment from this email?", "Block email");
            char *b = NULL;
            if (a) {
                snprintf(path, sizeof(path), "commenter/%s/block-delete", c->commenter_id);
                b = action(tpl, path, status, " boat-rudder-btn--danger",
                           "Block this email and delete everything it ever wrote?", "Block and delete all");
                a = b ? str_append(a, b) : (free(a), NULL);
                free(b);
            }
        }
        out = a ? str_append(out, a) : (free(out), NULL);
        free(a);
    }
    return out;
}

// "Windows/8_1" -> "Windows 8.1", "UP_Browser/4" -> "UP.Browser 4": ua_parser.h's
// keys as a person reads them. "" for an unknown or missing one.
static void readable_client(const char *key, char *out, size_t size) {
    out[0] = '\0';
    if (!key || !key[0] || strcmp(key, "Unknown") == 0) return;
    size_t k = 0;
    for (const char *p = key; *p && k + 1 < size; p++) out[k++] = *p == '/' ? ' ' : *p == '_' ? '.' : *p;
    out[k] = '\0';
}

// The date, and under it where the comment was written from: the browser's
// and system's icons, then browser, OS and epoch in words (comments older
// than that record show the date alone).
static char *client_line(const CmsCommentAdmin *c) {
    char browser[64], os[64], parts[200] = "";
    readable_client(c->browser, browser, sizeof(browser));
    readable_client(c->os, os, sizeof(os));
    if (browser[0]) snprintf(parts, sizeof(parts), "%s", browser);
    if (os[0]) snprintf(parts + strlen(parts), sizeof(parts) - strlen(parts), "%s%s", parts[0] ? " · " : "", os);
    if (c->epoch >= 0)
        snprintf(parts + strlen(parts), sizeof(parts) - strlen(parts), "%sepoch %d", parts[0] ? " · " : "", c->epoch);
    if (!parts[0]) return strdup(c->date);
    char *enc = html_encode_alloc(parts);
    char *icons = comment_client_html(EPOCH_MODERN, c->browser, c->os, c->user_agent);
    char *out = enc && icons ? render_template("%s<br><small>%s %s</small>", c->date, icons, enc) : NULL;
    free(enc);
    free(icons);
    return out;
}

char *comments_admin_page(int epoch, const char *status, const char *owner_id, int can_delete,
                          int can_block, const char *lang, const char *notice) {
    char *list_tpl = load_template("dashboard/comments/list_epoch%d.html", epoch);
    char *row_tpl = load_template("dashboard/comments/list-row_epoch%d.html", epoch);
    char *action_tpl = load_template("dashboard/comments/action_epoch%d.html", epoch);
    char *rows = NULL, *notice_html = NULL, *result = NULL;
    CmsCommentAdmin *list = NULL;
    int count = 0;
    if (!list_tpl || !row_tpl || !action_tpl) goto cleanup;

    count = cms_comments_admin_list(status, owner_id, lang, &list);
    rows = strdup("");
    if (count < 0) {
        notice = "Comments could not be loaded (database error).";
        count = 0;
    }
    for (int i = 0; rows && i < count; i++) {
        const CmsCommentAdmin *c = &list[i];
        char url[300];
        snprintf(url, sizeof(url), "/blog/%s", c->entry_link);
        char *url_enc = html_encode_alloc(url);
        char *title = html_encode_alloc(c->entry_title[0] ? c->entry_title : c->entry_link);
        char *name = html_encode_alloc(c->name);
        char *email = html_encode_alloc(c->email);
        char *text = text_html(c->text);
        char *acts = row_actions(action_tpl, c, status, can_delete, can_block);
        // An unconfirmed comment says how its confirmation email is doing.
        char badge[700] = "";
        if (c->commenter_blocked) snprintf(badge, sizeof(badge), "<br><span class=\"boat-rudder-badge\">Blocked</span>");
        if (strcmp(status, "unverified") == 0) {
            char *err = html_encode_alloc(c->mail_error ? c->mail_error : "");
            const char *state = strcmp(c->mail_status, "sent") == 0 ? "Email sent, not confirmed yet"
                              : strcmp(c->mail_status, "failed") == 0 ? "Email failed"
                              : strcmp(c->mail_status, "queued") == 0 ? (err && err[0] ? "Email not sent yet - retrying" : "Email queued")
                              : "No email found";
            size_t used = strlen(badge);
            snprintf(badge + used, sizeof(badge) - used, "<br><span class=\"boat-rudder-badge\">%s</span>%s%s%s",
                     state, err && err[0] ? "<br><small>" : "", err && err[0] ? err : "", err && err[0] ? "</small>" : "");
            free(err);
        }
        char *when = client_line(c);
        char *row = (url_enc && title && name && email && text && acts && when)
            ? render_template(row_tpl, url_enc, title, name, email, badge, text, when, acts) : NULL;
        rows = row ? str_append(rows, row) : (free(rows), NULL);
        free(url_enc); free(title); free(name); free(email); free(text); free(acts); free(row); free(when);
    }
    if (rows && count == 0) {
        char *empty = load_template("dashboard/comments/list-empty_epoch%d.html", epoch);
        if (empty) { rows = str_append(rows, empty); free(empty); }
    }

    notice_html = strdup("");
    if (notice && notice[0]) {
        char *ntpl = load_template("dashboard/comments/notice_epoch%d.html", epoch);
        char *enc = html_encode_alloc(notice);
        if (ntpl && enc) { free(notice_html); notice_html = render_template(ntpl, enc); }
        free(ntpl);
        free(enc);
    }

    long pending = cms_comments_pending_count(owner_id);
    long unverified = cms_comments_count("unverified", owner_id);
    const char *active = " boat-rudder-tabs__tab--active";
    if (rows && notice_html)
        result = render_template(list_tpl,
                                 strcmp(status, "pending") == 0 ? active : "",
                                 strcmp(status, "approved") == 0 ? active : "",
                                 strcmp(status, "rejected") == 0 ? active : "",
                                 (int)(pending > 0 ? pending : 0), notice_html, rows,
                                 owner_id ? "Comments on your own blog entries." : "Comments on every blog entry.",
                                 strcmp(status, "unverified") == 0 ? active : "",
                                 (int)(unverified > 0 ? unverified : 0));

cleanup:
    cms_comments_admin_free(list, count);
    free(list_tpl); free(row_tpl); free(action_tpl); free(rows); free(notice_html);
    return result;
}

char *comments_admin_pending_notice(int epoch, const char *owner_id) {
    if (epoch != EPOCH_MODERN) return strdup("");
    long n = cms_comments_pending_count(owner_id);
    if (n <= 0) return strdup("");
    return render_template("<p class=\"boat-rudder-alert\">%ld comment%s waiting for moderation. "
                           "<a href=\"/dashboard/comments\">Moderate</a></p>\n", n, n == 1 ? " is" : "s are");
}
