#ifndef MAILER_H
#define MAILER_H

// Outgoing mail. A request never waits for an SMTP server: mailer_enqueue()
// stores the message in `mail_queue` and returns; one sender thread sends
// queued mail through libcurl (mail_smtp_* keys, config_loader.h), retrying
// after 1, 5, 15 and 60 minutes before marking it failed.

// 1 when mail_smtp_url and mail_from are set.
int mailer_configured(void);

// Rough shape check for an address a visitor typed: one '@', something on
// both sides, a dot in the domain, no spaces, quotes, angle brackets or
// control characters, at most 254 bytes.
int mailer_valid_address(const char *address);

// Queues a plain-text UTF-8 message to `to`. Returns 0, or -1 if mail isn't
// configured, `to`/`subject` are unusable (line breaks, bad address) or on a
// DB error.
int mailer_enqueue(const char *to, const char *subject, const char *text);

// Starts / stops the sender thread (main.c). Start is a no-op without mail
// configured or MongoDB; returns 0, -1 if the thread can't start.
int mailer_start(void);
void mailer_stop(void);

#endif // MAILER_H
