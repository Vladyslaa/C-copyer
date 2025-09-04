#include "taskQueue.h"

#ifndef _WIN32
#include <stdlib.h>
#include <string.h>
#endif

task_queue_t* queue_create(size_t capacity) {
    task_queue_t *queue = malloc(sizeof(task_queue_t));
    if(!queue) {
        fprintf(stderr, RED "Cannot allocate memory for task_queue_t structure\n"  RESET);
        return NULL;
    }

    queue->tasks = calloc(capacity, sizeof(copy_task_t));
    if(!queue->tasks) {
        fprintf(stderr, RED "Cannot allocate memory for the tasks in task_queue_t\n" RESET);

        free(queue);

        return NULL;
    }

    queue->capacity = capacity;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->shutdown = 0;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        fprintf(stderr, RED "Cannot create mutex in task_queue_t\n" RESET);

        free(queue->tasks);
        free(queue);

        return NULL;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        fprintf(stderr, RED "Cannot initialise condition \"not_empty\" in task_queue_t\n" RESET);

        free(queue->tasks);
        pthread_mutex_destroy(&queue->mutex);
        free(queue);

        return NULL;
    }
    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        fprintf(stderr, RED "Cannot initialise condition \"not_full\" in task_queue_t\n" RESET);

        free(queue->tasks);
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue);

        return NULL;
    }

    return queue;
}

int queue_destroy(task_queue_t* queue) {
    if (!queue) return 0;

    free(queue->tasks);

    int errCode = pthread_cond_destroy(&queue->not_empty);
    if (errCode != 0) {
        fprintf(stderr, RED "Cannot destroy the condition \"not_empty\" in task_queue_t, code: \"%d\"\n" RESET, errCode);
        return -1;
    }

    errCode = pthread_cond_destroy(&queue->not_full);
    if (errCode != 0) {
        fprintf(stderr, RED "Cannot destroy the condition \"not_full\" in task_queue_t, code: \"%d\"\n" RESET, errCode);
        return -1;
    }

    errCode = pthread_mutex_destroy(&queue->mutex);
    if (errCode != 0) {
        fprintf(stderr, RED "Cannot destroy the mutex in task_queue_t, code: \"%d\"\n" RESET, errCode);
        return -1;
    }

    free(queue);
    return 0;
}

void queue_enqueue(task_queue_t* queue, copy_task_t task) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == queue->capacity) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % queue->capacity;
    ++queue->size;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

void queue_enqueue_batch(task_queue_t* queue, copy_task_t* tasks_batch, int batch_count) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->capacity - queue->size < batch_count) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    for (int i = 0; i < batch_count; ++i) {
        queue->tasks[queue->tail] = tasks_batch[i];
        queue->tail = (queue->tail + 1) % queue->capacity;
        ++queue->size;
    }
    
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

int queue_pop(task_queue_t* queue, copy_task_t* out_task) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->shutdown && queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    *out_task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->size;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int queue_pop_batch(task_queue_t* queue, copy_task_t* out_tasks_batch, int max_batch_count) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->shutdown && queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    int pop_count = (queue->size < max_batch_count) ? queue->size : max_batch_count;

    for (int i = 0; i < pop_count; ++i) {
        out_tasks_batch[i] = queue->tasks[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
    }

    queue->size -= pop_count;

    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return pop_count;
}