#ifndef LOG_H
#define LOG_H

// Leveled, thread-safe logging.
// All output is serialized through an internal mutex; safe for concurrent threads.
//
// Usage: LOG_INFO("listening on port %d", port);
//        LOG_ERROR("file not found");   // no format args needed
//
// The macros delegate to log_write() so __VA_ARGS__ always contains at least
// the format string - no zero-argument variadic expansion, no GCC extensions,
// fully standard C17.

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

extern int log_level;

// log_write is not meant to be called directly; use the macros below.
// The __attribute__ lets GCC/Clang type-check format strings at compile time.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void log_write(int level, const char *prefix, const char *fmt, ...);

#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, "ERROR: ", __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN,  "WARN:  ", __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO,  "INFO:  ", __VA_ARGS__)
#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, "DEBUG: ", __VA_ARGS__)

#endif // LOG_H
