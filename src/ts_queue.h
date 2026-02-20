#ifndef TS_QUEUE_H
#define TS_QUEUE_H

#include <pthread.h>

// You can change this or make it dynamic later
#define QUEUE_MAX_SIZE 100

// The generic Thread-Safe Queue structure
typedef struct {
  void* buffer[QUEUE_MAX_SIZE]; // void* allows holding ANY struct pointer
  int head;
  int tail;
  int count;
  int shutdown;
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} ts_queue_t;

// Public API Functions
void queue_init(ts_queue_t* q);
void queue_write(ts_queue_t* q, void* data_ptr);
int  queue_read(ts_queue_t* q, void** data_ptr);
void queue_shutdown(ts_queue_t* q);
void queue_destroy(ts_queue_t* q);

#endif // TS_QUEUE_H
