# Plan: writing comments from epochs 1 and 2

> **Status**: implemented (2026-10-29), and epoch 0 after it (the challenge as text art, see
> [comments.md](../reference/comments.md)), with the proposed answers to §6: GIF on both epochs, the
> form on entries with no comments yet, epoch 3 keeps its inline image. Form actions are tagged
> with `?theme=`/`?lang=`/`?site=` by the response layer, like links. Follows
> [comments-plan.md](comments-plan.md); reference: [comments.md](../reference/comments.md).
> Diagram: [sequence-comment-submit-retro.puml](../diagrams/sequence-comment-submit-retro.puml).

---

## 1. What is asked

Today a comment can only be **written** from epoch 3; epochs −1…2 show the approved comments
read-only, with "open this page in a modern browser". Bring the **form** to more epochs, in
stages:

1. **Epochs 1 and 2** (HTML 3.2 / HTML 4: Mosaic, Netscape 2-4, IE 3-6...) - this plan.
2. Epoch 0 (text browsers) - later, with a text challenge instead of an image.
3. Epoch −1 (WAP) - later or never: the WAP gateway only sends GETs.

Everything else stays as it is: the same checks, email confirmation, moderation, limits and
remembered browsers.

## 2. What stops it today

| Piece | Today | Why it fails on epochs 1-2 |
|---|---|---|
| Challenge image | `<img src="data:image/png;base64,...">` | Those browsers don't know `data:` URIs, and several don't show PNG at all |
| Challenge token | `nonce.issued.mac`, the answer only *checkable* | A separate image request has nothing to draw from: the answer is in no token and no record |
| Honeypot | A text field hidden by CSS | Without (reliable) CSS a person sees it, fills it, and the comment is silently dropped |
| Form input | Assumed UTF-8 | Epoch 1 pages are served in Latin-1 (`utf8_to_latin1()`), so its browsers send Latin-1: "Peña" arrives as invalid UTF-8 and is refused |
| Route | `POST /blog/<link>/comments` redirects any epoch but 3 back | - |
| Templates | `comment-form`, `comment-error`, `comments-notice`, `comments-closed` exist for epoch 3 only | - |

## 3. Design

### 3.1 Challenge: the answer derived from the token

The answer stops being random and stored nowhere; it becomes a function of the token and the
site secret:

```
answer = 5 characters drawn from HMAC-SHA256(secret, "captcha-text|nonce|issued")
mac    = HMAC-SHA256(secret, "captcha|nonce|issued|ANSWER")      (as today)
token  = nonce.issued.mac                                         (as today)
```

- Whoever holds the token can't compute the answer without the secret - same strength as today.
- The server can **redraw** the image from the token alone, so the image can be a URL.
- The drawing's randomness (slant, waves, lines, dots) is seeded from the same HMAC: one token
  always gives **the same image**. Reloading it gives a bot no new samples to average.
- `captcha_check()` is unchanged in what it accepts: token + answer, 3 s ≤ age ≤ 15 min, single
  use by nonce (`captcha_used`).
- Epoch 3 keeps its inline `data:` PNG (no extra request), from the same token scheme.

### 3.2 Image by URL: `GET /comments/captcha?t=<token>`

- Verifies the token's shape and age (≤ 15 min) and that its mac is the site's; then draws.
  Otherwise `404`. Nothing stored, nothing written.
- **GIF** (`image/gif`, 200x70, 16 grays): every browser of epochs 1-2 shows GIF; PNG support
  arrived unevenly. A small GIF87a encoder in `captcha.c` (LZW with a clear code often enough to
  keep codes 5 bits wide - about 100 lines, no library), like the PNG one.
- `Cache-Control: no-store`.
- The form: `<img src="/comments/captcha?t=..." width="200" height="70" alt="Challenge image">`.

### 3.3 Form input in Latin-1

Before any check, `comments_post()` decodes the form: valid UTF-8 is kept as is; anything else
is read as **Latin-1** (Windows-1252 for 0x80-0x9F) and converted to UTF-8. The name, email and
text checks then run as today. Epoch 2 pages declare UTF-8 and their browsers mostly send it; the
same rule covers the ones that don't.

### 3.4 Honeypot

Epochs 1-2 get **no honeypot field**. The server never required one (an empty or missing field
passes), so nothing changes there. Those epochs rely on the challenge, the 3 s minimum, the rate
limits and moderation - the same checks a bot that skips the honeypot meets on epoch 3.

### 3.5 Templates (`html/templates/comments/`)

New, for epochs 1 and 2, in each epoch's markup (tables and `<font>` for 1, light CSS for 2, the
theme's home-content text color like the read-only section):

| Template | Content |
|---|---|
| `comment-form_epoch1/2.html` | Name, email, comment (`<textarea>`), the image, the answer field, hidden token, *Send* |
| `comment-error_epoch1/2.html` | The refusal message above the form |
| `comments-notice_epoch1/2.html` | "Check your email" / "waiting for moderation" after sending |
| `comments_epoch1/2.html` | Changed: the comments, then the form - or the closed note |

`comments_section()` uses the form path for epochs 1-3 and `retro_section()` stays for −1 and 0
(today's read-only list and note). An entry with no comments yet shows the form on 1-2 too (today
the read-only section shows nothing).

### 3.6 Route and responses

- `POST /blog/<link>/comments` accepts epochs 1, 2 and 3 (−1 and 0 keep the redirect back).
- After sending: `302` to `/blog/<link>?comment=sent#comments` - already `302`, which every old
  browser follows.
- A refused form: the entry page again with the form, the message, what was typed and a new
  challenge - as on epoch 3.
- Theme and language: epochs 1-2 carry them in links (`?theme=`, `?lang=`); form actions aren't
  tagged by the response layer, so the form's `action` gets them written in, and the redirect
  keeps them.
- Remember cookie: those browsers keep cookies (Netscape 1.1+); unknown attributes
  (`SameSite`) are ignored. Unchanged.
- The confirmation link is opened from the email, usually on another device: unchanged.

### 3.7 What doesn't change

Storage, `comments`/`users`/`comment_tokens`, the limits, the name rules, email confirmation,
moderation, the author's notice, the dashboard.

## 4. Security

| Concern | Answer |
|---|---|
| A bot reading the answer from the token | It's an HMAC of the token under the site secret - not computable without it |
| A bot reloading the image for more samples | One token, one image (seeded drawing) |
| The image endpoint as a CPU sink | Only valid, unexpired tokens are drawn (mac checked first); a 200x70 drawing is cheap; the server's anti-DDoS limits apply |
| No honeypot on 1-2 | Challenge + 3 s + limits + moderation, as for any bot on epoch 3 that skips it |
| Latin-1 decoding | Converts bytes to UTF-8 before validation - no new characters get past the name/text rules |
| CSRF | None needed, as on epoch 3: no session involved, the challenge is single-use |

## 5. Work

| # | Piece | Files |
|---|---|---|
| 1 | Derived answer + seeded drawing; `captcha_draw_gif()`; token check for the image | `src/utils/captcha.{c,h}` |
| 2 | GIF encoder | `src/utils/captcha.c` |
| 3 | `GET /comments/captcha` | `src/web_server/http_router.c` |
| 4 | Form for epochs 1-2, Latin-1 decoding, image URL | `src/modules/comments/comments.c` |
| 5 | Route accepts epochs 1-2 | `src/web_server/http_router.c` |
| 6 | Templates (§3.5) | `html/templates/comments/` |
| 7 | Tests: derived answer, same token → same image, GIF header/size, Latin-1 decoding; end to end with epoch 1 and 2 user agents | `tests/captcha_test.c`, manual script |
| 8 | Docs: comments.md, routes.md, security.md, templates-catalog.md, CHANGELOG | `develop_docs/` |

Real browsers to try it on, if at hand: Netscape 3/4, IE 4-6, Mosaic 2.

## 6. Questions

1. **Image format for epoch 2** - GIF for both epochs (one path; proposed), or PNG for epoch 2?
2. **Empty entries on 1-2** - show the form when an entry has no comments yet (proposed), as
   epoch 3 does?
3. **Epoch 3** - keep the inline image (proposed), or move it to the URL too (one path, one more
   request)?
