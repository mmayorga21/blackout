#ifndef BLACKOUT_WORK_QUEUE_H
#define BLACKOUT_WORK_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "sale.h"
#include "route.h"

typedef enum { REQUEST_SALE, REQUEST_STATUS, REQUEST_HISTORY, REQUEST_HELP, REQUEST_REMOTE, REQUEST_PROGRESS, REQUEST_DISCONNECT, REQUEST_CONNECT, REQUEST_CLOSE, REQUEST_SYNC, REQUEST_CHECKPOINT, REQUEST_EXIT, REQUEST_QUERY } request_kind;
typedef struct {
    request_kind kind;
    int64_t amount;
    char sale_csv[SALE_CSV_SIZE]; /* REQUEST_REMOTE: venta completa con CRC. */
    char sale_id[SALE_ID_SIZE];   /* REQUEST_PROGRESS: identidad, sin CSV. */
    uint64_t connection;
    unsigned int connection_slot;
    char sender[33];
    int normal;
    char route_wire[ROUTE_WIRE_SIZE];
    char scope[2048];
    size_t target;
    uint64_t route_version;
    char progress; /* A/S: avance del origen; O: conciliación durante cierre. */
} work_request;
typedef enum { QUEUE_RUNNING, QUEUE_STOPPED, QUEUE_ERROR } queue_stop_reason;

typedef struct {
    work_request *items;
    size_t capacity, head, count;
    int input_closed;
    queue_stop_reason stop;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} work_queue;

int queue_init(work_queue *queue, size_t capacity);
/* 0: encolado; 1: recepción cerrada o detenida. Espera espacio sin espera activa. */
int queue_put(work_queue *queue, work_request request);
/* 0 encolado; 1 cerrada; 2 llena: la red conserva la solicitud y sigue atendiendo envíos. */
int queue_try_put(work_queue *queue, work_request request);
/* 0: entrega solicitud y estado; 1: no quedan solicitudes y la recepción terminó. */
int queue_take(work_queue *queue, work_request *request, queue_stop_reason *reason);
void queue_finish(work_queue *queue);
void queue_stop(work_queue *queue, queue_stop_reason reason);
queue_stop_reason queue_reason(work_queue *queue);
void queue_destroy(work_queue *queue);

#endif
