#ifndef COMMENTS_H
#define COMMENTS_H

#include "../../db/cms_comments.h"

// Blog comments, visitor side (develop_docs/reference/comments.md): the
// comments section under a blog entry, the form's submission and the
// email-confirmation pages. Writing from every epoch; WAP's form is a page of
// its own (comments_wap_form_page()).

// The WAP form's comment field: a phone's keypad, and a post that must fit
// one WAP gateway packet (wap_gateway.h). The server accepts up to
// COMMENT_TEXT_MAX from any form.
#define COMMENT_WAP_TEXT_MAX 300

// What the form held when it comes back with a message.
typedef struct {
    char        name[COMMENT_NAME_MAX * 2 + 1];
    char        email[256];
    char        text[COMMENT_TEXT_MAX + 1];
    const char *error;
} CommentForm;

// 1 when comments can be used at all: turned on in site settings and mail
// configured.
int comments_available(void);

// The section under a blog entry: approved comments, then the form (or "closed").
// `form` is NULL for an empty one; `notice` is "sent", "pending" or NULL.
// "" when comments aren't available. WAP (epoch -1) gets the approved
// comments and a "Write a comment" link to comments_wap_form_page() - the
// form itself isn't there - and nothing while there is neither a comment
// nor a way to write one. Returns malloc'd HTML.
char *comments_section(int epoch, const char *link, const char *lang, const CommentForm *form,
                       const char *notice);

// GET /blog/<link>/comment on WAP: the form, on pages of its own (WML) in
// two steps small enough for one gateway packet each - step 1 name, email
// and comment; step 2 (?step=2) the challenge as a WBMP
// (/comments/captcha?format=wbmp) and Send. The phone keeps what was typed
// in WML variables between the two. With `error` (a refused comment; NULL
// for none), a page of its own with the message, "Try again" (a new step 2)
// and "< Edit" (step 1). NULL when the entry takes no comments (none such,
// closed, comments off).
char *comments_wap_form_page(const char *link, const char *lang, int step, const char *error);

// GET /blog/<link>/comment?comment=sent|pending on WAP, where a sent form
// leads: the thanks and a link back to the entry - a short page, as the
// entry's own comments section is many packets away. NULL for any other
// `notice`.
char *comments_wap_sent_page(const char *link, const char *notice);

typedef enum {
    COMMENT_POST_VERIFY = 0,   // stored; confirmation email queued
    COMMENT_POST_PENDING,      // stored straight for moderation (remembered browser)
    COMMENT_POST_FORM,         // show the form again with form->error
    COMMENT_POST_NOT_FOUND,    // no such published blog entry
    COMMENT_POST_CLOSED,       // comments off, or closed on this entry
} CommentPostResult;

// Handles POST /blog/<link>/comments: every check (honeypot, challenge,
// fields, limits, name ownership, block), then storage and the email - to
// the commenter to confirm it, or, once it waits for moderation, to the
// entry's author.
// `base_url` ("https://host") builds the confirmation link; `user_agent` and
// `epoch` are stored with the comment (browser, OS - see CommentSubmission).
// Fills *form with what was sent (for COMMENT_POST_FORM).
CommentPostResult comments_post(const char *link, const char *lang, const char *body, int body_len,
                                const char *cookie_header, const char *client_ip, const char *base_url,
                                const char *user_agent, int epoch, CommentForm *form);

// "Resend email" on an unverified comment (dashboard): a new confirmation
// link replaces the old one, queued to the commenter, and the comment gets
// 24 more hours. Returns 0, -1 if it isn't unverified any more or on failure.
int comments_resend(const char *comment_id, const char *lang, const char *base_url);

// GET /comments/verify?token=: the "Confirm your comment" page (or "link
// expired"). Returns a malloc'd fragment.
char *comments_verify_page(int epoch, const char *token);

// Where a comment was written from, as `epoch` shows it next to the name:
// the browser's and the system's icons (client_icon.h) with their names as
// alt text on epochs 1-3, the names as text on 0 and WAP. Arguments as the
// comment stored them; "" when nothing was recorded. Returns malloc'd HTML.
char *comment_client_html(int epoch, const char *browser, const char *os, const char *user_agent);

// Who wrote a comment or reply, as `epoch` shows it: anyone may use any
// name, so a commenter's comes with their username - "Ana (user-12)"
// (comment-user_epoch*.html) - and the team's with a verified mark instead
// (comment-verified_epoch*.html: an icon on epochs 1-3, "[v]" on epoch 0 and
// WAP - plain ASCII: Lynx on DOS shows a "√" as "SQRT"); `username` NULL or
// "" and not verified: the name alone. Escaped. Returns malloc'd HTML.
char *comment_who_html(int epoch, const char *name, const char *username, int verified);

typedef enum {
    COMMENT_REPLY_SAVED = 0,
    COMMENT_REPLY_APPROVED,      // saved, and the pending comment approved by it
    COMMENT_REPLY_INVALID,       // *error says why
    COMMENT_REPLY_ERROR,         // the comment is gone, rejected/unverified, or a DB error
} CommentReplyResult;

// POST /dashboard/comments/<id>/reply: `text` becomes user_id's reply to the
// comment (the caller checked they may write it) - cleaned in place like a
// comment's text. At the first reply the commenter is told by email (title
// in `lang`, the link from `base_url`).
CommentReplyResult comments_reply(const char *comment_id, const char *user_id, char *text, size_t text_size,
                                  const char *lang, const char *base_url, const char **error);

// POST /comments/verify: confirms, and tells the entry's author (title in
// `lang`, links from `base_url`). Returns a malloc'd fragment; on success
// *set_cookie gets a malloc'd "Set-Cookie: ...\r\n" header line (remember
// this browser), else NULL.
char *comments_verify_post(int epoch, const char *token, const char *lang, const char *base_url,
                           char **set_cookie);

#endif // COMMENTS_H
