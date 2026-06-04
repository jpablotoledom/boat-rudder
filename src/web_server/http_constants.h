#ifndef HTTP_CONSTANTS_H
#define HTTP_CONSTANTS_H

#include <limits.h>

// Maximum size of the raw HTTP header buffer (32 KiB).
// Requests exceeding this receive 431 Request Header Fields Too Large.
#define RAW_REQUEST_SIZE  32768

// Maximum body size accepted (10 MiB).
#define MAX_BODY_SIZE     (10 * 1024 * 1024)

// Maximum resolved filesystem path length.
#define MAX_PATH_LENGTH   (PATH_MAX + 256)

// URL query parameter limits.
#define MAX_PARAMS        16
#define MAX_PARAM_LENGTH  256

// Compile-time sanity checks (C11+).
_Static_assert(RAW_REQUEST_SIZE >= 4096,
    "RAW_REQUEST_SIZE must be at least 4 KiB to hold minimal HTTP headers");
_Static_assert(MAX_BODY_SIZE > RAW_REQUEST_SIZE,
    "MAX_BODY_SIZE must be larger than the header buffer");
_Static_assert(MAX_PARAMS > 0 && MAX_PARAMS <= 64,
    "MAX_PARAMS must be between 1 and 64");

#endif // HTTP_CONSTANTS_H
