#ifndef READ_FILE_H
#define READ_FILE_H

// Reads the full contents of `filename` into a NUL-terminated, malloc'd
// buffer. Returns NULL if the file cannot be opened/read or on allocation
// failure. The caller must free() the returned buffer.
char *read_file_to_string(const char *filename);

#endif // READ_FILE_H
