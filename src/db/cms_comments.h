#ifndef CMS_COMMENTS_H
#define CMS_COMMENTS_H

#include <stdbool.h>
#include <stddef.h>

// Blog comments - see develop_docs/reference/comments.md.
//
//   users           a commenter is a user with role "commenter" (no
//                   password, can't sign in): name, name_key, email,
//                   email_verified_at, blocked. A team member commenting
//                   with their own address is themselves.
//   comments        status "unverified" -> "pending" -> "approved"/"rejected";
//                   at most one `reply` from the team (text, user_id, name,
//                   by_author, created_at, updated_at)
//   comment_tokens  SHA-256 of each email-confirmation token, TTL 24 h
//   captcha_used    nonces of answered challenges, TTL (single use)
//
// An unverified comment carries expires_at and a TTL index deletes it 24 h
// after it was sent; confirming it removes the field.

#define COMMENT_NAME_MIN      2      // bytes, after trimming
#define COMMENT_NAME_MAX      40
#define COMMENT_TEXT_MAX      2000
#define COMMENT_REPLY_MAX     2000
#define COMMENT_UA_MAX        256
#define COMMENT_EPOCH_NONE    (-99)  // CmsCommentAdmin.epoch of a comment older than that record
#define COMMENT_VERIFY_HOURS  24
#define COMMENT_LIMIT_PER_IP_HOUR     5
#define COMMENT_LIMIT_PER_EMAIL_DAY   10
#define COMMENT_ADMIN_LIST_LIMIT      200
#define COMMENT_TOKEN_HEX     64     // email-confirmation token, hex

// Creates the indexes (TTLs included) and moves a legacy `commenters`
// collection into `users`. Called once at startup.
void cms_comments_ensure_indexes(void);

// A published blog entry by link: its id (24 hex), whether comments are
// closed on it (entries.comments_closed) and its title in `lang`
// (malloc'd into *title when non-NULL). Returns 1 if found, 0 otherwise.
int cms_comments_blog_entry(const char *link, const char *lang, char id_hex[25], bool *closed, char **title);

// Marks a challenge nonce used until `expires_at` (unix time). Returns 0 the
// first time, 1 if it was already used, -1 on a DB error.
int cms_comments_use_captcha(const char *nonce, long long expires_at);

// 1 if neither the address (ip_hash) nor the email has gone over its limit.
int cms_comments_within_limits(const char *ip_hash, const char *email);

typedef enum {
    COMMENT_SUBMIT_VERIFY = 0,   // stored unverified; *token_out must be emailed
    COMMENT_SUBMIT_PENDING,      // stored pending (browser remembered, email already confirmed)
    COMMENT_SUBMIT_NAME_TAKEN,   // the name belongs to another email, or to a site user
    COMMENT_SUBMIT_BLOCKED,      // this email is blocked
    COMMENT_SUBMIT_ERROR,
} CommentSubmitResult;

typedef struct {
    const char *entry_id;        // 24 hex
    const char *name;            // validated
    const char *email;           // validated, lower-case
    const char *text;            // validated
    const char *ip_hash;
    const char *remembered_id;   // commenter id from a valid remember-me cookie, or NULL
    // Where it was written from, for showing it later (an icon): ua_parser.h's
    // "Browser/version" and "OS/version", the User-Agent itself (printable
    // ASCII, at most COMMENT_UA_MAX bytes, so it can be classified again),
    // and the epoch (-1-3).
    const char *browser;
    const char *os;
    const char *user_agent;
    int         epoch;
} CommentSubmission;

// Stores the comment (and the commenter, new or updated). On
// COMMENT_SUBMIT_VERIFY, token_out gets the confirmation token
// (COMMENT_TOKEN_HEX hex + NUL). commenter_id_out and comment_id_out (25) are
// filled when stored.
CommentSubmitResult cms_comments_submit(const CommentSubmission *s, char token_out[COMMENT_TOKEN_HEX + 1],
                                        char commenter_id_out[25], char comment_id_out[25]);

// 1 if `token` is a live confirmation token (for the confirm page).
int cms_comments_token_valid(const char *token);

// Confirms the comment behind `token`: comment -> pending, commenter's email
// confirmed, token spent. Fills commenter_id_out, comment_id_out (25) and
// entry_link_out. Returns 1, or 0 if the token is unknown/expired.
int cms_comments_verify(const char *token, char commenter_id_out[25], char comment_id_out[25],
                        char *entry_link_out, size_t link_size);

// What the author of an entry is told when a comment on it waits for
// moderation. Every string malloc'd (cms_comments_notice_free()).
typedef struct {
    char *author_email;
    char *author_name;           // name, else username
    char *commenter_name;
    char *text;
    char *entry_title;           // in `lang`
    char *entry_link;
} CommentNotice;

// Fills *out for `comment_id` (24 hex). Returns 1 when the entry's author
// (header.author_id, else entries.created_by) should be told: a team member with an email, other
// than the commenter. 0 when no one is to be told, -1 on a bad id or DB error.
int cms_comments_notice(const char *comment_id, const char *lang, CommentNotice *out);
void cms_comments_notice_free(CommentNotice *n);

// 1 if `commenter_id` (24 hex) is a commenter whose email was confirmed and
// who isn't blocked.
int cms_comments_commenter_verified(const char *commenter_id);

typedef struct {
    char  id[25];
    char *name;
    char *text;
    char  date[17];              // "YYYY-MM-DD HH:MM" (UTC)
    char *browser;               // where it was written from ("" before that was recorded)
    char *os;
    char *user_agent;
    // The team's reply: reply_text is "" when there is none.
    char *reply_name;
    char *reply_text;
    char  reply_date[17];
    bool  reply_by_author;       // the entry's author, else an Administrador
} CmsComment;

// The approved comments of an entry, oldest first (at most 500). Returns the
// count (*out malloc'd, cms_comments_free()), or -1 on a DB error.
int cms_comments_approved(const char *entry_id, CmsComment **out);
void cms_comments_free(CmsComment *list, int count);

typedef struct {
    char  id[25];
    char  commenter_id[25];
    char *entry_link;
    char *entry_title;
    char *name;
    char *email;
    char *text;
    char  status[12];
    char  date[17];
    bool  commenter_blocked;
    // Unverified comments only: the confirmation email's state - "sent",
    // "queued" (waiting or retrying), "failed", or "" when none is found -
    // and its last error ("" if none).
    char  mail_status[12];
    char *mail_error;
    // Where it was written from ("" / COMMENT_EPOCH_NONE for comments older than this).
    char *browser;
    char *os;
    char *user_agent;
    int   epoch;
    // The team's reply ("" when none).
    char *reply_name;
    char *reply_text;
    char  reply_date[17];
} CmsCommentAdmin;

// Moderation list: comments with `status` ("unverified", "pending",
// "approved", "rejected"), newest first; only on `owner_id`'s blog entries when it is
// non-NULL (an author). Titles in `lang`. Returns the count or -1.
int cms_comments_admin_list(const char *status, const char *owner_id, const char *lang, CmsCommentAdmin **out);
void cms_comments_admin_free(CmsCommentAdmin *list, int count);

// One comment as the moderation list shows it, whatever its status (titles
// in `lang`). Returns a malloc'd record (cms_comments_admin_free(c, 1)), or
// NULL when there is none.
CmsCommentAdmin *cms_comments_admin_one(const char *comment_id, const char *lang);

// Pending comments (on owner_id's entries when non-NULL). -1 on a DB error.
long cms_comments_pending_count(const char *owner_id);

// Comments with `status` (same scope rule). -1 on a DB error.
long cms_comments_count(const char *status, const char *owner_id);

// 1 if the comment exists and, when owner_id is non-NULL, is on one of
// owner_id's blog entries.
int cms_comments_can_moderate(const char *comment_id, const char *owner_id);

// status "approved" / "rejected", from any status - an unverified comment
// too: a moderator's approval stands in for the email check, and the
// comment no longer expires. Returns 0, -1 on a bad id/status or DB error.
int cms_comments_set_status(const char *comment_id, const char *status, const char *moderator_id);
int cms_comments_delete(const char *comment_id);

// For "Resend email" on an unverified comment: a fresh confirmation token
// (replacing the old ones) and 24 more hours before it expires. Fills
// token_out, and the commenter's email/name and the entry's link and title
// (in `lang`) as malloc'd strings. Returns 0, -1 if the comment isn't
// unverified any more, or on a DB error.
int cms_comments_renew_token(const char *comment_id, const char *lang, char token_out[COMMENT_TOKEN_HEX + 1],
                             char **email, char **name, char **title_out);

// The reply to a comment - one per comment: `text` (validated) becomes
// user_id's reply, replacing an earlier one. Only a pending or approved
// comment can be replied to; a pending one is approved by it. *first_out is
// 1 when the comment had no reply before, *approved_out 1 when it was
// pending. Returns 0, or -1 (bad id, another status, DB error).
int cms_comments_set_reply(const char *comment_id, const char *user_id, const char *text,
                           int *first_out, int *approved_out);
// Removes the reply. Returns 0 or -1.
int cms_comments_delete_reply(const char *comment_id);

// What a commenter is told when the team replies. Strings malloc'd
// (cms_comments_reply_notice_free()).
typedef struct {
    char *commenter_email;
    char *commenter_name;
    char *comment_text;
    char *replier_name;
    char *reply_text;
    char *entry_title;           // in `lang`
    char *entry_link;
} ReplyNotice;

// Fills *out for `comment_id`. Returns 1 when the commenter should be told:
// their email confirmed, not blocked, and not the one who replied. 0 when no
// one is to be told, -1 on a bad id or DB error.
int cms_comments_reply_notice(const char *comment_id, const char *lang, ReplyNotice *out);
void cms_comments_reply_notice_free(ReplyNotice *n);

// Blocks (or unblocks) a commenter; with delete_all, also deletes every
// comment they wrote. Returns 0 or -1.
int cms_comments_block(const char *commenter_id, bool block, bool delete_all);

#endif // CMS_COMMENTS_H
