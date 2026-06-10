#ifndef STATIC_FILE_SERVER_H
#define STATIC_FILE_SERVER_H

// Serve a static file or directory.
// if_modified_since: value of the client's If-Modified-Since header (may be NULL).
// Responds 304 Not Modified when the file has not changed since that date.
//
// Returns 0 if a response was already written (200, 304, or a streaming
// failure that closed the connection mid-transfer). Otherwise returns an
// HTTP status code (403, 404 or 500) that the caller should report via its
// own (epoch-aware) error response - in that case, no response has been
// written yet.
int serve_static_file(void *ctx,
                      const char *root_directory,
                      const char *decoded_url,
                      const char *if_modified_since);

#endif // STATIC_FILE_SERVER_H
