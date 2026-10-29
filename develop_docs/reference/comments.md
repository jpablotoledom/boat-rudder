# Blog comments

Visitors can comment on **blog entries** (never pages) from **epoch 3** browsers; every other
epoch (WAP, text, HTML 3.2, HTML 4) shows the approved comments read-only. A comment needs
a name, an email and the answer to a challenge Boat Rudder draws itself; the visitor confirms the
email through a link, and the comment waits for a moderator - an Administrador, a Moderador, or
the Autor of the entry. Off by default; it needs outgoing mail.

A commenter is a **user** (`users`, `role: "commenter"`): no password, can't sign in, listed in
**Users → Commenters**. Making one part of the team is choosing another role and giving them a
password ([dashboard.md](dashboard.md#roles-and-privileges-usersrole-entriescreated_by)).

Design and its decisions: [plans/comments-plan.md](../plans/comments-plan.md). Diagrams:
[comments-components.puml](../diagrams/comments-components.puml),
[comments-data-model.puml](../diagrams/comments-data-model.puml),
[comment-states.puml](../diagrams/comment-states.puml),
[sequence-comment-submit.puml](../diagrams/sequence-comment-submit.puml),
[sequence-comment-moderation.puml](../diagrams/sequence-comment-moderation.puml).

---

## 1. Turning it on

1. Mail in `configs/settings.conf` ([configuration.md](configuration.md#mail)):
   `mail_smtp_url`, `mail_smtp_user`, `mail_smtp_password`, `mail_from` (and optionally
   `mail_from_name`). Restart the server.
2. **Dashboard → Site → General → Comments**: *Send a test email* (it goes to your own
   account's address), then tick *Visitors can comment on blog entries*. The checkbox can't be
   ticked without mail.
3. Per entry, the entry editor's sidebar has **Close comments** (blog entries only): the approved
   comments stay, the form goes.

## 2. The visitor

Under a published blog entry (`comments_section()`, `src/modules/comments/`):

- **Comments (N)** - the approved ones, oldest first: name, date (UTC), text. Text is plain:
  escaped, line breaks kept, no links or markup.
- **The form**: name (2-40 bytes: letters of any language, digits, spaces, `. _ - '`), email
  (never shown publicly), comment (up to 2000 bytes), the challenge image and its answer, and a
  hidden honeypot field.
- After sending: `?comment=sent` - "check your email". A browser that already confirmed this
  email (cookie, §5) skips the email: `?comment=pending`.
- A refused form comes back with the reason, what was typed, and a **new** challenge.

**Older epochs** (−1…2, `retro_section()`): the approved comments only - `Comments (N)`, then
name, date and text per comment (`comments/comments_epoch<N>.html`,
`comment-item_epoch<N>.html`; epochs 1-2 in the theme's home-content text color, like the
entry's paragraphs; WML with `$` doubled) - and a line saying a modern browser is needed to
write one (or that comments are closed). Nothing at all while an entry has no approved comment,
or while comments are off.

The email links to `/comments/verify?token=…`. That page only shows a **Confirm** button: the
`GET` itself confirms nothing, since mail scanners open links on their own. The `POST` moves the
comment to *pending*, marks the email confirmed and sets the remember cookie. The link works
once, for 24 hours; an unconfirmed comment is deleted when it expires.

## 3. Checks on a comment, in order

| Check | Refusal |
|---|---|
| Comments on, mail set, entry is a published blog entry, not closed | redirect back / `404` |
| Honeypot field empty | *pretends success*, stores nothing |
| Name, email, text shapes and lengths | form + message |
| Challenge: signed, answered right, ≥ 3 s and ≤ 15 min after it was drawn, never used before | form + message, new image |
| ≤ 5 comments per address per hour, ≤ 10 per email per day | form + message |
| Name not one of the site's users' names/usernames, nor another email's | form + message |
| Email not blocked | form + message |

## 4. The challenge (`src/utils/captcha.c`)

- 5 characters from `ACDEFHJKMNPRTUVWXY34679` (no look-alikes), drawn from a built-in 5x7 font:
  each glyph scaled, slanted and shifted at random, then the whole image waved, crossed by lines
  and dotted. 200x70 grayscale **PNG**, encoded here (stored deflate, no image library), inlined
  as `data:image/png;base64`.
- Stateless token in the form: `nonce.issued.HMAC-SHA256(secret, "captcha|nonce|issued|ANSWER")`.
  Nothing is stored when the image is drawn; the answer is checked by recomputing the mac. The
  issue time is inside the mac, so the 3 s / 15 min window can't be faked.
- **Single use**: an accepted nonce goes into `captcha_used` (unique index, TTL).
- Answers ignore case and spaces. Unit test: `tests/captcha_test.c`.
- Limitation: an image excludes readers who can't see it; there is no audio or text alternative.

## 5. Secrets, cookies and stored data

- **Site secret**: 32 random bytes generated once into `site_settings.secret`
  (`cms_get_site_secret()`), keying every HMAC below. Rotating it (deleting the field) voids
  open challenges and every remember cookie.
- **Confirmation token**: 32 random bytes, only in the email; the database keeps its SHA-256
  (`comment_tokens`, TTL 24 h).
- **Commenters** are `users` documents created by the form: `role: "commenter"`, a username, the
  name and its folded `name_key`, `email_verified_at`, `blocked`; no password. Emails match
  without case, and a team member's own address is their own account. An older `commenters`
  collection is moved into `users` at startup (same `_id`s).
- **Remember cookie** `br_commenter=<commenter id>.<issued>.<HMAC>` - `HttpOnly`, `SameSite=Lax`,
  1 year. Valid only while the commenter's email stays confirmed and not blocked.
- **Addresses**: only `HMAC(secret, "ip|<address>")` (16 bytes, hex) is stored, for the limits.
- Emails are shown only in the dashboard.

Collections: [data-model.md](data-model.md#comments).

## 6. Moderation (`/dashboard/comments`, `src/modules/comments_admin/`)

- Tabs **Pending (N)** / **Approved** / **Rejected**: entry, name and email (with *Blocked*),
  text, date, actions.
- **Approve** (*Restore* on a rejected one), **Reject** (*Hide* on an approved one), **Delete**.
- **Unverified (N)**: comments whose email was never confirmed, each with how its confirmation
  email is doing - *sent, not confirmed yet*, *not sent yet - retrying* (with the SMTP error),
  *failed* - read from `mail_queue`. **Resend email** sends a new link (the old one stops
  working) and gives the comment 24 more hours. **Approve** works here too: the moderator's
  approval stands in for the email check, and the comment no longer expires. Unverified comments
  are never public.
- **Block email** / **Block and delete all** / **Unblock email** - Administrador only.
- Who sees what: an Administrador (and the read-only Observador) every comment; a **Moderador**
  every comment, but it only approves and rejects - no Delete, no blocking; an Autor the comments
  on **their own** blog entries (`entries.created_by`), and only those can they moderate.
  Writes go through the dashboard's guards: session, CSRF, the Observador refused.
- The dashboard home starts with "N comments are waiting for moderation" when there are any, and
  the menu's *Content* group links *Comments*.
- **The author is told by email** when a comment reaches *pending* - at the `POST` of
  `/comments/verify`, or at once from a remembered browser - with the commenter's name, the text
  and links to `/dashboard/comments` and the entry (`notify_author()`, `cms_comments_notice()`).
  The author is the entry's `header.author_id`, else `created_by` (older entries carry only the
  first). No email when that user is a commenter, has no email, or wrote the comment; a comment
  approved straight from *Unverified* sends none either. Links use `public_url`, else the
  request's host.

## 7. Mail (`src/utils/mailer.c`)

- `mailer_enqueue()` stores a message in `mail_queue` and wakes the sender thread; a request never
  waits for an SMTP server.
- The sender claims due messages one at a time (`queued` → `sending`), sends through libcurl
  (`smtps://`, or `smtp://` with STARTTLS required - except to `localhost`/`127.*`, a local
  relay), and marks them `sent`, or `queued` again after 1, 5, 15, 60 minutes, then `failed`
  (`last_error` kept). A message left `sending` by a stopped server is queued again at start.
- Messages are plain-text UTF-8; non-ASCII subjects and sender names are RFC 2047-encoded.
