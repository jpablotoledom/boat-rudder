# Blog comments

Visitors can comment on **blog entries** (never pages) from **every epoch** - WAP phones, text
browsers, HTML 3.2, HTML 4, HTML5. A comment needs
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
[sequence-comment-submit-retro.puml](../diagrams/sequence-comment-submit-retro.puml),
[sequence-comment-moderation.puml](../diagrams/sequence-comment-moderation.puml); epochs 1-2:
[plans/comments-retro-plan.md](../plans/comments-retro-plan.md).

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
  escaped, line breaks kept, no links or markup. Under a comment, the team's reply when it has
  one (§6).
- **The form**: name (2-40 bytes: letters of any language, digits, spaces, `. _ - '`), email
  (never shown publicly), comment (up to 2000 bytes), the challenge image and its answer, and a
  hidden honeypot field.
- After sending: `?comment=sent` - "check your email". A browser that already confirmed this
  email (cookie, §5) skips the email: `?comment=pending`.
- A refused form comes back with the reason, what was typed, and a **new** challenge.

**Epochs 0-2** get the same section and form in their own markup (`comments/*_epoch0.html`,
`*_epoch1.html`, `*_epoch2.html`: plain HTML, `<font>`, tables, `<a name="comments">`; on 1-2 text
in the theme's home-content color, like the entry's paragraphs), with three differences:

- The challenge: on epochs 1-2 a **GIF by URL** - `GET /comments/captcha?t=<token>` (§4) - since
  those browsers know neither `data:` URIs nor, all of them, PNG; on epoch 0 the same characters
  as **text art** in a `<pre>`, for browsers without images (Lynx, w3m, ELinks).
- **No honeypot field**: without (reliable) CSS a person would see it and fill it.
- **Form input in Latin-1**: epoch 1 pages go out in Latin-1, so their forms come back in it
  (as do some epoch 2 browsers'). A field that isn't valid UTF-8 is read as Windows-1252 and
  converted before any check (`to_utf8()`).

The form's `action` carries `?lang=` (epochs 0-1) and `?theme=` (1-2), like every link
([rendering.md](rendering.md)); after sending, the redirect keeps them, names the full URL
(Mosaic can't resolve a bare path), and on epoch 1 has no `#comments`, which an HTTP/1.0
browser may send on as part of the query. A browser that sends no `Host` (NCSA Mosaic 2) gets
the entry with its notice directly instead: no URL could name the server for it. Its reload
resends the form, which the spent challenge refuses.

**WAP, epoch −1** (`retro_section()`): `Comments (N)`, the approved comments - name, date and
text (`$` doubled) - or "No comments yet.", then a **Write a comment** link (or "Comments are
closed"). Nothing at all while there is neither a comment nor a way to write one. The form isn't
under the entry: a WAP deck is cut into ~860-byte packets (one gateway reply each, see
[wap-gateway.md](wap-gateway.md)) and a form must not be split across them, so it has pages of
its own, each small enough for one packet even with the gateway's absolute links
(`comments_wap_form_page()`, `GET /blog/<link>/comment`):

1. **Step 1** - name, email and comment (up to 300 characters on a keypad, `COMMENT_WAP_TEXT_MAX`;
   the server still takes 2000), "We email you a link to confirm it", **Next >**.
2. **Step 2** (`?step=2`) - the challenge as a 96x32 black-and-white **WBMP**
   (`/comments/captcha?t=…&format=wbmp`, §4), the answer and **Send**: a `<go method="post">`
   whose `<postfield>`s carry step 1's values.

What is typed lives in **WML variables** (`cn`, `cm`, `ct`), which a phone keeps from deck to
deck: step 2 posts them, and coming back to step 1 shows them still filled in. The answer's
variable is named after its token (`a<first 8 hex>`), so an old answer never fills in a new
challenge. A refused comment gets a page of its own - the reason, **Try again** (a new step 2)
and **< Edit** (step 1) - since the reason and the challenge together don't fit one packet.
After sending: `302` to `/blog/<link>/comment?comment=sent` (or `pending`), a short page with the
thanks and **Back to the entry** - the entry's own comments section is many packets from its
top. The phone sends no cookie through the gateway, so every WAP comment is confirmed by email.
Through the WAP gateway the post goes as a WSP Post, forwarded with the phone's own User-Agent
([wap-gateway.md](wap-gateway.md#forms-wsp-post)). HTTP/0.9 browsers on epoch 0 see the form but
can't send it: that protocol has no POST.

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
  each glyph scaled, slanted and shifted, then the whole image waved, crossed by lines and
  dotted. 200x70, encoded here with no image library: grayscale **PNG** (stored deflate) inlined
  as `data:image/png;base64` on epoch 3; 16-gray **GIF87a** (LZW kept uncompressed - a clear
  code every 12 pixels) at `GET /comments/captcha?t=<token>` on epochs 1-2; **text art** on
  epoch 0 (`captcha_token_text()`): each glyph pixel two characters wide, an ink character per
  glyph, slanted and shifted, a few holes, scattered punctuation - 9 lines within 80 columns.
  A bot built for this site reads text art more easily than an image; the 3 s minimum, the
  limits, email confirmation and moderation still apply. **WAP**: a 96x32 one-bit **WBMP**
  (`captcha_token_wbmp()`, `/comments/captcha?t=…&format=wbmp`, `image/vnd.wap.wbmp`) - the
  glyphs at twice their size, each shifted up or down, a few holes and dots and a dotted line;
  lighter than the GIF, as a phone's screen can take no more and stay readable.
- Stateless token in the form: `nonce.issued.HMAC-SHA256(secret, "captcha|nonce|issued|ANSWER")`.
  The **answer is derived** from the token under the secret - `HMAC-SHA256(secret,
  "captcha-text|nonce|issued")` mapped onto the alphabet - so it is in no token and no record,
  yet the server can draw the image again from the token alone. The drawing's randomness is a
  ChaCha20 stream seeded by `HMAC-SHA256(secret, "captcha-draw|nonce|issued")`: **one token,
  one image** - reloading it gives a bot nothing new. The answer is checked by recomputing the
  mac; the issue time is inside it, so the 3 s / 15 min window can't be faked.
- `/comments/captcha` (GIF, or WBMP with `format=wbmp`) draws only a token that is the site's own and unexpired (else `404`),
  `Cache-Control: no-store`, and stores nothing.
- **Single use**: an accepted nonce goes into `captcha_used` (unique index, TTL).
- Answers ignore case and spaces. Unit test: `tests/captcha_test.c` (derived answer, same token
  same image, GIF/PNG structure, expired / tampered / foreign tokens).
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
- **Where it was written from**: `browser` and `os` (`ua_parser.h`, the analytics classifier:
  `"MSIE/5"`, `"Windows/2000"`), the `user_agent` itself (printable ASCII, ≤ 256 bytes, so it can
  be classified again) and the `epoch`.
- **Shown as icons** next to the commenter's name (`comment_client_html()`): the browser's and the
  system's 20x20 GIFs from `html/assets/icons/clients/`, their names as alt text (and `title` on
  epochs 2-3); on epoch 0 and WAP the names as text, `[Lynx 2]` / `(Lynx 2)`. `client_icon.c` maps
  the stored keys to an icon and words - "MSIE/5" → `ie.gif`, "Internet Explorer 5";
  "Windows/2000" → `windows-classic.gif` - and reads the User-Agent where the keys say nothing
  (Mac OS before X, IRIX, other Unixes, DOS). DOS builds of Lynx and Links don't name their
  system unless their User-Agent is set to (`USER_AGENT:Lynx/2.9.0 (DOS) ...` in `lynx.cfg`);
  `DOS`, `MS-DOS`, `FreeDOS` or `DJGPP` in it, or Arachne (DOS only), show `dos.gif`. An unknown system shows no system icon (text browsers
  rarely say theirs); comments older than this record show none. The moderation list shows the
  icons and, under the date, the words: `MSIE 5 · Windows 2000 · epoch 2`.
- **The icons** are Boat Rudder's own pixel drawings, made to be recognizable - not the vendors'
  logos - drawn by `scripts/generate-client-icons.py` (`python3 scripts/generate-client-icons.py
  --preview sheet.png` redraws them all and a sample sheet): browsers `chrome`, `firefox`,
  `edge`, `safari`, `opera`, `ie`, `netscape`, `mosaic`, `text` (Lynx, Links, w3m, ELinks), `arachne`,
  `wap` (WAP phones, Rover, the WAP gateway), `browser` (any other); systems `windows` (8 and later), `windows-xp` (XP to 7),
  `windows-classic` (3.x to 2000), `mac`, `mac-classic`, `linux`, `android`, `ios`, `bsd`,
  `unix` (Solaris, IRIX, BeOS...), `dos`, `os2` (OS/2, and IBM WebExplorer, which
  only ran there), `palm` (Palm OS - read before any other system,
  as its browsers' User-Agents name another one too), `os`. GIF89a with a transparent background; a new icon
  needs a line in both the script and `client_icon.c`.
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
- **Replying** - one reply per comment, from the team only (visitors comment, they don't
  reply). *Reply* on a pending or approved comment (*Edit reply* once it has one) opens
  `/dashboard/comments/<id>/reply`: the comment and a text box. An **Administrador** replies to
  any comment, an **Autor** to those on their own blog entries; the Moderador and the Observador
  don't. Saving again replaces the reply (`reply.updated_at`, the first date kept); *Delete
  reply* removes it; deleting the comment deletes it too. **Replying to a pending comment
  approves it.** Plain text like a comment (≤ 2000 bytes, escaped, line breaks kept), stored in
  the comment as `reply` (text, the replier's id and name, `by_author`, dates). The list shows
  it under the comment's text.
- **On the page** the reply sits under its comment with the replier's name, the date and
  *Author* (the entry's `header.author_id` or `created_by`) or *Administrator* (anyone else):
  epoch 3 an indented block (`boat-rudder-comment__reply`, `__badge`), epochs 0-2 a
  `<blockquote>`, WAP a line `Re: name (Author) date` (`comment-reply_epoch*.html`).
- **The commenter is told by email** at a comment's first reply (not at edits) - the reply, their
  comment and the link to the entry - when their email was confirmed, they aren't blocked and
  they aren't the one replying (`notify_commenter()`, `cms_comments_reply_notice()`). A reply
  deleted and written again counts as a first one.
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
