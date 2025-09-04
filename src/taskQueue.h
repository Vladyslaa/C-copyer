#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct task_queue_t {
    copy_task_t* tasks;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int shutdown;
} task_queue_t;

task_queue_t* queue_create(size_t capacity);
int queue_destroy(task_queue_t* queue);

void queue_enqueue(task_queue_t* queue, copy_task_t task);
void queue_enqueue_batch(task_queue_t* queue, copy_task_t* tasks_batch, int batch_count);
int queue_pop(task_queue_t* queue, copy_task_t* out_task);
int queue_pop_batch(task_queue_t *queue, copy_task_t *out_tasks_batch, int max_batch_count);

#ifdef __cplusplus
}
#endif

#endif