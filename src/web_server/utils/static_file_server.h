#ifndef STATIC_FILE_SERVER_H
#define STATIC_FILE_SERVER_H

// Serve a static file or directory.
// if_modified_since: value of the client's If-Modified-Since header (may be NULL).
// Responds 304 Not Modified when the file has not changed since that date.
void serve_static_file(void *ctx,
                       const char *root_directory,
                       const char *decoded_url,
                       const char *if_modified_since);

#endif // STATIC_FILE_SERVER_H
