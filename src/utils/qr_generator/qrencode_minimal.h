/* Minimal declarations for libqrencode — header-only substitute for libqrencode-dev */
#ifndef QRENCODE_MINIMAL_H
#define QRENCODE_MINIMAL_H

typedef enum { QR_ECLEVEL_L=0, QR_ECLEVEL_M, QR_ECLEVEL_Q, QR_ECLEVEL_H } QRecLevel;
typedef enum { QR_MODE_NUL=-1, QR_MODE_NUM=0, QR_MODE_AN, QR_MODE_8 } QRmode;

typedef struct {
    int version;
    int width;
    unsigned char *data;
} QRcode;

QRcode *QRcode_encodeString(const char *string, int version,
                             QRecLevel level, QRmode hint, int casesensitive);
void QRcode_free(QRcode *qrcode);

#endif
