#include "ts_queue.h"
#include <stdlib.h>

void queue_init(ts_queue_t* q) {
  q->head = 0;
  q->tail = 0;
  q->count = 0;
  q->shutdown = 0;
  pthread_mutex_init(&q->lock, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
}

void queue_write(ts_queue_t* q, void* data_ptr) {
  pthread_mutex_lock(&q->lock);

  while (q->count == QUEUE_MAX_SIZE) {
    pthread_cond_wait(&q->not_full, &q->lock);
  }

  q->buffer[q->tail] = data_ptr;
  q->tail = (q->tail + 1) % QUEUE_MAX_SIZE;
  q->count++;

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->lock);
}

int queue_read(ts_queue_t* q, void** data_ptr) {
  pthread_mutex_lock(&q->lock);

  while (q->count == 0 && !q->shutdown) {
    pthread_cond_wait(&q->not_empty, &q->lock);
  }

  if (q->count == 0 && q->shutdown) {
    pthread_mutex_unlock(&q->lock);
    return 0; // Queue empty and shutting down
  }

  *data_ptr = q->buffer[q->head];
  q->head = (q->head + 1) % QUEUE_MAX_SIZE;
  q->count--;

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->lock);
  return 1; // Success
}

void queue_shutdown(ts_queue_t* q) {
  pthread_mutex_lock(&q->lock);
  q->shutdown = 1;
  pthread_cond_broadcast(&q->not_empty); // Wake all waiting readers
  pthread_cond_broadcast(&q->not_full);  // Wake all waiting writers (if any)
  pthread_mutex_unlock(&q->lock);
}

void queue_destroy(ts_queue_t* q) {
  pthread_mutex_destroy(&q->lock);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
}
