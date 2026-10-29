#ifndef COMMENTS_H
#define COMMENTS_H

#include "../../db/cms_comments.h"

// Blog comments, visitor side (develop_docs/reference/comments.md): the
// comments section under a blog entry, the form's submission and the
// email-confirmation pages. Epoch 3 only.

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
// "" when comments aren't available. Epochs -1..2 get the approved comments
// only, read-only (comments/comments_epoch<N>.html), and nothing when there
// are none. Returns malloc'd HTML.
char *comments_section(int epoch, const char *link, const char *lang, const CommentForm *form,
                       const char *notice);

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
// `base_url` ("https://host") builds the confirmation link. Fills *form with
// what was sent (for COMMENT_POST_FORM).
CommentPostResult comments_post(const char *link, const char *lang, const char *body, int body_len,
                                const char *cookie_header, const char *client_ip, const char *base_url,
                                CommentForm *form);

// "Resend email" on an unverified comment (dashboard): a new confirmation
// link replaces the old one, queued to the commenter, and the comment gets
// 24 more hours. Returns 0, -1 if it isn't unverified any more or on failure.
int comments_resend(const char *comment_id, const char *lang, const char *base_url);

// GET /comments/verify?token=: the "Confirm your comment" page (or "link
// expired"). Returns a malloc'd fragment.
char *comments_verify_page(int epoch, const char *token);

// POST /comments/verify: confirms, and tells the entry's author (title in
// `lang`, links from `base_url`). Returns a malloc'd fragment; on success
// *set_cookie gets a malloc'd "Set-Cookie: ...\r\n" header line (remember
// this browser), else NULL.
char *comments_verify_post(int epoch, const char *token, const char *lang, const char *base_url,
                           char **set_cookie);

#endif // COMMENTS_H
