#include "comments.h"
#include "../../db/cms_site_settings.h"
#include "../../db/cms_themes.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/captcha.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/log.h"
#include "../../utils/mailer.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <ctype.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The remember-this-browser cookie: "<commenter id>.<issued>.<mac>", where
// mac = HMAC-SHA256(site secret, "commenter|<id>|<issued>").
#define REMEMBER_COOKIE      "br_commenter"
#define REMEMBER_DAYS        365

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

int comments_available(void) {
    return mongodb_manager_is_ready() && mailer_configured() && cms_get_comments_enabled();
}

// ---- Form fields ------------------------------------------------------------------

// A urlencoded field of `body`, decoded into out. 1 if present.
static int form_field(const char *body, int body_len, const char *key, char *out, size_t size) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body, *end = body + body_len;
    while (p && p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp ? amp : end;
        if ((size_t)(stop - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t n = (size_t)(stop - p - klen - 1);
            char *raw = malloc(n + 1);
            char *dec = malloc(n + 1);
            if (raw && dec) {
                memcpy(raw, p + klen + 1, n);
                raw[n] = '\0';
                url_decode(dec, raw);
                snprintf(out, size, "%s", dec);
            }
            free(raw);
            free(dec);
            return raw && dec;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static int valid_utf8(const unsigned char *s) {
    while (*s) {
        int n = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return 0;
        for (s++; n > 0; n--, s++) if ((*s & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

// Name: letters (any language), digits, spaces and . _ - ' ; runs of spaces
// become one. Returns NULL if fine, else the message.
static const char *clean_name(char *name) {
    trim(name);
    char *w = name;
    int space = 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') { space = 1; continue; }
        if (c < 0x80 && !isalnum(c) && !strchr("._-'", c)) return "Your name may only use letters, digits, spaces and . _ - '";
        if (space && w > name) *w++ = ' ';
        space = 0;
        *w++ = (char)c;
    }
    *w = '\0';
    if (!valid_utf8((const unsigned char *)name)) return "Your name has characters we can't read.";
    if (strlen(name) < COMMENT_NAME_MIN) return "Please give a name (at least 2 characters).";
    if (strlen(name) > COMMENT_NAME_MAX) return "Your name is too long.";
    return NULL;
}

// Text: CRLF as LF, other control characters dropped (tabs as spaces).
static const char *clean_text(char *text) {
    char *w = text;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c < 0x20 && c != '\n') continue;
        if (c == 0x7f) continue;
        *w++ = (char)c;
    }
    *w = '\0';
    trim(text);
    if (!valid_utf8((const unsigned char *)text)) return "Your comment has characters we can't read.";
    if (!text[0]) return "Please write a comment.";
    if (strlen(text) > COMMENT_TEXT_MAX) return "Your comment is too long (2000 characters at most).";
    return NULL;
}

// ---- Remember this browser --------------------------------------------------------

static void remember_mac(const unsigned char key[SITE_SECRET_BYTES], const char *id, long long issued, char out[65]) {
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "commenter|%s|%lld", id, issued);
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char *)msg, (unsigned long long)n, key);
    sodium_bin2hex(out, 65, mac, sizeof(mac));
}

// The commenter id of a valid, unexpired cookie into id_out (25); 1 if so.
static int remembered_commenter(const char *cookie_header, char id_out[25]) {
    id_out[0] = '\0';
    const char *p = cookie_header ? strstr(cookie_header, REMEMBER_COOKIE "=") : NULL;
    if (!p) return 0;
    p += strlen(REMEMBER_COOKIE "=");
    char value[160];
    size_t n = strcspn(p, "; ");
    if (n >= sizeof(value)) return 0;
    memcpy(value, p, n);
    value[n] = '\0';

    char *d1 = strchr(value, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || d1 - value != 24 || strlen(d2 + 1) != 64) return 0;
    *d1 = *d2 = '\0';
    long long issued = atoll(d1 + 1);
    if (issued <= 0 || (long long)time(NULL) - issued > (long long)REMEMBER_DAYS * 86400) return 0;

    unsigned char key[SITE_SECRET_BYTES];
    char mac[65];
    if (cms_get_site_secret(key) != 0) return 0;
    remember_mac(key, value, issued, mac);
    sodium_memzero(key, sizeof(key));
    if (sodium_memcmp(mac, d2 + 1, 64) != 0) return 0;
    memcpy(id_out, value, 25);
    return 1;
}

static char *remember_cookie_header(const char *commenter_id) {
    unsigned char key[SITE_SECRET_BYTES];
    if (cms_get_site_secret(key) != 0) return NULL;
    long long issued = (long long)time(NULL);
    char mac[65];
    remember_mac(key, commenter_id, issued, mac);
    sodium_memzero(key, sizeof(key));
    return render_template("Set-Cookie: " REMEMBER_COOKIE "=%s.%lld.%s; Max-Age=%d; Path=/; HttpOnly; SameSite=Lax\r\n",
                           commenter_id, issued, mac, REMEMBER_DAYS * 86400);
}

// ---- Section ------------------------------------------------------------------------

// Escaped text with line breaks as <br>.
static char *text_html(const char *text) {
    char *enc = html_encode_alloc(text);
    if (!enc) return NULL;
    size_t breaks = 0;
    for (const char *p = enc; *p; p++) breaks += *p == '\n';
    char *out = malloc(strlen(enc) + breaks * 4 + 1);
    char *w = out;
    for (const char *p = enc; out && *p; p++) {
        if (*p == '\n') { memcpy(w, "<br>", 4); w += 4; }
        else *w++ = *p;
    }
    if (out) *w = '\0';
    free(enc);
    return out;
}

static char *form_html(int epoch, const char *link, const CommentForm *form) {
    char *tpl = load_template("comments/comment-form_epoch%d.html", epoch);
    unsigned char key[SITE_SECRET_BYTES];
    Captcha cap = { 0 };
    if (!tpl || cms_get_site_secret(key) != 0 || captcha_new(key, &cap) != 0) {
        free(tpl);
        captcha_free(&cap);
        return NULL;
    }
    sodium_memzero(key, sizeof(key));

    char *error = NULL;
    if (form && form->error) {
        char *etpl = load_template("comments/comment-error_epoch%d.html", epoch);
        char *enc = html_encode_alloc(form->error);
        error = (etpl && enc) ? render_template(etpl, enc) : NULL;
        free(etpl);
        free(enc);
    }
    char action[600];
    snprintf(action, sizeof(action), "/blog/%s/comments", link);
    char *action_enc = html_encode_alloc(action);
    char *name = html_encode_alloc(form ? form->name : "");
    char *email = html_encode_alloc(form ? form->email : "");
    char *text = html_encode_alloc(form ? form->text : "");
    char *html = (action_enc && name && email && text)
        ? render_template(tpl, action_enc, error ? error : "", name, email, text, cap.image_data_uri,
                          cap.token, COMMENT_NAME_MAX, COMMENT_TEXT_MAX)
        : NULL;
    free(action_enc); free(name); free(email); free(text); free(error); free(tpl);
    captcha_free(&cap);
    return html;
}

// WML reads a lone '$' as a variable: doubled there, unchanged elsewhere.
static char *wml_safe(char *html, int epoch) {
    if (!html || epoch != EPOCH_WML || !strchr(html, '$')) return html;
    char *out = malloc(strlen(html) * 2 + 1);
    char *w = out;
    for (const char *p = html; out && *p; p++) {
        if (*p == '$') *w++ = '$';
        *w++ = *p;
    }
    if (out) *w = '\0';
    free(html);
    return out;
}

// Epochs -1..2: the approved comments only, read-only - writing one needs
// epoch 3. Nothing at all while there are none.
static char *retro_section(int epoch, const char *link, const char *lang) {
    char entry_id[25];
    bool closed = false;
    if (!cms_comments_blog_entry(link, lang, entry_id, &closed, NULL)) return strdup("");
    CmsComment *list = NULL;
    int count = cms_comments_approved(entry_id, &list);
    if (count <= 0) { cms_comments_free(list, count); return strdup(""); }

    char *section_tpl = load_template("comments/comments_epoch%d.html", epoch);
    char *item_tpl = load_template("comments/comment-item_epoch%d.html", epoch);
    // Epoch 1/2 color their text like the entry's paragraphs (entry_page.c).
    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    char *items = (section_tpl && item_tpl) ? strdup("") : NULL;
    for (int i = 0; items && i < count; i++) {
        char *name = wml_safe(html_encode_alloc(list[i].name), epoch);
        char *text = wml_safe(text_html(list[i].text), epoch);
        char *item = (name && text) ? render_template(item_tpl, name, list[i].date, text, colors.home_content_text) : NULL;
        items = item ? str_append(items, item) : (free(items), NULL);
        free(name); free(text); free(item);
    }
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", count);
    const char *note = closed ? "Comments are closed on this entry."
                              : "To write a comment, open this page in a modern browser.";
    char *result = items ? render_template(section_tpl, count_str, items, note, colors.home_content_text) : NULL;

    cms_comments_free(list, count);
    free(items); free(section_tpl); free(item_tpl);
    return result ? result : strdup("");
}

char *comments_section(int epoch, const char *link, const char *lang, const CommentForm *form,
                       const char *notice) {
    if (epoch != EPOCH_MODERN)
        return mongodb_manager_is_ready() && cms_get_comments_enabled() ? retro_section(epoch, link, lang)
                                                                        : strdup("");
    if (!comments_available()) return strdup("");

    char entry_id[25];
    bool closed = false;
    if (!cms_comments_blog_entry(link, lang, entry_id, &closed, NULL)) return strdup("");

    char *section_tpl = load_template("comments/comments_epoch%d.html", epoch);
    char *item_tpl = load_template("comments/comment-item_epoch%d.html", epoch);
    if (!section_tpl || !item_tpl) { free(section_tpl); free(item_tpl); return strdup(""); }

    CmsComment *list = NULL;
    int count = cms_comments_approved(entry_id, &list);
    char *items = strdup("");
    for (int i = 0; items && i < count; i++) {
        char *name = html_encode_alloc(list[i].name);
        char *text = text_html(list[i].text);
        char *item = (name && text) ? render_template(item_tpl, name, list[i].date, text) : NULL;
        items = item ? str_append(items, item) : (free(items), NULL);
        free(name); free(text); free(item);
    }
    if (items && count <= 0) {
        char *empty = load_template("comments/comments-empty_epoch%d.html", epoch);
        if (empty) { items = str_append(items, empty); free(empty); }
    }

    char *notice_html = strdup("");
    const char *message = NULL;
    if (notice && strcmp(notice, "sent") == 0)
        message = "Thanks! Check your email: we sent you a link to confirm your comment. It works for 24 hours.";
    else if (notice && strcmp(notice, "pending") == 0)
        message = "Thanks! Your comment is waiting for a moderator.";
    if (message) {
        char *ntpl = load_template("comments/comments-notice_epoch%d.html", epoch);
        if (ntpl) { free(notice_html); notice_html = render_template(ntpl, message); free(ntpl); }
    }

    char *bottom = closed ? load_template("comments/comments-closed_epoch%d.html", epoch)
                          : form_html(epoch, link, form);
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", count > 0 ? count : 0);
    char *result = (items && notice_html && bottom)
        ? render_template(section_tpl, count_str, notice_html, items, bottom) : NULL;

    cms_comments_free(list, count);
    free(items); free(notice_html); free(bottom); free(section_tpl); free(item_tpl);
    return result ? result : strdup("");
}

// ---- Submission -----------------------------------------------------------------------

static void ip_hash(const char *ip, const unsigned char key[SITE_SECRET_BYTES], char out[33]) {
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "ip|%s", ip ? ip : "");
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char *)msg, (unsigned long long)n, key);
    sodium_bin2hex(out, 33, mac, 16);
}

static int queue_confirmation(const CommentForm *form, const char *title, const char *base_url, const char *token) {
    char *site = cms_get_site_name();
    char subject[200];
    snprintf(subject, sizeof(subject), "Confirm your comment on %s", site ? site : "the site");
    char *text = render_template(
        "Hello %s,\n\n"
        "You (or someone using this address) left a comment on \"%s\" at %s.\n"
        "To send it to the moderators, open this link and confirm:\n\n"
        "%s/comments/verify?token=%s\n\n"
        "The link works for 24 hours. If you didn't write this comment, ignore this\n"
        "email - it will be deleted.\n",
        form->name, title && title[0] ? title : "a blog entry", site ? site : "the site", base_url, token);
    int rc = text ? mailer_enqueue(form->email, subject, text) : -1;
    free(text);
    free(site);
    return rc;
}

// Tells the entry's author that `comment_id` waits for moderation. Best
// effort: the comment is stored either way.
static void notify_author(const char *comment_id, const char *lang, const char *base_url) {
    CommentNotice n;
    if (cms_comments_notice(comment_id, lang, &n) != 1) return;
    char *site = cms_get_site_name();
    const char *title = n.entry_title[0] ? n.entry_title : "your blog entry";
    // Subjects stop at 200 bytes (mailer_enqueue()): a long title is cut,
    // never inside a UTF-8 character.
    size_t cut = strlen(title);
    if (cut > 150) {
        cut = 150;
        while (cut > 0 && ((unsigned char)title[cut] & 0xC0) == 0x80) cut--;
    }
    char subject[200];
    snprintf(subject, sizeof(subject), "New comment on \"%.*s%s\"", (int)cut, title, title[cut] ? "..." : "");
    char *text = render_template(
        "Hello %s,\n\n"
        "%s left a comment on \"%s\" at %s:\n\n"
        "%s\n\n"
        "It is waiting for moderation - approve or reject it here:\n"
        "%s/dashboard/comments\n\n"
        "The entry: %s/blog/%s\n",
        n.author_name, n.commenter_name, title, site ? site : "the site", n.text,
        base_url, base_url, n.entry_link);
    if (!text || mailer_enqueue(n.author_email, subject, text) != 0)
        LOG_ERROR("Comment %s: the author's notice was not queued", comment_id);
    free(text);
    free(site);
    cms_comments_notice_free(&n);
}

int comments_resend(const char *comment_id, const char *lang, const char *base_url) {
    char token[COMMENT_TOKEN_HEX + 1];
    char *email = NULL, *name = NULL, *title = NULL;
    if (!mailer_configured() || cms_comments_renew_token(comment_id, lang, token, &email, &name, &title) != 0)
        return -1;
    CommentForm form = { 0 };
    snprintf(form.name, sizeof(form.name), "%s", name);
    snprintf(form.email, sizeof(form.email), "%s", email);
    int rc = queue_confirmation(&form, title, base_url, token);
    sodium_memzero(token, sizeof(token));
    free(email); free(name); free(title);
    return rc;
}

CommentPostResult comments_post(const char *link, const char *lang, const char *body, int body_len,
                                const char *cookie_header, const char *client_ip, const char *base_url,
                                CommentForm *form) {
    memset(form, 0, sizeof(*form));
    if (!comments_available()) return COMMENT_POST_CLOSED;

    char entry_id[25];
    bool closed = false;
    char *title = NULL;
    if (!cms_comments_blog_entry(link, lang, entry_id, &closed, &title)) return COMMENT_POST_NOT_FOUND;
    if (closed) { free(title); return COMMENT_POST_CLOSED; }
    if (!body) body = "", body_len = 0;

    char honeypot[64], answer[32], token[CAPTCHA_TOKEN_MAX];
    form_field(body, body_len, "name", form->name, sizeof(form->name));
    form_field(body, body_len, "email", form->email, sizeof(form->email));
    form_field(body, body_len, "text", form->text, sizeof(form->text));
    form_field(body, body_len, "website", honeypot, sizeof(honeypot));
    form_field(body, body_len, "captcha", answer, sizeof(answer));
    form_field(body, body_len, "captcha_token", token, sizeof(token));

    // A filled honeypot is a bot: it gets the usual answer, nothing is stored.
    if (honeypot[0]) {
        LOG_INFO("Comment refused (honeypot) on %s", link);
        free(title);
        return COMMENT_POST_VERIFY;
    }

    trim(form->email);
    for (char *p = form->email; *p; p++) *p = (char)tolower((unsigned char)*p);
    const char *error = clean_name(form->name);
    if (!error && !mailer_valid_address(form->email)) error = "Please give a valid email address.";
    if (!error) error = clean_text(form->text);

    unsigned char key[SITE_SECRET_BYTES];
    if (!error && cms_get_site_secret(key) != 0) error = "Comments are not available right now - try again later.";
    if (!error) {
        char nonce[33];
        switch (captcha_check(key, token, answer, time(NULL), nonce)) {
        case CAPTCHA_OK: {
            int used = cms_comments_use_captcha(nonce, (long long)time(NULL) + CAPTCHA_TTL_SECS + 60);
            if (used == 1) error = "That image was already used - type the characters of the new one.";
            else if (used < 0) error = "Comments are not available right now - try again later.";
            break;
        }
        case CAPTCHA_EXPIRED:  error = "The image expired - type the characters of the new one."; break;
        case CAPTCHA_TOO_FAST: error = "That was a little too fast - please type the characters again."; break;
        default:               error = "The characters didn't match the image - try the new one."; break;
        }
    }

    char hash[33] = "";
    if (!error) {
        ip_hash(client_ip, key, hash);
        if (!cms_comments_within_limits(hash, form->email))
            error = "Too many comments from here for now - please try again later.";
    }
    sodium_memzero(key, sizeof(key));

    CommentPostResult result = COMMENT_POST_FORM;
    if (!error) {
        char remembered[25];
        int has_cookie = remembered_commenter(cookie_header, remembered);
        CommentSubmission s = { entry_id, form->name, form->email, form->text, hash,
                                has_cookie ? remembered : NULL };
        char confirm_token[COMMENT_TOKEN_HEX + 1], commenter_id[25], comment_id[25];
        switch (cms_comments_submit(&s, confirm_token, commenter_id, comment_id)) {
        case COMMENT_SUBMIT_VERIFY:
            if (queue_confirmation(form, title, base_url, confirm_token) == 0) result = COMMENT_POST_VERIFY;
            else error = "We couldn't send the confirmation email - please try again later.";
            sodium_memzero(confirm_token, sizeof(confirm_token));
            break;
        case COMMENT_SUBMIT_PENDING:
            result = COMMENT_POST_PENDING;
            notify_author(comment_id, lang, base_url);
            break;
        case COMMENT_SUBMIT_NAME_TAKEN: error = "That name is already used by someone else - please choose another."; break;
        case COMMENT_SUBMIT_BLOCKED:    error = "Comments from this email address are not accepted."; break;
        default:                        error = "Your comment could not be saved - please try again later."; break;
        }
    }
    free(title);
    form->error = error;
    return error ? COMMENT_POST_FORM : result;
}

// ---- Confirmation pages -----------------------------------------------------------------

static char *verify_fragment(int epoch, const char *heading, const char *message, const char *token) {
    char *tpl = load_template("comments/verify_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *form = strdup("");
    if (token) {
        char *ftpl = load_template("comments/verify-form_epoch%d.html", epoch);
        if (ftpl) { free(form); form = render_template(ftpl, token); free(ftpl); }
    }
    char *html = form ? render_template(tpl, heading, message, form) : NULL;
    free(form);
    free(tpl);
    return html;
}

// Tokens are 64 lower-case hex digits; anything else never reaches a template.
static int token_shape(const char *token) {
    if (!token || strlen(token) != COMMENT_TOKEN_HEX) return 0;
    for (const char *p = token; *p; p++) if (!strchr("0123456789abcdef", *p)) return 0;
    return 1;
}

char *comments_verify_page(int epoch, const char *token) {
    if (!token_shape(token) || !mongodb_manager_is_ready() || !cms_comments_token_valid(token))
        return verify_fragment(epoch, "Link expired",
                               "This link has expired or was already used. Comments must be confirmed within 24 hours.",
                               NULL);
    // A link visit never confirms on its own: mail scanners open links too.
    return verify_fragment(epoch, "Confirm your comment",
                           "Press the button to confirm your email address and send your comment to the moderators.",
                           token);
}

char *comments_verify_post(int epoch, const char *token, const char *lang, const char *base_url,
                           char **set_cookie) {
    *set_cookie = NULL;
    char commenter_id[25], comment_id[25], link[256];
    if (!token_shape(token) || !mongodb_manager_is_ready() ||
        !cms_comments_verify(token, commenter_id, comment_id, link, sizeof(link)))
        return verify_fragment(epoch, "Link expired",
                               "This link has expired or was already used. Comments must be confirmed within 24 hours.",
                               NULL);
    *set_cookie = remember_cookie_header(commenter_id);
    notify_author(comment_id, lang, base_url);
    char message[600];
    char *link_enc = html_encode_alloc(link);
    snprintf(message, sizeof(message),
             "Thanks - your email is confirmed. Your comment will appear once a moderator approves it. "
             "<a href=\"/blog/%s#comments\">Back to the entry</a>", link_enc ? link_enc : "");
    free(link_enc);
    return verify_fragment(epoch, "Comment confirmed", message, NULL);
}
