#ifndef WORKER_H
#define WORKER_H

#include "ts_queue.h"

// The shared task queue defined in main.c
extern ts_queue_t task_queue;

typedef struct { 
    int client_fd; 
    int client_index; 
} Task;

void* worker_thread(void* arg);

#endif
