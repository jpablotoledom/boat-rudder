#ifndef COMMENTS_ADMIN_H
#define COMMENTS_ADMIN_H

// /dashboard/comments: visitors' comments by status ("pending", "approved",
// "rejected") with Approve / Reject, and Delete when `can_delete` (not a
// moderator), blocking a commenter's email when `can_block` (an
// Administrador). `owner_id` limits the list to that user's blog entries
// (an Autor); NULL for every entry. `notice` (may be NULL) is shown above
// the list. Epoch 3. Returns a malloc'd fragment or NULL.
char *comments_admin_page(int epoch, const char *status, const char *owner_id, int can_delete,
                          int can_block, const char *lang, const char *notice);

// The dashboard home's "N comments waiting for moderation" line, "" when
// there are none. Returns a malloc'd string, NULL on allocation failure.
char *comments_admin_pending_notice(int epoch, const char *owner_id);

#endif // COMMENTS_ADMIN_H
