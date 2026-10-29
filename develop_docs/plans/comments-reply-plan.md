# Plan: the author's reply to a comment

> **Status**: implemented (2026-10-29), with the proposed answers to §5: the Administrador
> replies too, a reply approves a pending comment, the commenter is emailed at the first reply.
> Reference: [comments.md](../reference/comments.md#6-moderation-dashboardcomments-srcmodulescomments_admin).

---

## 1. What is asked

The author of a blog entry can answer a comment on it. To keep it simple:

- **One reply per comment** - no threads, no reply to a reply.
- **Visitors don't reply**: they can only comment, as today.

## 2. Design

### 2.1 Who replies

| Role | Replies to |
|---|---|
| Autor | Comments on **their own** blog entries (the same scope as moderation, `entries.created_by`) |
| Administrador | Any comment, under their own name |
| Moderador, Observador | No one - they moderate or read; the voice is the author's |

### 2.2 Storage: inside the comment

No new collection. The comment document gets an optional sub-document:

```
comments.reply = {
  text:       "...",            // plain text, 1-2000 bytes, like a comment
  user_id:    ObjectId,         // who wrote it
  name:       "Pablo Toledo",   // their name then (else username), shown publicly
  created_at: Date,
  updated_at: Date              // only after an edit
}
```

One reply per comment is the shape itself: writing again **replaces** it (an edit), and
*Delete reply* `$unset`s it. Deleting the comment deletes its reply with it.

### 2.3 Dashboard (`/dashboard/comments`)

- A **Reply** button on approved and pending comments (*Edit reply* when there is one). A
  rejected or unverified comment can't be replied to.
- `GET /dashboard/comments/<id>/reply` - its own page: the entry, the comment (name, date,
  icons, text) and a `<textarea>` with the current reply, **Save reply**, and **Delete reply**
  when there is one. A page rather than an inline form: the list is a table and stays readable.
- `POST /dashboard/comments/<id>/reply` (`text`) and `POST /dashboard/comments/<id>/reply/delete`
  - session, CSRF, role and scope checked like the other moderation actions
  (`cms_comments_can_moderate()` with the author's `owner_id`).
- **Replying to a pending comment approves it**: answering a comment while keeping it hidden
  makes no sense, and it saves a click. The notice says so: "Reply saved, comment approved".
- The list shows the reply under the comment's text, as *Reply (Pablo Toledo):* + text.

### 2.4 Public page, every epoch

Under the comment, set apart and marked as the author's:

| Epoch | Shape |
|---|---|
| 3 | A nested block `boat-rudder-comment__reply` (indent, left border): **Pablo Toledo** · *Author* · date, then the text |
| 2 | `<blockquote>`: **Pablo Toledo** (author) - date, text |
| 1 | `<blockquote>` with `<font color>` (Mosaic and Netscape indent it) |
| 0 | `<blockquote>`: Lynx indents it |
| −1 (WAP) | A line under the comment: `Re (Pablo Toledo): text` |

New template per epoch: `comments/comment-reply_epoch{-1,0,1,2,3}.html` (name, date, text,
color), placed by `comment-item_*` as one more argument. The "Author" mark says the reply comes
from the site, not from a visitor using the author's name (a visitor can't use a team member's
name anyway - §3 of comments.md).

### 2.5 Telling the commenter

When a reply is **first** saved, the commenter gets an email: "Pablo Toledo replied to your
comment on «title»", the reply's text and the link to the entry (`#comments`). Only if their
email was confirmed and they aren't blocked; not on edits, not when the commenter is the replier
themselves. Through `mailer_enqueue()`, like the other mails.

## 3. Security

| Concern | Answer |
|---|---|
| An author replying on someone else's entry | Same scope check as moderation |
| Reply markup | Plain text, escaped, line breaks kept - like comments |
| CSRF | The dashboard's token on every POST |
| Mail used to spam a commenter | One email per comment, at the first reply only |

## 4. Work

| # | Piece | Files |
|---|---|---|
| 1 | `reply` read/write/delete; `CmsComment` / `CmsCommentAdmin` get the reply; notice data | `src/db/cms_comments.{c,h}` |
| 2 | Reply page, list display, actions | `src/modules/comments_admin/`, `html/templates/dashboard/comments/` |
| 3 | Routes GET/POST reply, POST reply/delete | `src/web_server/http_router.c` |
| 4 | Public display, every epoch | `src/modules/comments/comments.c`, `html/templates/comments/`, CSS |
| 5 | Email to the commenter | `src/modules/comments/comments.c` |
| 6 | Tests: end to end (reply, edit, delete, scope, pending → approved, every epoch, the email) | manual script |
| 7 | Docs: comments.md, routes.md, data-model.md, templates-catalog.md, CHANGELOG | `develop_docs/` |

## 5. Questions

1. **Administrador** replying too (proposed), or only the entry's author?
2. **Reply to a pending comment approves it** (proposed), or reply only to approved ones?
3. **Email the commenter** at the first reply (proposed), or no email?
