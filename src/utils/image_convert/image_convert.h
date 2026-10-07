#ifndef IMAGE_CONVERT_H
#define IMAGE_CONVERT_H

// Decodes an image file (PNG/JPEG, via the vendored stb_image.h) at
// `in_path`, scales it down (nearest-neighbor, aspect-preserved, never
// upscales) to fit within `max_dim` x `max_dim` if it's larger, converts to
// grayscale and thresholds to 1-bit (flat 50% threshold, no dithering - flat
// logo art reads fine this way; a photo-like source would look better with
// dithering, not implemented here), and writes the result as a WBMP file to
// `out_path` (see wbmp_writer.h). Used to auto-convert an epoch -1 (WAP/WML)
// logo upload to the WBMP format those devices require.
//
// Returns 0 on success, -1 if the file can't be decoded (missing, unreadable,
// or not a format stb_image supports) or the WBMP write fails.
int image_convert_to_wbmp(const char *in_path, const char *out_path, int max_dim);

#endif // IMAGE_CONVERT_H
