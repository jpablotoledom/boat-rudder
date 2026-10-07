#ifndef IMAGE_SIZE_H
#define IMAGE_SIZE_H

// Intrinsic pixel width of an image served by this site.
//
// Retro epochs need the width of a content image in *pixels*: percentage
// values on the `width` attribute of <img> arrived with HTML 4.0, and a
// browser of the epoch-2 era draws the image zero-wide when it meets one.
// The percentage the author chose is therefore resolved against the real
// width of the file being served.
//
// `url_path` is the site-absolute path of the image ("/content/posts/x.gif"),
// resolved under ./html like every other served file. Returns 0 when the
// width cannot be established - the file is missing, unreadable, or not a
// GIF - and the caller should then fall back to the image's natural size.
// Only GIF is read: every epoch below 3 is served the `_medium` variant,
// which image-optimizer.sh always writes as GIF.
int image_intrinsic_width(const char *url_path);

#endif // IMAGE_SIZE_H
