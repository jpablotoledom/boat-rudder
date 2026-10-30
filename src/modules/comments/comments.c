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
#include "../../utils/client_icon.h"
#include "../../utils/ua_parser.h"
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

// Windows-1252's 0x80-0x9F (what browsers really send for "ISO-8859-1"):
// curly quotes, dashes, the euro sign... 0 where it has nothing.
static const unsigned short CP1252_HIGH[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

// Epoch 1 pages go out in Latin-1 (build_epoch_response.c), so their forms
// come back in it - as do some epoch 2 browsers'. A field that isn't UTF-8
// is read as Windows-1252 and converted in place. Returns 0, or -1 if the
// converted text doesn't fit in `size` (the field is then too long anyway).
static int to_utf8(char *s, size_t size) {
    if (valid_utf8((const unsigned char *)s)) return 0;
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out) return -1;
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned cp = *p < 0x80 ? *p : *p < 0xA0 ? CP1252_HIGH[*p - 0x80] : *p;
        if (!cp) continue;
        if (cp < 0x80) *w++ = (char)cp;
        else if (cp < 0x800) { *w++ = (char)(0xC0 | cp >> 6); *w++ = (char)(0x80 | (cp & 0x3F)); }
        else { *w++ = (char)(0xE0 | cp >> 12); *w++ = (char)(0x80 | (cp >> 6 & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
    }
    *w = '\0';
    int fits = (size_t)(w - out) < size;
    if (fits) memcpy(s, out, (size_t)(w - out) + 1);
    free(out);
    return fits ? 0 : -1;
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

// The form. Epoch 3 gets the challenge inline (a data: PNG); epochs 1-2,
// which know neither data: URIs nor (all of them) PNG, a GIF by URL that
// GET /comments/captcha draws from the token; epoch 0's text browsers the
// same characters as text art for a <pre>. Below epoch 3, no honeypot:
// without CSS a person would see it and fill it.
static char *form_html(int epoch, const char *link, const CommentForm *form, const char *color) {
    char *tpl = load_template("comments/comment-form_epoch%d.html", epoch);
    unsigned char key[SITE_SECRET_BYTES];
    Captcha cap = { 0 };
    int inline_png = epoch == EPOCH_MODERN;
    if (!tpl || cms_get_site_secret(key) != 0 || captcha_new(key, inline_png, &cap) != 0) {
        free(tpl);
        captcha_free(&cap);
        return NULL;
    }
    sodium_memzero(key, sizeof(key));

    char *error = NULL;
    if (form && form->error) {
        char *etpl = load_template("comments/comment-error_epoch%d.html", epoch);
        char *enc = html_encode_alloc(form->error);
        error = (etpl && enc) ? render_template(etpl, enc, color) : NULL;
        free(etpl);
        free(enc);
    }
    char action[600];
    snprintf(action, sizeof(action), "/blog/%s/comments", link);
    char *action_enc = html_encode_alloc(action);
    char *name = html_encode_alloc(form ? form->name : "");
    char *email = html_encode_alloc(form ? form->email : "");
    char *text = html_encode_alloc(form ? form->text : "");
    char image_url[CAPTCHA_TOKEN_MAX + 32];
    snprintf(image_url, sizeof(image_url), "/comments/captcha?t=%s", cap.token);   // hex and '.' only
    char *art = NULL;
    if (epoch == EPOCH_PRESTANDARD && cms_get_site_secret(key) == 0) {
        art = captcha_token_text(key, cap.token, time(NULL));   // needs no escaping
        sodium_memzero(key, sizeof(key));
    }
    const char *challenge = inline_png ? cap.image_data_uri : epoch == EPOCH_PRESTANDARD ? art : image_url;
    char *html = (action_enc && name && email && text && challenge)
        ? render_template(tpl, action_enc, error ? error : "", name, email, text, challenge,
                          cap.token, COMMENT_NAME_MAX, COMMENT_TEXT_MAX, color)
        : NULL;
    free(action_enc); free(name); free(email); free(text); free(error); free(tpl); free(art);
    captcha_free(&cap);
    return html;
}

char *comment_client_html(int epoch, const char *browser, const char *os, const char *user_agent) {
    ClientIcons c;
    char *tpl = client_icons(browser, os, user_agent, &c)
        ? load_template("comments/comment-client_epoch%d.html", epoch) : NULL;
    if (!tpl) return strdup("");
    char src[96];
    snprintf(src, sizeof(src), CLIENT_ICON_DIR "%s.gif", c.browser_icon);
    char *out = render_template(tpl, c.browser_label, src);
    if (out && c.os_icon) {
        snprintf(src, sizeof(src), CLIENT_ICON_DIR "%s.gif", c.os_icon);
        char *os_html = render_template(tpl, c.os_label, src);
        out = os_html ? str_append(str_append(out, " "), os_html) : out;
        free(os_html);
    }
    free(tpl);
    return out ? out : strdup("");
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

char *comment_who_html(int epoch, const char *name, const char *username, int verified) {
    char *enc = wml_safe(html_encode_alloc(name ? name : ""), epoch);
    if (!enc) return NULL;
    char *tpl = NULL;
    if (verified) {
        tpl = load_template("comments/comment-verified_epoch%d.html", epoch);
    } else if (username && username[0]) {
        tpl = load_template("comments/comment-user_epoch%d.html", epoch);
    }
    if (!tpl) return enc;
    char *user = html_encode_alloc(username ? username : "");
    char *out = user ? render_template(tpl, enc, user) : NULL;
    free(tpl); free(user); free(enc);
    return out;
}

// The team's reply under a comment ("" when it has none), marked as the
// entry's author's or an Administrador's.
static char *reply_html(int epoch, const CmsComment *c, const char *color) {
    if (!c->reply_text || !c->reply_text[0]) return strdup("");
    char *tpl = load_template("comments/comment-reply_epoch%d.html", epoch);
    char *name = comment_who_html(epoch, c->reply_name, NULL, 1);   // the team's: verified
    char *text = wml_safe(text_html(c->reply_text), epoch);
    char *out = (tpl && name && text)
        ? render_template(tpl, name, c->reply_date, text, c->reply_by_author ? "Author" : "Administrator", color) : NULL;
    free(tpl); free(name); free(text);
    return out;
}

// The thanks after sending (?comment=sent|pending), NULL for anything else.
static const char *notice_message(const char *notice) {
    if (notice && strcmp(notice, "sent") == 0)
        return "Thanks! Check your email: we sent you a link to confirm your comment. It works for 24 hours.";
    if (notice && strcmp(notice, "pending") == 0)
        return "Thanks! Your comment is waiting for a moderator.";
    return NULL;
}

// WAP (epoch -1): the approved comments, then a link to the form - a page of
// its own (comments_wap_form_page()), as the deck is cut into packets the
// form must not be split across. Nothing at all while there is neither a
// comment nor a way to write one.
static char *retro_section(int epoch, const char *link, const char *lang, const char *notice) {
    char entry_id[25];
    bool closed = false;
    if (!cms_comments_blog_entry(link, lang, entry_id, &closed, NULL)) return strdup("");
    int writable = !closed && comments_available();
    CmsComment *list = NULL;
    int count = cms_comments_approved(entry_id, &list);
    const char *message = notice_message(notice);
    if (count <= 0 && !writable && !message) { cms_comments_free(list, count); return strdup(""); }

    char *section_tpl = load_template("comments/comments_epoch%d.html", epoch);
    char *item_tpl = load_template("comments/comment-item_epoch%d.html", epoch);
    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    char *items = (section_tpl && item_tpl) ? strdup("") : NULL;
    for (int i = 0; items && i < count; i++) {
        char *name = comment_who_html(epoch, list[i].name, list[i].username, list[i].verified);
        char *text = wml_safe(text_html(list[i].text), epoch);
        char *client = comment_client_html(epoch, list[i].browser, list[i].os, list[i].user_agent);
        char *reply = reply_html(epoch, &list[i], colors.home_content_text);
        char *item = (name && text && client && reply)
            ? render_template(item_tpl, name, list[i].date, text, client, reply, colors.home_content_text) : NULL;
        items = item ? str_append(items, item) : (free(items), NULL);
        free(name); free(text); free(client); free(reply); free(item);
    }
    if (items && count <= 0) {
        char *empty = load_template("comments/comments-empty_epoch%d.html", epoch);
        if (empty) items = str_append(items, empty);
        free(empty);
    }
    char *notice_html = NULL;
    if (message) {
        char *ntpl = load_template("comments/comments-notice_epoch%d.html", epoch);
        notice_html = ntpl ? render_template(ntpl, message, colors.home_content_text) : NULL;
        free(ntpl);
    }
    char *note = NULL;
    if (closed) note = strdup("Comments are closed on this entry.");
    else if (writable) {
        char *enc = html_encode_alloc(link);
        note = enc ? render_template("<a href=\"/blog/%s/comment\">Write a comment</a>", enc) : NULL;
        free(enc);
    } else note = strdup("");
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", count > 0 ? count : 0);
    char *result = items && note
        ? render_template(section_tpl, count_str, notice_html ? notice_html : "", items, note) : NULL;

    cms_comments_free(list, count);
    free(items); free(notice_html); free(note); free(section_tpl); free(item_tpl);
    return result ? result : strdup("");
}

char *comments_wap_sent_page(const char *link, const char *notice) {
    const char *message = notice_message(notice);
    char *tpl = message ? load_template("comments/comment-sent_epoch%d.html", EPOCH_WML) : NULL;
    char back[600];
    snprintf(back, sizeof(back), "/blog/%s", link);
    char *back_enc = html_encode_alloc(back);
    char *html = tpl && back_enc ? render_template(tpl, message, back_enc) : NULL;
    free(tpl);
    free(back_enc);
    return html;
}

char *comments_wap_form_page(const char *link, const char *lang, int step, const char *error) {
    char entry_id[25];
    bool closed = false;
    if (!comments_available() || !cms_comments_blog_entry(link, lang, entry_id, &closed, NULL) || closed)
        return NULL;
    char here[600];
    snprintf(here, sizeof(here), "/blog/%s/comment", link);
    char *here_enc = html_encode_alloc(here);
    if (!here_enc) return NULL;

    char step2[700];
    snprintf(step2, sizeof(step2), "%s?step=2", here_enc);

    // A refused comment: why, and the way back - a page of its own, as the
    // message and the challenge together don't fit one packet.
    if (error) {
        char *tpl = load_template("comments/comment-error_epoch%d.html", EPOCH_WML);
        char *enc = html_encode_alloc(error);
        char *html = tpl && enc ? render_template(tpl, enc, here_enc, step2) : NULL;
        free(tpl); free(enc); free(here_enc);
        return html;
    }

    // Step 1: what is said. The fields are WML variables, which the phone
    // keeps from deck to deck - so they need no value here, and are still
    // filled in when the visitor comes back to them.
    if (step != 2) {
        char *tpl = load_template("comments/comment-form_epoch%d.html", EPOCH_WML);
        char *html = tpl ? render_template(tpl, COMMENT_NAME_MAX, COMMENT_WAP_TEXT_MAX, step2) : NULL;
        free(tpl);
        free(here_enc);
        return html;
    }

    // Step 2: the challenge and Send, which posts the variables of step 1.
    char *tpl = load_template("comments/comment-code_epoch%d.html", EPOCH_WML);
    unsigned char key[SITE_SECRET_BYTES];
    Captcha cap = { 0 };
    if (!tpl || cms_get_site_secret(key) != 0 || captcha_new(key, 0, &cap) != 0) {
        free(tpl);
        free(here_enc);
        captcha_free(&cap);
        return NULL;
    }
    sodium_memzero(key, sizeof(key));

    char action[600], image[CAPTCHA_TOKEN_MAX + 64], var[9];
    snprintf(action, sizeof(action), "/blog/%s/comments", link);
    snprintf(image, sizeof(image), "/comments/captcha?t=%s&format=wbmp", cap.token);
    // The answer's variable is the token's own: a phone keeps WML variables
    // between decks, and an old answer must not fill in a new challenge.
    snprintf(var, sizeof(var), "%.8s", cap.token);
    char *action_enc = html_encode_alloc(action);
    char *image_enc = html_encode_alloc(image);
    char *html = (action_enc && image_enc)
        ? render_template(tpl, image_enc, cap.token, action_enc, var)
        : NULL;
    free(action_enc); free(image_enc); free(here_enc); free(tpl);
    captcha_free(&cap);
    return html;
}

char *comments_section(int epoch, const char *link, const char *lang, const CommentForm *form,
                       const char *notice) {
    if (epoch < EPOCH_PRESTANDARD)
        return mongodb_manager_is_ready() && cms_get_comments_enabled() ? retro_section(epoch, link, lang, notice)
                                                                        : strdup("");
    if (!comments_available()) return strdup("");
    // Epochs 1-2 color their text like the entry's paragraphs (entry_page.c);
    // epochs 0 and 3's templates ignore it.
    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);
    const char *color = colors.home_content_text;

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
        char *name = comment_who_html(epoch, list[i].name, list[i].username, list[i].verified);
        char *text = text_html(list[i].text);
        char *client = comment_client_html(epoch, list[i].browser, list[i].os, list[i].user_agent);
        char *reply = reply_html(epoch, &list[i], color);
        char *item = (name && text && client && reply)
            ? render_template(item_tpl, name, list[i].date, text, client, reply, color) : NULL;
        items = item ? str_append(items, item) : (free(items), NULL);
        free(name); free(text); free(client); free(reply); free(item);
    }
    if (items && count <= 0) {
        char *etpl = load_template("comments/comments-empty_epoch%d.html", epoch);
        char *empty = etpl ? render_template(etpl, color) : NULL;
        if (empty) items = str_append(items, empty);
        free(etpl); free(empty);
    }

    char *notice_html = strdup("");
    const char *message = notice_message(notice);
    if (message) {
        char *ntpl = load_template("comments/comments-notice_epoch%d.html", epoch);
        if (ntpl) { free(notice_html); notice_html = render_template(ntpl, message, color); free(ntpl); }
    }

    char *bottom = NULL;
    if (closed) {
        char *ctpl = load_template("comments/comments-closed_epoch%d.html", epoch);
        bottom = ctpl ? render_template(ctpl, color) : NULL;
        free(ctpl);
    } else {
        bottom = form_html(epoch, link, form, color);
    }
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", count > 0 ? count : 0);
    char *result = (items && notice_html && bottom)
        ? render_template(section_tpl, count_str, notice_html, items, bottom, color) : NULL;

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
    // "Ana (user-12)": names are anyone's, the username says which Ana.
    char who[160];
    if (n.commenter_username[0]) snprintf(who, sizeof(who), "%s (%s)", n.commenter_name, n.commenter_username);
    else snprintf(who, sizeof(who), "%s", n.commenter_name);
    char *text = render_template(
        "Hello %s,\n\n"
        "%s left a comment on \"%s\" at %s:\n\n"
        "%s\n\n"
        "It is waiting for moderation - approve or reject it here:\n"
        "%s/dashboard/comments\n\n"
        "The entry: %s/blog/%s\n",
        n.author_name, who, title, site ? site : "the site", n.text,
        base_url, base_url, n.entry_link);
    if (!text || mailer_enqueue(n.author_email, subject, text) != 0)
        LOG_ERROR("Comment %s: the author's notice was not queued", comment_id);
    free(text);
    free(site);
    cms_comments_notice_free(&n);
}

// Tells the commenter that the team replied to `comment_id`. Best effort:
// the reply is saved either way.
static void notify_commenter(const char *comment_id, const char *lang, const char *base_url) {
    ReplyNotice n;
    if (cms_comments_reply_notice(comment_id, lang, &n) != 1) return;
    char *site = cms_get_site_name();
    const char *title = n.entry_title[0] ? n.entry_title : "a blog entry";
    size_t cut = strlen(title);   // as in notify_author()
    if (cut > 150) {
        cut = 150;
        while (cut > 0 && ((unsigned char)title[cut] & 0xC0) == 0x80) cut--;
    }
    char subject[200];
    snprintf(subject, sizeof(subject), "Reply to your comment on \"%.*s%s\"", (int)cut, title, title[cut] ? "..." : "");
    char *text = render_template(
        "Hello %s,\n\n"
        "%s replied to your comment on \"%s\" at %s:\n\n"
        "%s\n\n"
        "Your comment was:\n\n"
        "%s\n\n"
        "Read it on the site: %s/blog/%s#comments\n",
        n.commenter_name, n.replier_name, title, site ? site : "the site", n.reply_text,
        n.comment_text, base_url, n.entry_link);
    if (!text || mailer_enqueue(n.commenter_email, subject, text) != 0)
        LOG_ERROR("Comment %s: the commenter's reply notice was not queued", comment_id);
    free(text);
    free(site);
    cms_comments_reply_notice_free(&n);
}

CommentReplyResult comments_reply(const char *comment_id, const char *user_id, char *text, size_t text_size,
                                  const char *lang, const char *base_url, const char **error) {
    *error = NULL;
    if (to_utf8(text, text_size) != 0) { *error = "The reply is too long (2000 characters at most)."; return COMMENT_REPLY_INVALID; }
    const char *bad = clean_text(text);
    if (bad) {
        *error = !text[0] ? "Please write a reply."
               : strlen(text) > COMMENT_REPLY_MAX ? "The reply is too long (2000 characters at most)."
               : "The reply has characters that can't be read.";
        return COMMENT_REPLY_INVALID;
    }
    int first = 0, approved = 0;
    if (cms_comments_set_reply(comment_id, user_id, text, &first, &approved) != 0) return COMMENT_REPLY_ERROR;
    if (first && mailer_configured()) notify_commenter(comment_id, lang, base_url);
    return approved ? COMMENT_REPLY_APPROVED : COMMENT_REPLY_SAVED;
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
                                const char *user_agent, int epoch, CommentForm *form) {
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
    trim(token);   // hex and dots only: whitespace around it is never part of it

    // A filled honeypot is a bot: it gets the usual answer, nothing is stored.
    if (honeypot[0]) {
        LOG_INFO("Comment refused (honeypot) on %s", link);
        free(title);
        return COMMENT_POST_VERIFY;
    }

    const char *error = NULL;
    if (to_utf8(form->name, sizeof(form->name)) != 0) error = "Your name is too long.";
    if (!error && to_utf8(form->text, sizeof(form->text)) != 0)
        error = "Your comment is too long (2000 characters at most).";
    to_utf8(form->email, sizeof(form->email));   // shape-checked below anyway

    trim(form->email);
    for (char *p = form->email; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!error) error = clean_name(form->name);
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
        // Where it was written from: classified as analytics does, and the
        // User-Agent itself kept (printable ASCII only: it must be valid
        // UTF-8 for the database) so it can be classified again later.
        char browser[64], os[64], ua[COMMENT_UA_MAX + 1];
        ua_parse_browser(user_agent, browser, sizeof(browser));
        ua_parse_os(user_agent, os, sizeof(os));
        size_t k = 0;
        for (const char *p = user_agent ? user_agent : ""; *p && k < COMMENT_UA_MAX; p++)
            ua[k++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?';
        ua[k] = '\0';
        CommentSubmission s = { entry_id, form->name, form->email, form->text, hash,
                                has_cookie ? remembered : NULL, browser, os, ua, epoch };
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
