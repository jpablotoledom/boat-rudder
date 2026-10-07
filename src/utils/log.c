#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

int log_level = LOG_LEVEL_INFO;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_write(int level, const char *prefix, const char *fmt, ...) {
    if (log_level < level) return;

    va_list args;
    va_start(args, fmt);

    pthread_mutex_lock(&log_mutex);
    fputs(prefix, stdout);
    vprintf(fmt, args);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);

    va_end(args);
}
