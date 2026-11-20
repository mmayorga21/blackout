#include "work_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Con objetos inicializados, estos fallos indican un error interno irrecuperable. */
static void checked(int result)
{
    if (result != 0) {
        fprintf(stderr, "Error interno de sincronización: %s\n", strerror(result));
        exit(EXIT_FAILURE);
    }
}

int queue_init(work_queue *queue, size_t capacity)
{
    *queue = (work_queue){0};
    if (!capacity || capacity > SIZE_MAX / sizeof *queue->items) { errno = EINVAL; return -1; }
    queue->items = calloc(capacity, sizeof *queue->items);
    if (queue->items == NULL) return -1;
    int result = pthread_mutex_init(&queue->mutex, NULL);
    if (result != 0) goto free_items;
    result = pthread_cond_init(&queue->not_empty, NULL);
    if (result != 0) goto destroy_mutex;
    result = pthread_cond_init(&queue->not_full, NULL);
    if (result != 0) goto destroy_empty;
    queue->capacity = capacity;
    return 0;

destroy_empty:
    checked(pthread_cond_destroy(&queue->not_empty));
destroy_mutex:
    checked(pthread_mutex_destroy(&queue->mutex));
free_items:
    free(queue->items);
    queue->items = NULL;
    errno = result;
    return -1;
}

int queue_put(work_queue *queue, work_request request)
{
    checked(pthread_mutex_lock(&queue->mutex));
    while (queue->count == queue->capacity && !queue->input_closed && queue->stop == QUEUE_RUNNING)
        checked(pthread_cond_wait(&queue->not_full, &queue->mutex));
    if (queue->input_closed || queue->stop != QUEUE_RUNNING) {
        checked(pthread_mutex_unlock(&queue->mutex));
        return 1;
    }
    size_t tail = (queue->head + queue->count) % queue->capacity;
    queue->items[tail] = request;
    queue->count++;
    checked(pthread_cond_signal(&queue->not_empty));
    checked(pthread_mutex_unlock(&queue->mutex));
    return 0;
}

int queue_take(work_queue *queue, work_request *request, queue_stop_reason *reason)
{
    checked(pthread_mutex_lock(&queue->mutex));
    while (!queue->count && !queue->input_closed && queue->stop == QUEUE_RUNNING)
        checked(pthread_cond_wait(&queue->not_empty, &queue->mutex));
    *reason = queue->stop;
    if (!queue->count) {
        checked(pthread_mutex_unlock(&queue->mutex));
        return 1;
    }
    *request = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    checked(pthread_cond_signal(&queue->not_full));
    checked(pthread_mutex_unlock(&queue->mutex));
    return 0;
}

void queue_finish(work_queue *queue)
{
    checked(pthread_mutex_lock(&queue->mutex));
    queue->input_closed = 1;
    checked(pthread_cond_broadcast(&queue->not_empty));
    checked(pthread_cond_broadcast(&queue->not_full));
    checked(pthread_mutex_unlock(&queue->mutex));
}

void queue_stop(work_queue *queue, queue_stop_reason reason)
{
    checked(pthread_mutex_lock(&queue->mutex));
    /* Un fallo tiene prioridad sobre el límite; una detención nunca se revierte. */
    if (reason > queue->stop) queue->stop = reason;
    checked(pthread_cond_broadcast(&queue->not_empty));
    checked(pthread_cond_broadcast(&queue->not_full));
    checked(pthread_mutex_unlock(&queue->mutex));
}

queue_stop_reason queue_reason(work_queue *queue)
{
    checked(pthread_mutex_lock(&queue->mutex));
    queue_stop_reason reason = queue->stop;
    checked(pthread_mutex_unlock(&queue->mutex));
    return reason;
}

void queue_destroy(work_queue *queue)
{
    /* Solo después de que productores y consumidor hayan terminado. */
    checked(pthread_cond_destroy(&queue->not_full));
    checked(pthread_cond_destroy(&queue->not_empty));
    checked(pthread_mutex_destroy(&queue->mutex));
    free(queue->items);
    queue->items = NULL;
}
