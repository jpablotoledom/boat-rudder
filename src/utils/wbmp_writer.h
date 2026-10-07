#ifndef WBMP_WRITER_H
#define WBMP_WRITER_H

// Writes a WBMP (type 0: B&W, no compression) file to `path` from a flat
// one-byte-per-pixel `pixels` buffer, `pixels[y*width+x] & 1`, where a
// nonzero (odd) value means "dark" (rendered black); 0 means "light"
// (rendered white). Pure C, no external dependencies - shared by the QR
// code generator (qr_generator.c) and the logo image-to-WBMP converter
// (image_convert.c). Returns 0 on success, -1 on a write/fopen failure.
int write_wbmp(const char *path, const unsigned char *pixels, int width, int height);

#endif // WBMP_WRITER_H
