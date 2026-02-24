#ifndef WORKER_H
#define WORKER_H

#include "ts_queue.h"

// The shared task queue
extern ts_queue_t task_queue;

typedef struct {
    int client_fd;
    int conn_type;
} Task;

void* worker_thread(void* arg);

#endif
