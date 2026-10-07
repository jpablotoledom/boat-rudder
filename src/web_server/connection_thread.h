#ifndef CONNECTION_THREAD_H
#define CONNECTION_THREAD_H

#include <pthread.h>
#include "connection.h"

struct thread_args {
    int         client_socket;
    const char *root_directory;
    SSL        *ssl;
};

void *connection_thread(void *arg);

#endif // CONNECTION_THREAD_H
